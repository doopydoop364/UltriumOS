#ifndef ULTRIUM_ULTRIUMFS_H
#define ULTRIUM_ULTRIUMFS_H
#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"
#include "vfs_internal.h"
#include "storage/block.h"

/* UltriumFS v1: a small write-through filesystem for one block device.
   Format: docs/ultriumfs-format.md. One volume may be mounted at a time. */
#define UFS_BLOCK_SIZE 4096U
#define UFS_VERSION 1U
#define UFS_MAX_FILE_BYTES (1036ULL * UFS_BLOCK_SIZE)
#define UFS_MAX_TOTAL_BLOCKS (1U << 20)
#define UFS_MAX_INODES 65536U

struct ufs_info {
    char label[17];
    uint64_t total_blocks, free_blocks;   /* 4 KiB filesystem blocks */
    uint32_t inode_count, free_inodes;
    uint32_t orphan_inodes;
    bool read_only, unclean_mount;
    bool faulted;                         /* a metadata write failed: read-only until remount */
    struct block_device *device;
};

/* Destructive. Requires a writable, unmounted, scratch-flagged device. */
enum vfs_error ufs_format(struct block_device *device, const char *label);
enum vfs_error ufs_mount(struct block_device *device, bool read_only);
enum vfs_error ufs_unmount(void);
/* Flush data, mark the volume clean and flush again. Failure is reported and
   the volume stays mounted. */
enum vfs_error ufs_sync(void);
bool ufs_mounted(void);
bool ufs_get_info(struct ufs_info *info);
const struct vfs_backend_ops *ufs_backend(void);
/* Test hook: drop the mounted volume without syncing or marking it clean,
   simulating loss of power after write-through. Never used outside tests. */
void ufs_test_abandon(void);
#endif
