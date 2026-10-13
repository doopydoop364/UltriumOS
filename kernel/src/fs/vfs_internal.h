#ifndef ULTRIUM_VFS_INTERNAL_H
#define ULTRIUM_VFS_INTERNAL_H

#include "vfs.h"

struct vfs_backend_ops {
    enum vfs_error (*create_file)(const char *path);
    enum vfs_error (*create_directory)(const char *path);
    enum vfs_error (*unlink)(const char *path);
    bool (*is_directory)(const char *path);
    enum vfs_error (*file_open)(const char *path, uint32_t flags,
                                struct vfs_file *out);
    enum vfs_error (*file_close)(struct vfs_file *file);
    enum vfs_error (*file_read)(struct vfs_file *file, void *buffer,
                                 size_t capacity, size_t *bytes_read);
    enum vfs_error (*file_write)(struct vfs_file *file, const void *buffer,
                                  size_t length, size_t *bytes_written);
    enum vfs_error (*dir_open)(const char *path, struct vfs_dir *out);
    enum vfs_error (*dir_next)(struct vfs_dir *dir, char *name, size_t capacity,
                               enum vfs_node_type *type);
    enum vfs_error (*dir_close)(struct vfs_dir *dir);
};

bool ramfs_init(void);
const struct vfs_backend_ops *ramfs_backend(void);

#endif
