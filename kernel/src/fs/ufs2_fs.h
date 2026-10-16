#ifndef ULTRIUM_UFS2_FS_H
#define ULTRIUM_UFS2_FS_H
#include <stdbool.h>
#include <stdint.h>
#include "ufs2.h"
#include "vfs_internal.h"

/* UltriumFS 2 filesystem (docs/ultriumfs-v2-format.md). One volume at a time. */

/* ---- verification report (shared by mount and the checker) ---- */
enum ufs2_finding_class { UFS2_REPAIRABLE = 0, UFS2_ORPHAN = 1, UFS2_STRUCTURAL = 2 };
enum ufs2_finding_code {
    UF_BITMAP_CRC = 1, UF_BITMAP_PADDING, UF_BITMAP_LEAK, UF_BITMAP_UNMARKED,
    UF_INODE_CRC, UF_INODE_FIELDS, UF_ROOT_INVALID, UF_EXTENT_RANGE, UF_EXTENT_ORDER, UF_EXTENT_OVERLAP,
    UF_EXTENT_COUNT, UF_LEAF_INVALID, UF_DIR_BLOCK, UF_DIR_ENTRY, UF_DIR_DUPLICATE_REF, UF_DIR_PARENT,
    UF_DIR_CYCLE, UF_DIR_DUPLICATE_NAME, UF_ORPHAN_INODE, UF_STALE_STATE, UF_SB_COPY, UF_JOURNAL_PENDING
};
#define UFS2_REPORT_ITEMS 32U
struct ufs2_report {
    uint32_t count[3];                  /* findings per class (all, not only stored ones) */
    uint32_t stored;
    struct { uint8_t cls, code; uint32_t inode, block; } item[UFS2_REPORT_ITEMS];
    uint32_t inodes_used, files, directories, orphans, blocks_owned, extents;
    uint32_t bitmap_leaked, bitmap_unmarked;
    bool large_directory_unchecked;     /* a directory too large for the duplicate-name scan */
};
const char *ufs2_finding_name(uint8_t code);

/* Verify an open volume without modifying it. When `owned` / `used` are
   non-NULL the caller receives ownership bitmaps (vmm pages, caller frees) so a
   mount can reuse them. Returns VFS_OK even when findings exist. */
enum vfs_error ufs2_verify(struct ufs2_core *core, struct ufs2_report *report);
/* Disk bitmap as the verifier sees it: flat bit array (bit b = block b). */

/* ---- volume ---- */
struct ufs2_fs_info {
    char label[33];
    uint64_t total_blocks, free_blocks;         /* 4 KiB data-area blocks */
    uint32_t inode_count, free_inodes, orphans;
    bool read_only, faulted, unclean, repair_recommended;
    uint64_t generation, transactions;
    struct block_device *device;
    const char *ro_reason;
};
enum vfs_error ufs2_fs_format(struct block_device *device, const char *label, const uint8_t *uuid);
/* Mount. VFS_CORRUPT: structural damage (nothing mounted). VFS_RECOVERY:
   journal pending on a device that cannot be written. A volume with only
   repairable findings mounts read-only (repair_recommended). */
enum vfs_error ufs2_fs_mount(struct block_device *device, bool want_write, struct ufs2_report *report);
enum vfs_error ufs2_fs_unmount(void);
enum vfs_error ufs2_fs_sync(void);
bool ufs2_fs_mounted(void);
bool ufs2_fs_get_info(struct ufs2_fs_info *info);
const struct vfs_backend_ops *ufs2_fs_backend(void);
void ufs2_fs_test_abandon(void);
/* Test hook: the volume's core (journal/superblock access for tests). */
struct ufs2_core *ufs2_fs_test_core(void);
#endif
