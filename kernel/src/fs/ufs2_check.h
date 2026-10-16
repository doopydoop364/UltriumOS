#ifndef ULTRIUM_UFS2_CHECK_H
#define ULTRIUM_UFS2_CHECK_H
#include "ufs2_fs.h"

#define UFS2_CHECK_ACTIONS 8U
struct ufs2_check_result {
    enum vfs_error status;              /* VFS_OK: the check ran; otherwise why it could not (BUSY, CORRUPT, UNSUPPORTED, IO) */
    bool superblock_unreadable;         /* both superblock copies invalid */
    bool journal_pending;               /* committed transactions waiting for replay */
    uint32_t pending_transactions;
    struct ufs2_report before, after;   /* `after` is valid only when a repair ran */
    bool repair_requested, repair_refused, repair_ran;
    const char *refusal;                /* why a repair was refused, with the options available */
    const char *actions[UFS2_CHECK_ACTIONS];
    uint32_t action_count;
    uint32_t bitmap_leaked, bitmap_unmarked;   /* counts that the bitmap rebuild corrected */
    bool fully_repaired;                /* nothing but (reported, unrepaired) orphans remains */
    bool safe_to_mount_writable;
};

/* Check (and optionally repair) a v2 volume on an unmounted device. The default
   never modifies the device. Repair is refused on a claimed (mounted) device,
   a read-only device, a device without flush, and whenever structural
   corruption is present. */
enum vfs_error ufs2_check(struct block_device *device, bool repair, struct ufs2_check_result *result);
#endif
