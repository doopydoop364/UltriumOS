#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ufs2_check.h"
#include "ufs2_meta.h"
#include "memory/vmm.h"
#include "memory.h"

/* Filesystem checker and conservative repair (docs/ultriumfs-v2-format.md
   section 11). Repairs use the same journaled, flush-ordered paths as normal
   operation; nothing is deleted, truncated, moved or invented. */

static void action(struct ufs2_check_result *r, const char *text) {
    if (r->action_count < UFS2_CHECK_ACTIONS) r->actions[r->action_count++] = text;
}
static bool safe_after(const struct ufs2_report *rep) { return !rep->count[UFS2_STRUCTURAL] && !rep->count[UFS2_REPAIRABLE]; }

/* Rebuild every bitmap block from the ownership derived from the inodes, in one
   journaled transaction (the bitmap occupies at most max_txn_blocks - 12 blocks). */
static enum vfs_error rebuild_bitmap(struct ufs2_core *c, const uint8_t *owned) {
    const struct ufs2_geometry *g = &c->sb.geo;
    enum vfs_error e = ufs2_txn_begin(c);
    if (e != VFS_OK) return e;
    for (uint32_t k = 0; k < g->bitmap_blocks && e == VFS_OK; ++k) {
        uint8_t *image;
        e = ufs2_txn_block(c, g->bitmap_start + k, &image);
        if (e != VFS_OK) break;
        memset(image, 0, UFS2_BLOCK);
        memcpy(image, owned + (size_t)k * 4088, 4088);
        ufs2_bitmap_seal(c->sb.uuid, g->bitmap_start + k, image);
    }
    if (e != VFS_OK) { ufs2_txn_abort(c); return e; }
    return ufs2_txn_commit(c);
}

enum vfs_error ufs2_check(struct block_device *d, bool repair, struct ufs2_check_result *r) {
    memset(r, 0, sizeof(*r));
    r->repair_requested = repair;
    if (!d || !d->registered) return r->status = VFS_IO;
    if (d->holders) return r->status = VFS_BUSY;                      /* mounted or otherwise in use */
    struct ufs2_core core;
    enum vfs_error e = ufs2_core_inspect(&core, d);
    if (e == VFS_CORRUPT) { r->superblock_unreadable = true; r->status = VFS_CORRUPT; r->safe_to_mount_writable = false;
        r->refusal = "both superblock copies are damaged: no third copy exists, so nothing can be repaired automatically"; r->repair_refused = repair; return VFS_OK; }
    if (e != VFS_OK) return r->status = e;                             /* UNSUPPORTED, IO, ... */
    r->pending_transactions = core.pending; r->journal_pending = core.pending != 0;
    if (r->journal_pending) {
        /* Home locations are stale until replay: verifying them now would report false problems. */
        r->before.count[UFS2_REPAIRABLE] = 1;
        r->before.item[0].cls = UFS2_REPAIRABLE; r->before.item[0].code = UF_JOURNAL_PENDING; r->before.stored = 1;
    } else {
        e = ufs2_verify(&core, &r->before);
        if (e != VFS_OK) { ufs2_test_abandon(&core); return r->status = e; }
    }
    ufs2_test_abandon(&core);                                           /* inspect never wrote anything */
    r->status = VFS_OK;
    if (!repair) { r->safe_to_mount_writable = safe_after(&r->before); return VFS_OK; }

    /* ---- repair ---- */
    if (!r->journal_pending && r->before.count[UFS2_STRUCTURAL]) {
        r->repair_refused = true;
        r->refusal = "structural corruption present: automatic repair could lose recoverable data. Options: restore from a backup, or reformat the scratch disk";
        return VFS_OK;
    }
    if (d->flags & BLOCK_FLAG_READ_ONLY) { r->repair_refused = true; r->refusal = "the device is read-only"; return VFS_OK; }
    if (!(d->flags & BLOCK_FLAG_FLUSH)) { r->repair_refused = true; r->refusal = "the device has no flush: ordered, durable repair cannot be guaranteed"; return VFS_OK; }
    e = ufs2_core_open_ex(&core, d, true, false);                       /* replays the journal and heals a damaged superblock copy */
    if (e != VFS_OK) { r->repair_refused = true; r->refusal = "the volume could not be opened for writing"; return VFS_OK; }
    r->repair_ran = true;
    if (r->journal_pending) action(r, "replayed committed journal transactions");
    if (core.sb_degraded && ufs2_sb_update(&core) == VFS_OK)          /* writes the next generation to the damaged copy */
        action(r, "rewrote the damaged superblock copy from the valid one");
    struct ufs2_report mid; uint8_t *owned = 0;
    e = ufs2_verify_ex(&core, &mid, &owned);
    if (e != VFS_OK) { ufs2_test_abandon(&core); r->status = e; return VFS_OK; }
    if (mid.count[UFS2_STRUCTURAL]) {       /* replay exposed structural damage: do not touch anything else */
        if (owned) vmm_free_pages(owned);
        r->after = mid; ufs2_test_abandon(&core);
        r->repair_refused = true; r->refusal = "structural corruption present after journal replay; no further repair attempted";
        return VFS_OK;
    }
    if (mid.bitmap_leaked || mid.bitmap_unmarked || mid.count[UFS2_REPAIRABLE]) {
        bool bitmap_problem = false;
        for (uint32_t i = 0; i < mid.stored; ++i)
            if (mid.item[i].code == UF_BITMAP_LEAK || mid.item[i].code == UF_BITMAP_UNMARKED || mid.item[i].code == UF_BITMAP_CRC || mid.item[i].code == UF_BITMAP_PADDING) bitmap_problem = true;
        if (bitmap_problem && owned) {
            r->bitmap_leaked = mid.bitmap_leaked; r->bitmap_unmarked = mid.bitmap_unmarked;
            e = rebuild_bitmap(&core, owned);
            if (e != VFS_OK) { vmm_free_pages(owned); ufs2_test_abandon(&core); r->status = e; return VFS_OK; }
            action(r, "rebuilt the block bitmap from extent ownership");
        }
    }
    if (owned) vmm_free_pages(owned);
    e = ufs2_verify(&core, &r->after);
    if (e != VFS_OK) { ufs2_test_abandon(&core); r->status = e; return VFS_OK; }
    if (!r->after.count[UFS2_STRUCTURAL] && !r->after.count[UFS2_REPAIRABLE] && core.sb.state == UFS2_STATE_MOUNTED) {
        core.sb.state = UFS2_STATE_CLEAN;                               /* the previous session never unmounted; verification is clean */
        if (ufs2_sb_update(&core) == VFS_OK) action(r, "cleared the stale mounted state");
    }
    enum vfs_error c = ufs2_core_close(&core);                          /* marks the volume cleanly unmounted */
    if (c != VFS_OK) r->status = c;
    /* A stale state flag found only by the read-only inspection no longer counts once cleared. */
    r->fully_repaired = !r->after.count[UFS2_STRUCTURAL] && !r->after.count[UFS2_REPAIRABLE];
    r->safe_to_mount_writable = r->fully_repaired;
    return VFS_OK;
}
