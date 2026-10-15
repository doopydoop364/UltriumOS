#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "vfs.h"
#include "vfs_internal.h"
#include "memory/heap.h"
#include "memory.h"

struct vfs_node {
    char name[VFS_NAME_MAX + 1U];
    enum vfs_node_type type;
    struct vfs_node *parent;
    struct vfs_node *children;
    struct vfs_node *next;
    uint8_t *data;
    size_t length;
    size_t capacity;
    uint32_t open_handles;
};

struct file_state {
    uint32_t generation;
    bool active;
    bool retired;
    struct vfs_node *node;
    size_t offset;
    uint32_t flags;
};

struct dir_state {
    uint32_t generation;
    bool active;
    bool retired;
    struct vfs_node *node;
    struct vfs_node *next;
};

static struct vfs_node *root;
static struct file_state file_slots[VFS_HANDLE_SLOTS];
static struct dir_state dir_slots[VFS_HANDLE_SLOTS];
static size_t node_count;
static size_t allocated_data_capacity;
static int32_t fail_after = -1;

static void *fs_alloc(size_t size) {
    if (fail_after == 0) {
        fail_after = -1;
        return 0;
    }
    if (fail_after > 0) --fail_after;
    return kmalloc(size);
}

void vfs_test_fail_alloc_after(int32_t successful_allocations) {
    fail_after = successful_allocations < 0 ? -1 : successful_allocations;
}

static size_t string_length(const char *s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) ++n;
    return n;
}

static bool copy_name(char *out, const char *name, size_t length) {
    if (!length || length > VFS_NAME_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == '/' || c == 127) return false;
        out[i] = (char)c;
    }
    out[length] = 0;
    return true;
}

bool ramfs_init(void) {
    if (root) return false;
    struct vfs_node *node = fs_alloc(sizeof(*node));
    if (!node) return false;
    memset(node, 0, sizeof(*node));
    node->type = VFS_NODE_DIRECTORY;
    root = node;
    node_count = 1;
    return true;
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name);

static enum vfs_error resolve(const char *path, struct vfs_node **out) {
    if (!root || !path || path[0] != '/' || !out) return VFS_INVALID_PATH;
    *out = 0;
    if (path[1] == 0) { *out = root; return VFS_OK; }
    struct vfs_node *node = root;
    size_t i = 1;
    while (path[i]) {
        if (node->type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
        size_t start = i;
        while (path[i] && path[i] != '/') ++i;
        size_t length = i - start;
        char name[VFS_NAME_MAX + 1U];
        if (!copy_name(name, path + start, length)) return VFS_INVALID_PATH;
        struct vfs_node *child = find_child(node, name);
        if (!child) return VFS_NOT_FOUND;
        node = child;
        if (path[i] == '/') {
            ++i;
            if (!path[i]) return VFS_INVALID_PATH;
        }
    }
    *out = node;
    return VFS_OK;
}

static enum vfs_error resolve_parent(const char *path, struct vfs_node **parent,
                                     char name[VFS_NAME_MAX + 1U]) {
    if (!path || path[0] != '/') return VFS_INVALID_PATH;
    size_t length = string_length(path);
    if (length < 2 || length > VFS_PATH_MAX || path[length - 1] == '/')
        return VFS_INVALID_PATH;
    size_t start = length;
    while (start > 0 && path[start - 1] != '/') --start;
    if (!copy_name(name, path + start, length - start)) return VFS_INVALID_PATH;
    if (start == 1) { *parent = root; return root ? VFS_OK : VFS_INVALID_PATH; }
    char parent_path[VFS_PATH_MAX + 1U];
    size_t parent_length = start - 1;
    for (size_t i = 0; i < parent_length; ++i) parent_path[i] = path[i];
    parent_path[parent_length] = 0;
    return resolve(parent_path, parent);
}

static struct vfs_node *find_child(struct vfs_node *parent, const char *name) {
    for (struct vfs_node *node = parent->children; node; node = node->next) {
        size_t a = string_length(node->name), b = string_length(name);
        if (a != b) continue;
        size_t i = 0;
        while (i < a && node->name[i] == name[i]) ++i;
        if (i == a) return node;
    }
    return 0;
}

static struct file_state *get_file(struct vfs_file handle) {
    if (!handle.generation || handle.slot >= VFS_HANDLE_SLOTS) return 0;
    struct file_state *state = &file_slots[handle.slot];
    return state->active && state->generation == handle.generation ? state : 0;
}

static struct dir_state *get_dir(struct vfs_dir handle) {
    if (!handle.generation || handle.slot >= VFS_HANDLE_SLOTS) return 0;
    struct dir_state *state = &dir_slots[handle.slot];
    return state->active && state->generation == handle.generation ? state : 0;
}

static struct file_state *allocate_file(uint32_t *slot) {
    for (uint32_t i = 0; i < VFS_HANDLE_SLOTS; ++i) {
        struct file_state *state = &file_slots[i];
        if (state->active || state->retired) continue;
        if (!state->generation) state->generation = 1;
        *slot = i;
        return state;
    }
    return 0;
}

static struct dir_state *allocate_dir(uint32_t *slot) {
    for (uint32_t i = 0; i < VFS_HANDLE_SLOTS; ++i) {
        struct dir_state *state = &dir_slots[i];
        if (state->active || state->retired) continue;
        if (!state->generation) state->generation = 1;
        *slot = i;
        return state;
    }
    return 0;
}

static enum vfs_error create_node(const char *path, enum vfs_node_type type) {
    struct vfs_node *parent;
    char name[VFS_NAME_MAX + 1U];
    enum vfs_error error = resolve_parent(path, &parent, name);
    if (error != VFS_OK) return error;
    if (parent->type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
    if (parent->open_handles) return VFS_UNSUPPORTED;
    if (find_child(parent, name)) return VFS_EXISTS;
    if (node_count >= VFS_NODE_MAX) return VFS_NO_SPACE;
    struct vfs_node *node = fs_alloc(sizeof(*node));
    if (!node) return VFS_NO_SPACE;
    memset(node, 0, sizeof(*node));
    size_t length = string_length(name);
    for (size_t i = 0; i <= length; ++i) node->name[i] = name[i];
    node->type = type;
    node->parent = parent;
    struct vfs_node **tail = &parent->children;
    while (*tail) tail = &(*tail)->next;
    *tail = node;
    ++node_count;
    return VFS_OK;
}

static enum vfs_error ramfs_create_file(const char *path) {
    return create_node(path, VFS_NODE_FILE);
}

static enum vfs_error ramfs_create_directory(const char *path) {
    return create_node(path, VFS_NODE_DIRECTORY);
}

static bool ramfs_is_directory(const char *path) {
    struct vfs_node *node;
    return resolve(path, &node) == VFS_OK && node->type == VFS_NODE_DIRECTORY;
}

static enum vfs_error ramfs_unlink(const char *path) {
    struct vfs_node *parent;
    char name[VFS_NAME_MAX + 1U];
    enum vfs_error error = resolve_parent(path, &parent, name);
    if (error != VFS_OK) return error;
    if (parent->type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
    if (parent->open_handles) return VFS_UNSUPPORTED;
    struct vfs_node *target = find_child(parent, name);
    if (!target) return VFS_NOT_FOUND;
    struct vfs_node **link = &parent->children;
    while (*link && *link != target) link = &(*link)->next;
    if (!*link) return VFS_NOT_FOUND;
    struct vfs_node *node = *link;
    if (node->open_handles) return VFS_UNSUPPORTED;
    if (node->type == VFS_NODE_DIRECTORY && node->children) return VFS_NOT_EMPTY;
    *link = node->next;
    if (node->data) kfree(node->data);
    allocated_data_capacity -= node->capacity;
    kfree(node);
    --node_count;
    return VFS_OK;
}

static enum vfs_error ramfs_file_open(const char *path, uint32_t flags,
                                      struct vfs_file *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    out->slot = out->generation = 0;
    if (!flags || (flags & ~(VFS_OPEN_READ | VFS_OPEN_WRITE))) return VFS_BAD_HANDLE;
    struct vfs_node *node;
    enum vfs_error error = resolve(path, &node);
    if (error != VFS_OK) return error;
    if (node->type != VFS_NODE_FILE) return VFS_IS_DIRECTORY;
    uint32_t slot;
    struct file_state *file = allocate_file(&slot);
    if (!file) return VFS_NO_SPACE;
    file->node = node; file->offset = 0; file->flags = flags; file->active = true;
    ++node->open_handles;
    out->slot = slot; out->generation = file->generation;
    return VFS_OK;
}

static enum vfs_error ramfs_file_close(struct vfs_file *file) {
    if (!file) return VFS_BAD_HANDLE;
    struct file_state *state = get_file(*file);
    if (!state || !state->node || !state->node->open_handles) return VFS_BAD_HANDLE;
    --state->node->open_handles;
    state->node = 0; state->offset = 0; state->flags = 0; state->active = false;
    if (state->generation == UINT32_MAX) state->retired = true;
    else ++state->generation;
    file->slot = file->generation = 0;
    return VFS_OK;
}

static enum vfs_error ramfs_file_read(struct vfs_file *file, void *buffer,
                                      size_t capacity, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    if (!file || !bytes_read || (capacity && !buffer))
        return VFS_BAD_HANDLE;
    struct file_state *state = get_file(*file);
    if (!state || !(state->flags & VFS_OPEN_READ)) return VFS_BAD_HANDLE;
    size_t available = state->offset < state->node->length ?
                       state->node->length - state->offset : 0;
    size_t count = capacity < available ? capacity : available;
    if (count) memcpy(buffer, state->node->data + state->offset, count);
    state->offset += count;
    *bytes_read = count;
    return VFS_OK;
}

static enum vfs_error ramfs_file_write(struct vfs_file *file, const void *buffer,
                                       size_t length, size_t *bytes_written) {
    if (bytes_written) *bytes_written = 0;
    if (!file || !bytes_written || (length && !buffer))
        return VFS_BAD_HANDLE;
    struct file_state *state = get_file(*file);
    if (!state || !(state->flags & VFS_OPEN_WRITE)) return VFS_BAD_HANDLE;
    if (state->offset > VFS_FILE_MAX || length > VFS_FILE_MAX - state->offset)
        return VFS_NO_SPACE;
    size_t end = state->offset + length;
    struct vfs_node *node = state->node;
    if (end > node->capacity) {
        size_t capacity = node->capacity ? node->capacity : 64;
        while (capacity < end && capacity < VFS_FILE_MAX) capacity *= 2;
        if (capacity > VFS_FILE_MAX) capacity = VFS_FILE_MAX;
        size_t growth = capacity - node->capacity;
        if (growth > VFS_DATA_MAX - allocated_data_capacity) return VFS_NO_SPACE;
        uint8_t *data = fs_alloc(capacity);
        if (!data) return VFS_NO_SPACE;
        memset(data, 0, capacity);
        if (node->length) memcpy(data, node->data, node->length);
        if (node->data) kfree(node->data);
        node->data = data;
        node->capacity = capacity;
        allocated_data_capacity += growth;
    }
    if (length) memcpy(node->data + state->offset, buffer, length);
    state->offset = end;
    if (end > node->length) node->length = end;
    *bytes_written = length;
    return VFS_OK;
}

static enum vfs_error ramfs_dir_open(const char *path, struct vfs_dir *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    out->slot = out->generation = 0;
    struct vfs_node *node;
    enum vfs_error error = resolve(path, &node);
    if (error != VFS_OK) return error;
    if (node->type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
    uint32_t slot;
    struct dir_state *dir = allocate_dir(&slot);
    if (!dir) return VFS_NO_SPACE;
    dir->node = node; dir->next = node->children; dir->active = true;
    ++node->open_handles;
    out->slot = slot; out->generation = dir->generation;
    return VFS_OK;
}

static enum vfs_error ramfs_dir_next(struct vfs_dir *dir, char *name, size_t capacity,
                                     enum vfs_node_type *type) {
    if (!dir || !name || !capacity || !type) return VFS_BAD_HANDLE;
    struct dir_state *state = get_dir(*dir);
    if (!state) return VFS_BAD_HANDLE;
    if (!state->next) return VFS_END;
    size_t length = string_length(state->next->name);
    if (capacity <= length) return VFS_BUFFER_TOO_SMALL;
    for (size_t i = 0; i <= length; ++i) name[i] = state->next->name[i];
    *type = state->next->type;
    state->next = state->next->next;
    return VFS_OK;
}

static enum vfs_error ramfs_dir_close(struct vfs_dir *dir) {
    if (!dir) return VFS_BAD_HANDLE;
    struct dir_state *state = get_dir(*dir);
    if (!state || !state->node || !state->node->open_handles) return VFS_BAD_HANDLE;
    --state->node->open_handles;
    state->node = 0; state->next = 0; state->active = false;
    if (state->generation == UINT32_MAX) state->retired = true;
    else ++state->generation;
    dir->slot = dir->generation = 0;
    return VFS_OK;
}

static enum vfs_error destination(const char *path, struct vfs_node **parent,
                                    char name[VFS_NAME_MAX + 1U]) {
    enum vfs_error error = resolve_parent(path, parent, name);
    if (error != VFS_OK) return error;
    if ((*parent)->type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
    if ((*parent)->open_handles) return VFS_UNSUPPORTED;
    if (find_child(*parent, name)) return VFS_EXISTS;
    return VFS_OK;
}
static enum vfs_error ramfs_copy(const char *source, const char *target) {
    struct vfs_node *node, *parent;
    enum vfs_error error = resolve(source, &node);
    if (error != VFS_OK) return error;
    if (node->type != VFS_NODE_FILE) return VFS_IS_DIRECTORY;
    char name[VFS_NAME_MAX + 1U];
    error = destination(target, &parent, name);
    if (error != VFS_OK) return error;
    if (node_count == VFS_NODE_MAX || node->capacity > VFS_DATA_MAX - allocated_data_capacity)
        return VFS_NO_SPACE;
    struct vfs_node *copy = fs_alloc(sizeof(*copy));
    if (!copy) return VFS_NO_SPACE;
    memset(copy, 0, sizeof(*copy));
    if (node->capacity) {
        copy->data = fs_alloc(node->capacity);
        if (!copy->data) { kfree(copy); return VFS_NO_SPACE; }
        memset(copy->data, 0, node->capacity);
        if (node->length) memcpy(copy->data, node->data, node->length);
    }
    copy->capacity = node->capacity; copy->length = node->length;
    copy->type = VFS_NODE_FILE; copy->parent = parent;
    copy_name(copy->name, name, string_length(name));
    /* All failure points precede the namespace/accounting commit. */
    struct vfs_node **tail = &parent->children;
    while (*tail) tail = &(*tail)->next;
    *tail = copy; ++node_count; allocated_data_capacity += copy->capacity;
    return VFS_OK;
}
static enum vfs_error ramfs_rename(const char *source, const char *target) {
    struct vfs_node *node, *parent;
    enum vfs_error error = resolve(source, &node);
    if (error != VFS_OK) return error;
    if (node == root) return VFS_INVALID_PATH;
    char name[VFS_NAME_MAX + 1U];
    error = destination(target, &parent, name);
    if (error != VFS_OK) return error;
    if (node->open_handles || node->parent->open_handles) return VFS_UNSUPPORTED;
    for (struct vfs_node *p = parent; p; p = p->parent)
        if (p == node) return VFS_INVALID_PATH;
    /* Check that every descendant remains addressable after relocation.
       Bounded node worklist, no recursive C calls or allocation. */
    struct vfs_node *pending[VFS_NODE_MAX];
    size_t lengths[VFS_NODE_MAX], count = 1;
    pending[0] = node; lengths[0] = string_length(target);
    while (count) {
        --count;
        struct vfs_node *p = pending[count]; size_t n = lengths[count];
        if (n > VFS_PATH_MAX) return VFS_INVALID_PATH;
        for (struct vfs_node *c = p->children; c; c = c->next) {
            if (count == VFS_NODE_MAX) return VFS_NO_SPACE;
            pending[count] = c; lengths[count++] = n + 1 + string_length(c->name);
        }
    }
    struct vfs_node **link = &node->parent->children;
    while (*link && *link != node) link = &(*link)->next;
    if (!*link) return VFS_NOT_FOUND;
    *link = node->next;
    node->next = 0; node->parent = parent;
    copy_name(node->name, name, string_length(name));
    struct vfs_node **tail = &parent->children;
    while (*tail) tail = &(*tail)->next;
    *tail = node;
    return VFS_OK;
}

static enum vfs_error ramfs_stat(const char *path, struct vfs_info *out) {
    if (!out) return VFS_BAD_HANDLE;
    struct vfs_node *node;
    enum vfs_error error = resolve(path, &node);
    if (error != VFS_OK) return error;
    out->type = node->type;
    out->size = node->length;
    return VFS_OK;
}

const struct vfs_backend_ops *ramfs_backend(void) {
    static const struct vfs_backend_ops operations = {
        .copy = ramfs_copy,
        .rename = ramfs_rename,
        .stat = ramfs_stat,
        .create_file = ramfs_create_file,
        .create_directory = ramfs_create_directory,
        .unlink = ramfs_unlink,
        .is_directory = ramfs_is_directory,
        .file_open = ramfs_file_open,
        .file_close = ramfs_file_close,
        .file_read = ramfs_file_read,
        .file_write = ramfs_file_write,
        .dir_open = ramfs_dir_open,
        .dir_next = ramfs_dir_next,
        .dir_close = ramfs_dir_close
    };
    return &operations;
}
