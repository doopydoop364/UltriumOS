#ifndef ULTRIUM_VFS_H
#define ULTRIUM_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct vfs_backend_ops;
#define VFS_NAME_MAX 31U
#define VFS_PATH_MAX 127U
#define VFS_FILE_MAX 4096U
#define VFS_DATA_MAX 65536U
#define VFS_NODE_MAX 128U
#define VFS_HANDLE_SLOTS 64U

enum vfs_error {
    VFS_OK = 0,
    VFS_END,
    VFS_NOT_FOUND,
    VFS_EXISTS,
    VFS_NOT_DIRECTORY,
    VFS_IS_DIRECTORY,
    VFS_INVALID_PATH,
    VFS_NO_SPACE,
    VFS_BAD_HANDLE,
    VFS_BUFFER_TOO_SMALL,
    VFS_UNSUPPORTED,
    VFS_NOT_EMPTY,
    VFS_READ_ONLY,   /* volume or device is read-only */
    VFS_IO,          /* block-device error */
    VFS_CORRUPT,     /* on-disk structure failed validation */
    VFS_BUSY,        /* open handles, mount point or frozen for shutdown */
    VFS_DENIED,      /* operation not permitted by policy (e.g. not a scratch disk) */
    VFS_RECOVERY     /* journal recovery or repair is required before this mount */
};

enum vfs_node_type { VFS_NODE_FILE, VFS_NODE_DIRECTORY };
enum vfs_open_flags { VFS_OPEN_READ = 1U, VFS_OPEN_WRITE = 2U };

/* Copy only by move convention: copies share a live capability. The
   generation rejects a stale copy after the slot has been closed and reused. */
struct vfs_file { uint32_t slot; uint32_t generation; };
struct vfs_dir { uint32_t slot; uint32_t generation; };

/* `filesystem` and `persistent` come from the mounted backend, never from the shell. */
struct vfs_info { enum vfs_node_type type; size_t size; const char *filesystem; bool persistent; };
enum vfs_error vfs_stat(const char *absolute_path, struct vfs_info *out);

/* Mounts. The root filesystem is the volatile RAMFS. Other backends mount on
   existing empty directories of the root filesystem (no nesting). Paths are
   routed by longest mount-point prefix; handles carry their mount index. */
#define VFS_MOUNT_MAX 4U
struct vfs_mount_ops {
    enum vfs_error (*sync)(void);      /* flush pending changes; failure keeps the mount */
    enum vfs_error (*release)(void);   /* sync and release the backend; failure keeps it mounted
                                          unless mounted() then reports false */
    bool (*mounted)(void);
};
struct vfs_mount_info {
    char path[VFS_PATH_MAX + 1U];
    const char *filesystem, *source;
    bool persistent, read_only;
    uint32_t open_handles;
};
enum vfs_error vfs_mount(const char *absolute_path, const struct vfs_backend_ops *ops,
                         const struct vfs_mount_ops *mount_ops, const char *filesystem,
                         const char *source, bool persistent, bool read_only);
enum vfs_error vfs_unmount(const char *absolute_path);
size_t vfs_mount_count(void);
bool vfs_mount_info_at(size_t index, struct vfs_mount_info *out);
/* Sync every mounted persistent filesystem. Continues past failures and
   returns the first error; *synced counts successful syncs. */
enum vfs_error vfs_sync_all(size_t *synced);
bool vfs_is_mount_point(const char *absolute_path);
/* Stop accepting changes (shutdown). Reads keep working. */
void vfs_freeze(bool frozen);
bool vfs_frozen(void);

/* Backends currently mount only at root. Tree removal must be revised before
   introducing mounts or symbolic links: never cross them implicitly. */
struct vfs_remove_report { size_t removed; char remaining[VFS_PATH_MAX + 1U]; };
enum vfs_error vfs_resolve_path(const char *path, const char *cwd, char *out, size_t capacity);
enum vfs_error vfs_copy(const char *source, const char *destination);
enum vfs_error vfs_rename(const char *source, const char *destination);
enum vfs_error vfs_remove_tree(const char *path, struct vfs_remove_report *report);
/* Observer receives a borrowed path after each successful unlink. It must not
   mutate the filesystem or retain the pointer. Used for shell partial-progress reporting. */
enum vfs_error vfs_remove_tree_observe(const char *path, struct vfs_remove_report *report,
                                      void (*removed)(const char *path));
bool vfs_init(void);
enum vfs_error vfs_normalize(const char *path, const char *cwd,
                             char *output, size_t capacity);
enum vfs_error vfs_create_file(const char *absolute_path);
enum vfs_error vfs_create_directory(const char *absolute_path);
enum vfs_error vfs_unlink(const char *absolute_path);
bool vfs_is_directory(const char *absolute_path);
/* Open output tokens must be zero-initialized and empty. */
enum vfs_error vfs_file_open(const char *absolute_path, uint32_t flags,
                             struct vfs_file *out);
enum vfs_error vfs_file_close(struct vfs_file *file);
enum vfs_error vfs_file_read(struct vfs_file *file, void *buffer,
                             size_t capacity, size_t *bytes_read);
enum vfs_error vfs_file_write(struct vfs_file *file, const void *buffer,
                              size_t length, size_t *bytes_written);
enum vfs_error vfs_dir_open(const char *absolute_path, struct vfs_dir *out);
enum vfs_error vfs_dir_next(struct vfs_dir *dir, char *name, size_t capacity,
                            enum vfs_node_type *type);
enum vfs_error vfs_dir_close(struct vfs_dir *dir);

/* Deterministic one-shot allocation failure injection for kernel tests. */
void vfs_test_fail_alloc_after(int32_t successful_allocations);

#endif
