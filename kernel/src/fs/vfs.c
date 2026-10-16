#include <stddef.h>
#include <stdbool.h>
#include "vfs.h"
#include "vfs_internal.h"

struct mount {
    bool used, persistent, read_only;
    char path[VFS_PATH_MAX + 1U];
    size_t path_length;
    const struct vfs_backend_ops *ops;
    const struct vfs_mount_ops *mount_ops;
    const char *filesystem, *source;
    uint32_t open_handles;
};
static struct mount mounts[VFS_MOUNT_MAX];   /* index 0 is the root RAMFS */
static bool frozen;
#define backend (mounts[0].ops)               /* root backend; also the "initialized" flag */
#define HANDLE_MOUNT_SHIFT 24U
#define HANDLE_SLOT_MASK 0x00ffffffU

bool vfs_init(void) {
    if (mounts[0].used || !ramfs_init()) return false;
    mounts[0] = (struct mount){.used = true, .path = "/", .path_length = 1, .ops = ramfs_backend(),
                               .filesystem = "RAMFS", .source = "memory", .persistent = false};
    return mounts[0].ops != 0;
}
void vfs_freeze(bool value) { frozen = value; }
bool vfs_frozen(void) { return frozen; }

static size_t bounded_length(const char *text, size_t maximum) {
    size_t length = 0;
    if (!text) return maximum + 1;
    while (length <= maximum && text[length]) ++length;
    return length;
}

static bool valid_component(const char *part, size_t length) {
    if (!length || length > VFS_NAME_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)part[i];
        if (c < 32 || c == '/' || c == 127) return false;
    }
    return true;
}

static enum vfs_error append_component(char *output, size_t *length,
                                       size_t capacity, const char *part,
                                       size_t part_length) {
    if (!valid_component(part, part_length)) return VFS_INVALID_PATH;
    size_t separator = *length > 1 ? 1 : 0;
    if (*length + separator + part_length + 1 > capacity ||
        *length + separator + part_length > VFS_PATH_MAX)
        return VFS_INVALID_PATH;
    if (separator) output[(*length)++] = '/';
    for (size_t i = 0; i < part_length; ++i) output[(*length)++] = part[i];
    output[*length] = 0;
    return VFS_OK;
}

static void pop_component(char *output, size_t *length) {
    if (*length <= 1) return;
    while (*length > 1 && output[*length - 1] != '/') --*length;
    if (*length > 1) --*length;
    output[*length] = 0;
}

enum vfs_error vfs_normalize(const char *path, const char *cwd,
                             char *output, size_t capacity) {
    if (!backend || !path || !output || capacity < 2 || !path[0])
        return VFS_INVALID_PATH;
    size_t path_length = bounded_length(path, VFS_PATH_MAX);
    if (path_length > VFS_PATH_MAX) return VFS_INVALID_PATH;
    output[0] = '/'; output[1] = 0;
    size_t length = 1;
    if (path[0] != '/') {
        if (!cwd || cwd[0] != '/') return VFS_INVALID_PATH;
        size_t cwd_length = bounded_length(cwd, VFS_PATH_MAX);
        if (cwd_length > VFS_PATH_MAX) return VFS_INVALID_PATH;
        for (size_t i = 1; i <= cwd_length;) {
            size_t start = i;
            while (i < cwd_length && cwd[i] != '/') ++i;
            if (i > start) {
                enum vfs_error error = append_component(output, &length,
                    capacity, cwd + start, i - start);
                if (error != VFS_OK) return error;
            }
            ++i;
        }
    }
    for (size_t i = path[0] == '/' ? 1 : 0; i <= path_length;) {
        size_t start = i;
        while (i < path_length && path[i] != '/') ++i;
        size_t component_length = i - start;
        if (component_length == 1 && path[start] == '.') { }
        else if (component_length == 2 && path[start] == '.' && path[start + 1] == '.')
            pop_component(output, &length);
        else if (component_length) {
            enum vfs_error error = append_component(output, &length,
                capacity, path + start, component_length);
            if (error != VFS_OK) return error;
        }
        ++i;
    }
    return VFS_OK;
}

/* Backend entry points accept canonical absolute paths only. Normalization
   remains a lexical utility; user actions use resolve_path to check traversal. */
static bool canonical(const char *path) {
    size_t n = bounded_length(path, VFS_PATH_MAX);
    if (!path || n > VFS_PATH_MAX || !n || path[0] != '/') return false;
    if (n == 1) return true;
    for (size_t i = 1; i <= n;) {
        size_t start = i;
        while (i < n && path[i] != '/') ++i;
        size_t count = i - start;
        if (!valid_component(path + start, count) ||
            (count == 1 && path[start] == '.') ||
            (count == 2 && path[start] == '.' && path[start+1] == '.')) return false;
        ++i;
    }
    return true;
}

/* ---- mount routing ---- */
static size_t text_length(const char *text) { size_t n = 0; while (text[n]) ++n; return n; }
static bool covers(const char *prefix, size_t prefix_length, const char *path) {
    for (size_t i = 0; i < prefix_length; ++i) if (path[i] != prefix[i]) return false;
    return path[prefix_length] == 0 || path[prefix_length] == '/';
}
/* Longest mount-point prefix wins; *relative is the path inside that mount. */
static unsigned route(const char *path, const char **relative) {
    unsigned best = 0; size_t best_length = 1;
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i)
        if (mounts[i].used && mounts[i].path_length >= best_length && covers(mounts[i].path, mounts[i].path_length, path)) {
            best = i; best_length = mounts[i].path_length;
        }
    if (best == 0) { *relative = path; return 0; }
    *relative = path[best_length] ? path + best_length : "/";
    return best;
}
bool vfs_is_mount_point(const char *path) {
    if (!canonical(path)) return false;
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i)
        if (mounts[i].used && text_length(path) == mounts[i].path_length && covers(mounts[i].path, mounts[i].path_length, path)) return true;
    return false;
}
/* True when `path` is a mount point or an ancestor of one. */
static bool covers_mount(const char *path) {
    size_t n = text_length(path);
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i)
        if (mounts[i].used && n <= mounts[i].path_length && covers(path, n, mounts[i].path)) return true;
    return false;
}
static bool mount_valid(unsigned index) { return index < VFS_MOUNT_MAX && mounts[index].used; }

enum vfs_error vfs_create_file(const char *path) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (frozen) return VFS_BUSY;
    const char *relative; unsigned m = route(path, &relative);
    return mounts[m].ops->create_file(relative);
}
enum vfs_error vfs_create_directory(const char *path) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (frozen) return VFS_BUSY;
    const char *relative; unsigned m = route(path, &relative);
    return mounts[m].ops->create_directory(relative);
}
enum vfs_error vfs_unlink(const char *path) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (frozen) return VFS_BUSY;
    if (covers_mount(path)) return VFS_BUSY;      /* a mount point or its ancestor */
    const char *relative; unsigned m = route(path, &relative);
    return mounts[m].ops->unlink(relative);
}
bool vfs_is_directory(const char *path) {
    if (!backend || !canonical(path)) return false;
    const char *relative; unsigned m = route(path, &relative);
    return mounts[m].ops->is_directory(relative);
}
static enum vfs_error tag_handle(enum vfs_error error, uint32_t *slot, uint32_t generation, unsigned m) {
    if (error == VFS_OK && generation) { *slot |= (uint32_t)m << HANDLE_MOUNT_SHIFT; ++mounts[m].open_handles; }
    return error;
}
enum vfs_error vfs_file_open(const char *path, uint32_t flags, struct vfs_file *out) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (!out || out->slot || out->generation) return VFS_BAD_HANDLE;
    if (frozen && (flags & VFS_OPEN_WRITE)) return VFS_BUSY;
    const char *relative; unsigned m = route(path, &relative);
    enum vfs_error error = mounts[m].ops->file_open(relative, flags, out);
    return tag_handle(error, &out->slot, out->generation, m);
}
/* Run `call` with the mount index stripped from the handle, then restore it. */
#define WITH_FILE(file, call, close_op) do { \
    if (!backend || !(file)) return VFS_BAD_HANDLE; \
    unsigned m_ = (file)->slot >> HANDLE_MOUNT_SHIFT; \
    if (!mount_valid(m_)) return VFS_BAD_HANDLE; \
    (file)->slot &= HANDLE_SLOT_MASK; \
    enum vfs_error e_ = mounts[m_].ops->call; \
    if (close_op && e_ == VFS_OK) { if (mounts[m_].open_handles) --mounts[m_].open_handles; } \
    else if ((file)->generation) (file)->slot |= (uint32_t)m_ << HANDLE_MOUNT_SHIFT; \
    return e_; } while (0)
enum vfs_error vfs_file_close(struct vfs_file *file) { WITH_FILE(file, file_close(file), 1); }
enum vfs_error vfs_file_read(struct vfs_file *file, void *buffer, size_t capacity, size_t *bytes_read) {
    WITH_FILE(file, file_read(file, buffer, capacity, bytes_read), 0);
}
enum vfs_error vfs_file_write(struct vfs_file *file, const void *buffer, size_t length, size_t *bytes_written) {
    if (frozen) { if (bytes_written) *bytes_written = 0; return VFS_BUSY; }
    WITH_FILE(file, file_write(file, buffer, length, bytes_written), 0);
}
enum vfs_error vfs_dir_open(const char *path, struct vfs_dir *out) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (!out || out->slot || out->generation) return VFS_BAD_HANDLE;
    const char *relative; unsigned m = route(path, &relative);
    enum vfs_error error = mounts[m].ops->dir_open(relative, out);
    return tag_handle(error, &out->slot, out->generation, m);
}
#define WITH_DIR(dir, call, close_op) do { \
    if (!backend || !(dir)) return VFS_BAD_HANDLE; \
    unsigned m_ = (dir)->slot >> HANDLE_MOUNT_SHIFT; \
    if (!mount_valid(m_)) return VFS_BAD_HANDLE; \
    (dir)->slot &= HANDLE_SLOT_MASK; \
    enum vfs_error e_ = mounts[m_].ops->call; \
    if (close_op && e_ == VFS_OK) { if (mounts[m_].open_handles) --mounts[m_].open_handles; } \
    else if ((dir)->generation) (dir)->slot |= (uint32_t)m_ << HANDLE_MOUNT_SHIFT; \
    return e_; } while (0)
enum vfs_error vfs_dir_next(struct vfs_dir *dir, char *name, size_t capacity, enum vfs_node_type *type) {
    WITH_DIR(dir, dir_next(dir, name, capacity, type), 0);
}
enum vfs_error vfs_dir_close(struct vfs_dir *dir) { WITH_DIR(dir, dir_close(dir), 1); }

enum vfs_error vfs_stat(const char *path, struct vfs_info *out) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    const char *relative; unsigned m = route(path, &relative);
    enum vfs_error error = mounts[m].ops->stat(relative, out);
    if (error == VFS_OK && out) { out->filesystem = mounts[m].filesystem; out->persistent = mounts[m].persistent; }
    return error;
}

/* Check every traversed directory, including components later canceled by '..'.
   A missing/non-directory prefix must never redirect an action to another item. */
enum vfs_error vfs_resolve_path(const char *path, const char *cwd,
                                 char *out, size_t capacity) {
    size_t n = bounded_length(path, VFS_PATH_MAX);
    if (!path || !n || n > VFS_PATH_MAX || !out || capacity < 2 || !canonical(cwd))
        return VFS_INVALID_PATH;
    enum vfs_error error = vfs_normalize(path[0] == '/' ? "/" : cwd, "/", out, capacity);
    if (error != VFS_OK) return error;
    size_t used = bounded_length(out, VFS_PATH_MAX);
    for (size_t i = path[0] == '/' ? 1 : 0; i < n;) {
        while (i < n && path[i] == '/') ++i;
        if (i == n) break;
        struct vfs_info info;
        error = vfs_stat(out, &info);
        if (error != VFS_OK) return error;
        if (info.type != VFS_NODE_DIRECTORY) return VFS_NOT_DIRECTORY;
        size_t start = i;
        while (i < n && path[i] != '/') ++i;
        size_t count = i - start;
        if (!valid_component(path + start, count)) return VFS_INVALID_PATH;
        if (count == 1 && path[start] == '.') { }
        else if (count == 2 && path[start] == '.' && path[start+1] == '.') pop_component(out, &used);
        else {
            error = append_component(out, &used, capacity, path + start, count);
            if (error != VFS_OK) return error;
        }
    }
    if (path[n-1] == '/' && !vfs_is_directory(out)) return VFS_NOT_DIRECTORY;
    return VFS_OK;
}
/* Chunked copy through the public API: used across mounts and for backends
   without their own copy. Fails without leaving a partial destination. */
static uint8_t copy_buffer[1024];
static enum vfs_error generic_copy(const char *source, const char *destination) {
    struct vfs_info info;
    enum vfs_error error = vfs_stat(source, &info);
    if (error != VFS_OK) return error;
    if (info.type != VFS_NODE_FILE) return VFS_IS_DIRECTORY;
    error = vfs_stat(destination, &info);
    if (error == VFS_OK) return VFS_EXISTS;
    if (error != VFS_NOT_FOUND) return error;
    error = vfs_create_file(destination);
    if (error != VFS_OK) return error;
    struct vfs_file in = {0}, out = {0};
    error = vfs_file_open(source, VFS_OPEN_READ, &in);
    if (error == VFS_OK) error = vfs_file_open(destination, VFS_OPEN_WRITE, &out);
    while (error == VFS_OK) {
        size_t got = 0, put = 0;
        error = vfs_file_read(&in, copy_buffer, sizeof(copy_buffer), &got);
        if (error != VFS_OK || !got) break;
        error = vfs_file_write(&out, copy_buffer, got, &put);
        if (error == VFS_OK && put != got) error = VFS_NO_SPACE;
    }
    if (in.generation) { enum vfs_error c = vfs_file_close(&in); if (error == VFS_OK) error = c; }
    if (out.generation) { enum vfs_error c = vfs_file_close(&out); if (error == VFS_OK) error = c; }
    if (error != VFS_OK) (void)vfs_unlink(destination);   /* best-effort rollback */
    return error;
}
enum vfs_error vfs_copy(const char *source, const char *destination) {
    if (!canonical(source) || !canonical(destination)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (frozen) return VFS_BUSY;
    const char *rs, *rd; unsigned ms = route(source, &rs), md = route(destination, &rd);
    if (ms == md && mounts[ms].ops->copy) return mounts[ms].ops->copy(rs, rd);
    return generic_copy(source, destination);
}
enum vfs_error vfs_rename(const char *source, const char *destination) {
    if (!canonical(source) || !canonical(destination)) return VFS_INVALID_PATH;
    if (!backend) return VFS_BAD_HANDLE;
    if (frozen) return VFS_BUSY;
    if (covers_mount(source)) return VFS_BUSY;
    const char *rs, *rd; unsigned ms = route(source, &rs), md = route(destination, &rd);
    if (ms != md) return VFS_UNSUPPORTED;   /* cross-mount move: copy, then remove */
    return mounts[ms].ops->rename(rs, rd);
}
enum vfs_error vfs_remove_tree_observe(const char *path, struct vfs_remove_report *report,
                                      void (*removed)(const char *path)) {
    if (!report) return VFS_BAD_HANDLE;
    report->removed = 0; report->remaining[0] = 0;
    if (!canonical(path) || !path[1]) return VFS_INVALID_PATH;
    size_t base = bounded_length(path, VFS_PATH_MAX);
    char current[VFS_PATH_MAX + 1U];
    for (size_t i = 0; i <= base; ++i) current[i] = path[i];
    /* Iterative postorder walk: open one iterator, select one child, close it
       before mutating. One path buffer, constant stack, at most 3N steps. */
    enum vfs_error error = VFS_UNSUPPORTED;
    for (size_t steps = 0; steps < VFS_NODE_MAX * 3U; ++steps) {
        struct vfs_info info;
        error = vfs_stat(current, &info);
        if (error != VFS_OK) break;
        if (info.type == VFS_NODE_DIRECTORY) {
            if (covers_mount(current)) { error = VFS_BUSY; break; }   /* never cross or remove a mount */
            struct vfs_dir dir = {0};
            error = vfs_dir_open(current, &dir);
            if (error != VFS_OK) break;
            char name[VFS_NAME_MAX + 1U]; enum vfs_node_type type;
            error = vfs_dir_next(&dir, name, sizeof(name), &type);
            enum vfs_error closed = vfs_dir_close(&dir);
            if (closed != VFS_OK) { error = closed; break; }
            if (error == VFS_OK) {
                size_t count = bounded_length(current, VFS_PATH_MAX);
                error = append_component(current, &count, sizeof(current), name,
                                          bounded_length(name, VFS_NAME_MAX));
                if (error != VFS_OK) break;
                continue;
            }
            if (error != VFS_END) break;
        }
        error = vfs_unlink(current);
        if (error != VFS_OK) break;
        ++report->removed;
        if (removed) removed(current);
        size_t count = bounded_length(current, VFS_PATH_MAX);
        if (count == base) return VFS_OK;
        pop_component(current, &count);
        error = VFS_UNSUPPORTED;
    }
    for (size_t i = 0; i <= VFS_PATH_MAX; ++i) {
        report->remaining[i] = current[i]; if (!current[i]) break;
    }
    return error;
}

enum vfs_error vfs_remove_tree(const char *path, struct vfs_remove_report *report) {
    return vfs_remove_tree_observe(path, report, 0);
}

/* ---- mount management ---- */
enum vfs_error vfs_mount(const char *path, const struct vfs_backend_ops *ops,
                         const struct vfs_mount_ops *mount_ops, const char *filesystem,
                         const char *source, bool persistent, bool read_only) {
    if (!backend || !ops || !filesystem) return VFS_BAD_HANDLE;
    if (!canonical(path) || !path[1]) return VFS_INVALID_PATH;    /* the root is not replaceable */
    if (frozen) return VFS_BUSY;
    const char *relative;
    if (route(path, &relative) != 0 || covers_mount(path)) return VFS_BUSY;   /* no nesting or stacking */
    if (!backend->is_directory(path)) return backend->stat(path, &(struct vfs_info){0}) == VFS_OK ? VFS_NOT_DIRECTORY : VFS_NOT_FOUND;
    /* The mount point must be empty so no hidden content appears to vanish. */
    struct vfs_dir dir = {0};
    enum vfs_error error = backend->dir_open(path, &dir);
    if (error != VFS_OK) return error;
    char name[VFS_NAME_MAX + 1U]; enum vfs_node_type type;
    enum vfs_error next = backend->dir_next(&dir, name, sizeof(name), &type);
    (void)backend->dir_close(&dir);
    if (next == VFS_OK) return VFS_NOT_EMPTY;
    if (next != VFS_END) return next;
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i) {
        if (mounts[i].used) continue;
        mounts[i] = (struct mount){.used = true, .ops = ops, .mount_ops = mount_ops, .filesystem = filesystem,
                                   .source = source, .persistent = persistent, .read_only = read_only};
        size_t n = text_length(path);
        for (size_t k = 0; k <= n; ++k) mounts[i].path[k] = path[k];
        mounts[i].path_length = n;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
enum vfs_error vfs_unmount(const char *path) {
    if (!canonical(path)) return VFS_INVALID_PATH;
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i) {
        if (!mounts[i].used || text_length(path) != mounts[i].path_length || !covers(mounts[i].path, mounts[i].path_length, path)) continue;
        if (mounts[i].open_handles) return VFS_BUSY;
        enum vfs_error error = mounts[i].mount_ops && mounts[i].mount_ops->release ? mounts[i].mount_ops->release() : VFS_OK;
        /* A failed release keeps the mount (pending changes are never dropped)
           unless the backend reports it is gone, as for a faulted volume. */
        if (error != VFS_OK && (!mounts[i].mount_ops || !mounts[i].mount_ops->mounted || mounts[i].mount_ops->mounted())) return error;
        mounts[i] = (struct mount){0};
        return error;
    }
    return VFS_NOT_FOUND;
}
size_t vfs_mount_count(void) {
    size_t n = 0;
    for (unsigned i = 0; i < VFS_MOUNT_MAX; ++i) n += mounts[i].used;
    return n;
}
bool vfs_mount_info_at(size_t index, struct vfs_mount_info *out) {
    if (!out) return false;
    for (unsigned i = 0; i < VFS_MOUNT_MAX; ++i) {
        if (!mounts[i].used || index--) continue;
        for (size_t k = 0; k <= mounts[i].path_length; ++k) out->path[k] = mounts[i].path[k];
        out->filesystem = mounts[i].filesystem; out->source = mounts[i].source;
        out->persistent = mounts[i].persistent; out->read_only = mounts[i].read_only;
        out->open_handles = mounts[i].open_handles;
        return true;
    }
    return false;
}
enum vfs_error vfs_sync_all(size_t *synced) {
    if (synced) *synced = 0;
    enum vfs_error first = VFS_OK;
    for (unsigned i = 1; i < VFS_MOUNT_MAX; ++i) {
        if (!mounts[i].used || !mounts[i].mount_ops || !mounts[i].mount_ops->sync) continue;
        enum vfs_error error = mounts[i].mount_ops->sync();
        if (error == VFS_OK) { if (synced) ++*synced; }
        else if (first == VFS_OK) first = error;
    }
    return first;
}
