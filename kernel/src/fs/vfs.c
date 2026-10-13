#include <stddef.h>
#include <stdbool.h>
#include "vfs.h"
#include "vfs_internal.h"

static const struct vfs_backend_ops *backend;

bool vfs_init(void) {
    if (backend || !ramfs_init()) return false;
    backend = ramfs_backend();
    return backend != 0;
}

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

enum vfs_error vfs_create_file(const char *path) {
    return backend ? backend->create_file(path) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_create_directory(const char *path) {
    return backend ? backend->create_directory(path) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_unlink(const char *path) {
    return backend ? backend->unlink(path) : VFS_BAD_HANDLE;
}
bool vfs_is_directory(const char *path) {
    return backend && backend->is_directory(path);
}
enum vfs_error vfs_file_open(const char *path, uint32_t flags, struct vfs_file *out) {
    return backend ? backend->file_open(path, flags, out) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_file_close(struct vfs_file *file) {
    return backend ? backend->file_close(file) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_file_read(struct vfs_file *file, void *buffer,
                             size_t capacity, size_t *bytes_read) {
    return backend ? backend->file_read(file, buffer, capacity, bytes_read) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_file_write(struct vfs_file *file, const void *buffer,
                              size_t length, size_t *bytes_written) {
    return backend ? backend->file_write(file, buffer, length, bytes_written) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_dir_open(const char *path, struct vfs_dir *out) {
    return backend ? backend->dir_open(path, out) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_dir_next(struct vfs_dir *dir, char *name, size_t capacity,
                            enum vfs_node_type *type) {
    return backend ? backend->dir_next(dir, name, capacity, type) : VFS_BAD_HANDLE;
}
enum vfs_error vfs_dir_close(struct vfs_dir *dir) {
    return backend ? backend->dir_close(dir) : VFS_BAD_HANDLE;
}
