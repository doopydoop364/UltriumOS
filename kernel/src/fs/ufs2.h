#ifndef ULTRIUM_UFS2_H
#define ULTRIUM_UFS2_H
#include <stdbool.h>
#include <stdint.h>
#include "vfs.h"
#include "storage/block.h"

/* UltriumFS 2 core: superblock pair and write-ahead metadata journal.
   Specification: docs/ultriumfs-v2-format.md. Single-CPU, non-reentrant. */
#define UFS2_BLOCK 4096U
#define UFS2_VERSION 2U
#define UFS2_MIN_BLOCKS 512U
#define UFS2_MAX_BLOCKS (1U << 20)
#define UFS2_BITMAP_BITS 32704U
#define UFS2_INODE_SIZE 256U
#define UFS2_DIRENT_SIZE 64U
#define UFS2_TXN_LIMIT 45U            /* upper bound of max_txn_blocks (12 + 33 bitmap blocks) */
#define UFS2_INCOMPAT_EXTENTS 1U
#define UFS2_INCOMPAT_JOURNAL 2U
#define UFS2_INCOMPAT_KNOWN (UFS2_INCOMPAT_EXTENTS | UFS2_INCOMPAT_JOURNAL)
#define UFS2_ROCOMPAT_KNOWN 0U
#define UFS2_STATE_CLEAN 0U
#define UFS2_STATE_MOUNTED 1U

struct ufs2_geometry {
    uint32_t total, journal_start, journal_blocks, max_txn, bitmap_start, bitmap_blocks,
             inode_start, inode_count, inode_blocks, data_start;
};
struct ufs2_sb {
    uint64_t generation;
    uint32_t compat, ro_compat, incompat, state;
    uint8_t uuid[16];
    char label[33];
    struct ufs2_geometry geo;
    uint64_t tail_seq;
    uint32_t tail_pos;
};

struct ufs2_core {
    struct block_device *dev;
    uint32_t per;                       /* device blocks per filesystem block */
    bool open, read_only, faulted;
    const char *ro_reason;              /* why a read-write open became read-only, if it did */
    struct ufs2_sb sb;
    uint32_t sb_block[2];               /* block numbers of copies A and B */
    int sb_current;                     /* which copy holds `sb` */
    bool sb_degraded;                   /* one copy is invalid right now (healed by the next superblock update) */
    bool sb_was_degraded;               /* one copy was invalid at open; a read-write open has already rewritten it */
    bool unclean;                       /* previous session left the state "mounted" */
    bool incomplete_tail;               /* an uncommitted transaction was found and ignored */
    uint32_t replayed;                  /* transactions replayed at open */
    uint32_t pending;                   /* committed transactions found by a read-only open */
    /* journal */
    uint32_t head_pos;
    uint64_t next_seq;
    /* transaction staging */
    uint8_t *stage;
    uint32_t stage_block[UFS2_TXN_LIMIT];
    uint32_t stage_count;
    bool in_txn;
    uint64_t txns;                      /* transactions committed in this session */
};

bool ufs2_geometry_for(uint32_t total_blocks, struct ufs2_geometry *geometry);
void ufs2_uuid_make(uint8_t out[16], const struct block_device *device);

/* Destructive: writable, unmounted, scratch-flagged device only. `uuid` may be
   NULL (a unique, non-random one is derived). */
enum vfs_error ufs2_core_format(struct block_device *device, const char *label, const uint8_t *uuid);
enum vfs_error ufs2_core_format_with(struct block_device *device, const char *label, const uint8_t *uuid,
                                     enum vfs_error (*populate)(struct ufs2_core *, void *), void *context);
/* Open: select the superblock, replay the journal (writable opens only). A
   read-only open of a volume needing recovery fails with VFS_RECOVERY. */
enum vfs_error ufs2_core_open(struct ufs2_core *core, struct block_device *device, bool want_write);
/* As ufs2_core_open, but a writable open does not mark the volume mounted until
   ufs2_core_mark_mounted() (so a failed verification leaves the state untouched). */
enum vfs_error ufs2_core_open_ex(struct ufs2_core *core, struct block_device *device, bool want_write, bool mark_mounted);
enum vfs_error ufs2_core_mark_mounted(struct ufs2_core *core);
/* Read-only open for the checker: a committed-but-unapplied journal is reported
   in core->pending instead of being refused. Never writes. */
enum vfs_error ufs2_core_inspect(struct ufs2_core *core, struct block_device *device);
enum vfs_error ufs2_core_open_impl(struct ufs2_core *core, struct block_device *device, bool want_write, bool mark_mounted, bool inspect);
enum vfs_error ufs2_core_close(struct ufs2_core *core);

/* Metadata transactions. Blocks modified inside a transaction become durable
   atomically at ufs2_txn_commit(). A failed commit faults the volume. */
enum vfs_error ufs2_txn_begin(struct ufs2_core *core);
/* Writable image of a metadata block (loaded from disk or an earlier staged
   copy). VFS_NO_SPACE means the transaction would exceed max_txn_blocks. */
enum vfs_error ufs2_txn_block(struct ufs2_core *core, uint32_t block, uint8_t **image);
enum vfs_error ufs2_txn_commit(struct ufs2_core *core);
void ufs2_txn_abort(struct ufs2_core *core);
/* Read a metadata block, seeing the staged copy when one exists. */
enum vfs_error ufs2_meta_read(struct ufs2_core *core, uint32_t block, void *buffer);
/* Plain block access for file data (no journaling). */
enum vfs_error ufs2_block_read(struct ufs2_core *core, uint32_t block, void *buffer);
enum vfs_error ufs2_block_write(struct ufs2_core *core, uint32_t block, const void *buffer);
enum vfs_error ufs2_flush(struct ufs2_core *core);

/* Superblock state transitions (clean/mounted) and explicit update. */
enum vfs_error ufs2_sb_update(struct ufs2_core *core);
/* Is `block` a legal home location for a journaled metadata block? */
bool ufs2_target_ok(const struct ufs2_core *core, uint32_t block);

/* CRC-32C with the format's context prefix: UUID then a 32- or 64-bit value. */
uint32_t ufs2_crc32(const uint8_t uuid[16], uint32_t context, const void *data, size_t length);
uint32_t ufs2_crc64ctx(const uint8_t uuid[16], uint64_t context, const void *data, size_t length);

/* Test hooks. */
void ufs2_test_abandon(struct ufs2_core *core);   /* drop without writing anything */
enum vfs_error ufs2_test_set_tail(struct ufs2_core *core, uint32_t pos, uint64_t seq);
void ufs2_test_set_next_seq(struct ufs2_core *core, uint64_t seq);
#endif
