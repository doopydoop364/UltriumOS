#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ufs2.h"
#include "crc32c.h"
#include "memory/vmm.h"
#include "memory.h"

/* UltriumFS 2 core: geometry, superblock pair and write-ahead journal. See
   docs/ultriumfs-v2-format.md sections 2-4 and 9. Every flush that the
   specification requires is issued here; a missing one is a bug that the
   volatile-cache tests are designed to expose. */

static const uint8_t sb_magic[8] = {'U', 'L', 'T', 'R', 'I', 'U', 'M', '2'};
#define MAGIC_DESCRIPTOR 0x32444a55U   /* "UJD2" little-endian */
#define MAGIC_COMMIT 0x32434a55U       /* "UJC2" */
#define SB_CRC_OFFSET 252U
#define BLOCK_CRC_OFFSET 4092U
#define DESC_TARGETS 24U

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

uint32_t ufs2_crc32(const uint8_t uuid[16], uint32_t context, const void *data, size_t length) {
    uint8_t ctx[4]; put32(ctx, context);
    uint32_t c = crc32c_begin();
    c = crc32c_update(c, uuid, 16); c = crc32c_update(c, ctx, 4); c = crc32c_update(c, data, length);
    return crc32c_end(c);
}
uint32_t ufs2_crc64ctx(const uint8_t uuid[16], uint64_t context, const void *data, size_t length) {
    uint8_t ctx[8]; put64(ctx, context);
    uint32_t c = crc32c_begin();
    c = crc32c_update(c, uuid, 16); c = crc32c_update(c, ctx, 8); c = crc32c_update(c, data, length);
    return crc32c_end(c);
}
static uint32_t sb_crc(uint32_t block, const uint8_t *data) {   /* context: block number only */
    uint8_t ctx[8]; put64(ctx, block);
    uint32_t c = crc32c_begin();
    c = crc32c_update(c, ctx, 8); c = crc32c_update(c, data, SB_CRC_OFFSET);
    return crc32c_end(c);
}

/* ---- geometry ---- */
bool ufs2_geometry_for(uint32_t total, struct ufs2_geometry *g) {
    if (total < UFS2_MIN_BLOCKS || total > UFS2_MAX_BLOCKS) return false;
    g->total = total;
    g->bitmap_blocks = (total + UFS2_BITMAP_BITS - 1) / UFS2_BITMAP_BITS;
    g->max_txn = 12 + g->bitmap_blocks;
    uint32_t minimum = 4 * (g->max_txn + 2), wanted = total / 64;
    g->journal_blocks = wanted < minimum ? minimum : wanted > 4096 ? 4096 : wanted;
    uint32_t inodes = total / 4;
    if (inodes < 64) inodes = 64;
    if (inodes > 65536) inodes = 65536;
    g->inode_count = (inodes + 15) / 16 * 16;
    g->inode_blocks = g->inode_count / 16;
    g->journal_start = 1;
    g->bitmap_start = 1 + g->journal_blocks;
    g->inode_start = g->bitmap_start + g->bitmap_blocks;
    g->data_start = g->inode_start + g->inode_blocks;
    return g->max_txn <= UFS2_TXN_LIMIT && g->data_start + 8 < total;
}
static bool geometry_equal(const struct ufs2_geometry *a, const struct ufs2_geometry *b) {
    return !memcmp(a, b, sizeof(*a));
}

/* ---- superblock encode/decode ---- */
static void sb_encode(const struct ufs2_sb *sb, uint32_t block, uint8_t *out) {
    memset(out, 0, UFS2_BLOCK);
    memcpy(out, sb_magic, 8);
    put32(out + 8, UFS2_VERSION); put32(out + 12, UFS2_BLOCK);
    put64(out + 16, sb->generation); put64(out + 24, sb->geo.total);
    put32(out + 32, sb->compat); put32(out + 36, sb->ro_compat); put32(out + 40, sb->incompat);
    put32(out + 44, sb->state);
    memcpy(out + 48, sb->uuid, 16);
    memcpy(out + 64, sb->label, 32);
    put32(out + 96, sb->geo.journal_start); put32(out + 100, sb->geo.journal_blocks); put32(out + 104, sb->geo.max_txn);
    put32(out + 108, sb->geo.bitmap_start); put32(out + 112, sb->geo.bitmap_blocks);
    put32(out + 116, sb->geo.inode_start); put32(out + 120, sb->geo.inode_count); put32(out + 124, sb->geo.inode_blocks);
    put32(out + 128, sb->geo.data_start); put32(out + 132, 1);
    put32(out + 136, UFS2_INODE_SIZE); put32(out + 140, UFS2_DIRENT_SIZE);
    put64(out + 144, sb->tail_seq); put32(out + 152, sb->tail_pos);
    put32(out + SB_CRC_OFFSET, sb_crc(block, out));
}
static bool all_zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return false; return true; }

/* VFS_OK; VFS_CORRUPT (invalid or not a v2 superblock); VFS_UNSUPPORTED
   (valid v2 superblock this implementation cannot mount). */
static enum vfs_error sb_decode(const uint8_t *raw, uint32_t block, struct ufs2_sb *sb) {
    if (memcmp(raw, sb_magic, 8)) return VFS_CORRUPT;
    if (le32(raw + SB_CRC_OFFSET) != sb_crc(block, raw)) return VFS_CORRUPT;
    memset(sb, 0, sizeof(*sb));
    if (le32(raw + 12) != UFS2_BLOCK) return VFS_CORRUPT;
    uint64_t total = le64(raw + 24);
    if (le32(raw + 8) != UFS2_VERSION) return VFS_UNSUPPORTED;
    if (total < UFS2_MIN_BLOCKS || total > UFS2_MAX_BLOCKS || (block != 0 && block != total - 1)) return VFS_CORRUPT;
    sb->generation = le64(raw + 16);
    sb->compat = le32(raw + 32); sb->ro_compat = le32(raw + 36); sb->incompat = le32(raw + 40);
    sb->state = le32(raw + 44);
    memcpy(sb->uuid, raw + 48, 16);
    memcpy(sb->label, raw + 64, 32); sb->label[32] = 0;
    sb->geo = (struct ufs2_geometry){(uint32_t)total, le32(raw + 96), le32(raw + 100), le32(raw + 104), le32(raw + 108),
        le32(raw + 112), le32(raw + 116), le32(raw + 120), le32(raw + 124), le32(raw + 128)};
    sb->tail_seq = le64(raw + 144); sb->tail_pos = le32(raw + 152);
    struct ufs2_geometry canonical;
    if (!ufs2_geometry_for((uint32_t)total, &canonical) || !geometry_equal(&canonical, &sb->geo)) return VFS_CORRUPT;
    if (le32(raw + 132) != 1 || le32(raw + 136) != UFS2_INODE_SIZE || le32(raw + 140) != UFS2_DIRENT_SIZE) return VFS_CORRUPT;
    if (sb->state > UFS2_STATE_MOUNTED || sb->tail_seq < 1 || sb->tail_pos >= sb->geo.journal_blocks) return VFS_CORRUPT;
    if (!all_zero(raw + 156, 96)) return VFS_CORRUPT;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned char c = (unsigned char)sb->label[i];
        if (c && (c < 32 || c > 126)) return VFS_CORRUPT;
        if (!c) { for (unsigned k = i; k < 32; ++k) if (sb->label[k]) return VFS_CORRUPT; break; }
    }
    if (sb->incompat & ~UFS2_INCOMPAT_KNOWN) return VFS_UNSUPPORTED;
    if ((sb->incompat & UFS2_INCOMPAT_KNOWN) != UFS2_INCOMPAT_KNOWN) return VFS_UNSUPPORTED;
    return VFS_OK;
}
/* Fields that must be identical in both copies. */
static bool sb_static_equal(const struct ufs2_sb *a, const struct ufs2_sb *b) {
    return a->compat == b->compat && a->ro_compat == b->ro_compat && a->incompat == b->incompat &&
           !memcmp(a->uuid, b->uuid, 16) && !memcmp(a->label, b->label, 33) && geometry_equal(&a->geo, &b->geo);
}

/* ---- device access ---- */
static enum vfs_error from_block(enum block_error e) {
    return e == BLOCK_OK ? VFS_OK : e == BLOCK_READ_ONLY ? VFS_READ_ONLY : VFS_IO;
}
static enum vfs_error raw_read(struct block_device *d, uint32_t per, uint32_t block, void *buffer) {
    return from_block(block_read(d, (uint64_t)block * per, per, buffer, UFS2_BLOCK));
}
static enum vfs_error raw_write(struct block_device *d, uint32_t per, uint32_t block, const void *buffer) {
    return from_block(block_write(d, (uint64_t)block * per, per, buffer, UFS2_BLOCK));
}
static enum vfs_error core_read(const struct ufs2_core *c, uint32_t block, void *buffer) {
    if (block >= c->sb.geo.total) return VFS_CORRUPT;
    return raw_read(c->dev, c->per, block, buffer);
}
static enum vfs_error core_write(const struct ufs2_core *c, uint32_t block, const void *buffer) {
    if (block >= c->sb.geo.total) return VFS_CORRUPT;
    return raw_write(c->dev, c->per, block, buffer);
}
static enum vfs_error device_flush(const struct ufs2_core *c) {
    enum block_error e = block_flush(c->dev);
    return from_block(e);            /* a device without flush is refused earlier for writable opens */
}
enum vfs_error ufs2_flush(struct ufs2_core *c) { return device_flush(c); }
enum vfs_error ufs2_block_read(struct ufs2_core *c, uint32_t block, void *buffer) { return core_read(c, block, buffer); }
enum vfs_error ufs2_block_write(struct ufs2_core *c, uint32_t block, const void *buffer) {
    if (c->read_only) return VFS_READ_ONLY;
    return core_write(c, block, buffer);
}

static uint8_t buf_super[UFS2_BLOCK], buf_desc[UFS2_BLOCK], buf_commit[UFS2_BLOCK], buf_data[UFS2_BLOCK];

/* ---- uuid (unique, not random) ---- */
void ufs2_uuid_make(uint8_t out[16], const struct block_device *d) {
    static uint32_t counter;
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    uint8_t seed[16];
    put32(seed, lo); put32(seed + 4, hi); put32(seed + 8, ++counter); put32(seed + 12, d ? (uint32_t)d->block_count : 0);
    for (unsigned part = 0; part < 4; ++part) {
        uint32_t c = crc32c_begin();
        uint8_t tag = (uint8_t)(0x55 + part);
        c = crc32c_update(c, &tag, 1); c = crc32c_update(c, seed, 16);
        if (d) c = crc32c_update(c, d->serial, sizeof(d->serial));
        put32(out + part * 4, crc32c_end(c));
    }
}

/* ---- superblock selection ---- */
static uint32_t capacity_blocks(const struct block_device *d, uint32_t per) {
    uint64_t n = d->block_count / per;
    return n > UFS2_MAX_BLOCKS ? UFS2_MAX_BLOCKS : (uint32_t)n;
}

/* Reads and validates both copies. Fills sb/current/degraded. */
static enum vfs_error select_superblock(struct ufs2_core *c) {
    uint32_t cap = capacity_blocks(c->dev, c->per);
    struct ufs2_sb a, b; bool va = false, vb = false;
    enum vfs_error ea = raw_read(c->dev, c->per, 0, buf_super);
    if (ea != VFS_OK) return ea;
    enum vfs_error da = sb_decode(buf_super, 0, &a);
    va = da == VFS_OK;
    uint32_t b_block = va ? a.geo.total - 1 : cap - 1;
    if (cap < UFS2_MIN_BLOCKS || (va && a.geo.total > cap)) return VFS_CORRUPT;
    enum vfs_error eb = raw_read(c->dev, c->per, b_block, buf_super);
    if (eb != VFS_OK) return eb;
    enum vfs_error db = sb_decode(buf_super, b_block, &b);
    vb = db == VFS_OK && b.geo.total - 1 == b_block;
    if (db == VFS_UNSUPPORTED && !va) return VFS_UNSUPPORTED;
    if (da == VFS_UNSUPPORTED) return VFS_UNSUPPORTED;
    if (!va && !vb) return VFS_CORRUPT;                      /* both copies damaged: unrecoverable */
    if (va && vb) {
        if (!sb_static_equal(&a, &b)) return VFS_CORRUPT;
        if (a.generation == b.generation && memcmp(&a, &b, sizeof(a))) return VFS_CORRUPT;
        c->sb_degraded = false;
        if (a.generation >= b.generation) { c->sb = a; c->sb_current = 0; } else { c->sb = b; c->sb_current = 1; }
    } else {
        c->sb = va ? a : b; c->sb_current = va ? 0 : 1; c->sb_degraded = true; c->sb_was_degraded = true;
    }
    c->sb_block[0] = 0; c->sb_block[1] = c->sb.geo.total - 1;
    return VFS_OK;
}

/* Write the next superblock generation to the copy that is NOT current. */
enum vfs_error ufs2_sb_update(struct ufs2_core *c) {
    if (c->read_only) return VFS_READ_ONLY;
    int target = c->sb_current ^ 1;
    struct ufs2_sb next = c->sb;
    if (next.generation == UINT64_MAX) return VFS_NO_SPACE;
    next.generation += 1;
    sb_encode(&next, c->sb_block[target], buf_super);
    block_set_phase("sb-write");
    enum vfs_error e = core_write(c, c->sb_block[target], buf_super);
    if (e == VFS_OK) { block_set_phase("sb-flush"); e = device_flush(c); }
    if (e != VFS_OK) { c->faulted = true; c->read_only = true; c->ro_reason = "superblock update failed"; return e; }
    c->sb = next; c->sb_current = target; c->sb_degraded = false;
    return VFS_OK;
}

/* ---- format ---- */
enum vfs_error ufs2_core_format(struct block_device *d, const char *label, const uint8_t *uuid) {
    if (!d || !d->registered) return VFS_IO;
    if (d->holders) return VFS_BUSY;
    if (!(d->flags & BLOCK_FLAG_SCRATCH)) return VFS_DENIED;
    if (d->flags & BLOCK_FLAG_READ_ONLY) return VFS_READ_ONLY;
    if (!(d->flags & BLOCK_FLAG_FLUSH)) return VFS_UNSUPPORTED;      /* ordering is meaningless without flush */
    if (d->block_size < 512 || UFS2_BLOCK % d->block_size) return VFS_UNSUPPORTED;
    uint32_t per = UFS2_BLOCK / d->block_size, total = capacity_blocks(d, per);
    struct ufs2_sb sb = {0};
    if (!ufs2_geometry_for(total, &sb.geo)) return VFS_NO_SPACE;
    if (!block_claim(d)) return VFS_BUSY;
    sb.generation = 1; sb.incompat = UFS2_INCOMPAT_EXTENTS | UFS2_INCOMPAT_JOURNAL; sb.state = UFS2_STATE_CLEAN;
    if (uuid) memcpy(sb.uuid, uuid, 16); else ufs2_uuid_make(sb.uuid, d);
    for (unsigned i = 0; label && label[i] && i < 31; ++i) sb.label[i] = label[i];
    if (!label || !label[0]) memcpy(sb.label, "ULTRIUM", 7);
    sb.tail_seq = 1; sb.tail_pos = 0;
    enum vfs_error e = VFS_OK;
    /* Invalidate any previous filesystem first, then zero the journal, then write
       the new superblocks (copy B, then copy A) so an interrupted format is never mountable. */
    memset(buf_data, 0, UFS2_BLOCK);
    if (e == VFS_OK) e = raw_write(d, per, 0, buf_data);
    if (e == VFS_OK) e = raw_write(d, per, total - 1, buf_data);
    for (uint32_t b = 0; e == VFS_OK && b < sb.geo.journal_blocks; ++b) e = raw_write(d, per, sb.geo.journal_start + b, buf_data);
    if (e == VFS_OK) e = from_block(block_flush(d));
    if (e == VFS_OK) { sb_encode(&sb, total - 1, buf_super); e = raw_write(d, per, total - 1, buf_super); }
    if (e == VFS_OK) e = from_block(block_flush(d));
    if (e == VFS_OK) { sb_encode(&sb, 0, buf_super); e = raw_write(d, per, 0, buf_super); }
    if (e == VFS_OK) e = from_block(block_flush(d));
    block_release(d);
    return e;
}

/* ---- journal ---- */
static uint32_t ring_block(const struct ufs2_core *c, uint32_t pos) { return c->sb.geo.journal_start + pos % c->sb.geo.journal_blocks; }
static uint32_t ring_add(const struct ufs2_core *c, uint32_t pos, uint32_t n) {
    return (uint32_t)(((uint64_t)pos + n) % c->sb.geo.journal_blocks);
}
bool ufs2_target_ok(const struct ufs2_core *c, uint32_t b) {
    const struct ufs2_geometry *g = &c->sb.geo;
    if (b == 0 || b >= g->total || b == g->total - 1) return false;                  /* superblock copies */
    if (b >= g->journal_start && b < g->journal_start + g->journal_blocks) return false;
    return true;
}

/* Validate descriptor at ring position pos expecting sequence `expected`.
   Returns the payload count in *n, or 0 when there is no valid descriptor. */
static bool read_descriptor(struct ufs2_core *c, uint32_t pos, uint64_t expected, uint32_t *n, bool *looks_real) {
    /* *looks_real: the record is a damaged or incomplete transaction *at the expected position*, as opposed to
       nothing or a stale record from an earlier lap of the ring (valid checksum, other sequence). */
    *looks_real = false;
    if (core_read(c, ring_block(c, pos), buf_desc) != VFS_OK) return false;
    if (le32(buf_desc) != MAGIC_DESCRIPTOR) return false;
    if (le32(buf_desc + BLOCK_CRC_OFFSET) != ufs2_crc32(c->sb.uuid, pos, buf_desc, BLOCK_CRC_OFFSET)) { *looks_real = true; return false; }
    if (le64(buf_desc + 8) != expected) return false;
    *looks_real = true;
    uint32_t count = le32(buf_desc + 4);
    if (!count || count > c->sb.geo.max_txn || count + 2 > c->sb.geo.journal_blocks) return false;
    if (le64(buf_desc + 16) != 0) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t t = le32(buf_desc + DESC_TARGETS + 4 * i);
        if (!ufs2_target_ok(c, t)) return false;
        for (uint32_t k = 0; k < i; ++k) if (le32(buf_desc + DESC_TARGETS + 4 * k) == t) return false;
    }
    *n = count;
    return true;
}
/* Verify the commit block and the payload checksum of the transaction whose
   (valid) descriptor is in buf_desc. */
static bool verify_commit(struct ufs2_core *c, uint32_t pos, uint64_t seq, uint32_t n) {
    uint32_t crc = crc32c_begin(); uint8_t sq[8]; put64(sq, seq);
    crc = crc32c_update(crc, c->sb.uuid, 16); crc = crc32c_update(crc, sq, 8); crc = crc32c_update(crc, buf_desc, UFS2_BLOCK);
    for (uint32_t i = 0; i < n; ++i) {
        if (core_read(c, ring_block(c, ring_add(c, pos, 1 + i)), buf_data) != VFS_OK) return false;
        crc = crc32c_update(crc, buf_data, UFS2_BLOCK);
    }
    crc = crc32c_end(crc);
    uint32_t cpos = ring_add(c, pos, n + 1);
    if (core_read(c, ring_block(c, cpos), buf_commit) != VFS_OK) return false;
    return le32(buf_commit) == MAGIC_COMMIT && le32(buf_commit + 4) == n && le64(buf_commit + 8) == seq &&
           le32(buf_commit + 16) == crc && all_zero(buf_commit + 20, 4072) &&
           le32(buf_commit + BLOCK_CRC_OFFSET) == ufs2_crc32(c->sb.uuid, cpos, buf_commit, BLOCK_CRC_OFFSET);
}

/* Scan (and, when apply is set, replay) committed transactions from the tail. */
static enum vfs_error journal_scan(struct ufs2_core *c, bool apply, uint32_t *found) {
    uint32_t pos = c->sb.tail_pos; uint64_t expected = c->sb.tail_seq;
    *found = 0; c->incomplete_tail = false;
    uint32_t limit = c->sb.geo.journal_blocks / 3 + 1;
    for (uint32_t iteration = 0; iteration < limit; ++iteration) {
        uint32_t n = 0; bool real = false;
        if (!read_descriptor(c, pos, expected, &n, &real)) { if (real) c->incomplete_tail = true; break; }
        if (!verify_commit(c, pos, expected, n)) { c->incomplete_tail = true; break; }
        ++*found;
        if (apply) {
            block_set_phase("replay-apply");
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t target = le32(buf_desc + DESC_TARGETS + 4 * i);
                enum vfs_error e = core_read(c, ring_block(c, ring_add(c, pos, 1 + i)), buf_data);
                if (e == VFS_OK) e = core_write(c, target, buf_data);
                if (e != VFS_OK) return e;
            }
        }
        pos = ring_add(c, pos, n + 2); ++expected;
    }
    c->head_pos = pos; c->next_seq = expected;
    return VFS_OK;
}

/* ---- open / close ---- */
static void drop_stage(struct ufs2_core *c) {
    if (c->stage) { vmm_free_pages(c->stage); c->stage = 0; }
}
enum vfs_error ufs2_core_open(struct ufs2_core *c, struct block_device *d, bool want_write) {
    memset(c, 0, sizeof(*c));
    if (!d || !d->registered) return VFS_IO;
    if (d->block_size < 512 || UFS2_BLOCK % d->block_size) return VFS_UNSUPPORTED;
    if (!block_claim(d)) return VFS_BUSY;
    c->dev = d; c->per = UFS2_BLOCK / d->block_size;
    enum vfs_error e = select_superblock(c);
    if (e != VFS_OK) { block_release(d); memset(c, 0, sizeof(*c)); return e; }
    c->read_only = true; c->ro_reason = "requested read-only";
    if (want_write) {
        if (d->flags & BLOCK_FLAG_READ_ONLY) c->ro_reason = "device is read-only";
        else if (!(d->flags & BLOCK_FLAG_FLUSH)) c->ro_reason = "device has no flush: durable ordering cannot be provided";
        else if (c->sb.ro_compat & ~UFS2_ROCOMPAT_KNOWN) c->ro_reason = "unknown read-only-compatible feature";
        else { c->read_only = false; c->ro_reason = 0; }
    }
    uint32_t found = 0;
    e = journal_scan(c, false, &found);
    if (e != VFS_OK) { block_release(d); memset(c, 0, sizeof(*c)); return e; }
    c->pending = found;
    if (found && c->read_only) { block_release(d); memset(c, 0, sizeof(*c)); return VFS_RECOVERY; }
    if (!c->read_only) {
        c->stage = vmm_alloc_pages(c->sb.geo.max_txn);
        if (!c->stage) { block_release(d); memset(c, 0, sizeof(*c)); return VFS_NO_SPACE; }
        c->unclean = c->sb.state == UFS2_STATE_MOUNTED;
        if (found) {
            e = journal_scan(c, true, &found);
            if (e == VFS_OK) { block_set_phase("replay-flush"); e = device_flush(c); }
            if (e == VFS_OK) {                   /* checkpoint the replayed transactions */
                c->sb.tail_pos = c->head_pos; c->sb.tail_seq = c->next_seq;
                block_set_phase("replay-checkpoint");
                c->replayed = found;
                e = ufs2_sb_update(c);
            }
            if (e != VFS_OK) { drop_stage(c); block_release(d); memset(c, 0, sizeof(*c)); return e; }
        }
        c->sb.state = UFS2_STATE_MOUNTED;
        e = ufs2_sb_update(c);
        if (e != VFS_OK) { drop_stage(c); block_release(d); memset(c, 0, sizeof(*c)); return e; }
    }
    c->open = true;
    return VFS_OK;
}
enum vfs_error ufs2_core_close(struct ufs2_core *c) {
    if (!c->open) return VFS_NOT_FOUND;
    enum vfs_error e = VFS_OK;
    if (c->in_txn) ufs2_txn_abort(c);
    if (!c->read_only && !c->faulted) {
        c->sb.state = UFS2_STATE_CLEAN;
        e = ufs2_sb_update(c);
    } else if (c->faulted) e = VFS_IO;
    drop_stage(c);
    block_release(c->dev);
    memset(c, 0, sizeof(*c));
    return e;
}
enum vfs_error ufs2_test_set_tail(struct ufs2_core *c, uint32_t pos, uint64_t seq) {
    c->sb.tail_pos = pos; c->sb.tail_seq = seq; c->head_pos = pos; c->next_seq = seq;
    return ufs2_sb_update(c);
}
void ufs2_test_set_next_seq(struct ufs2_core *c, uint64_t seq) { c->next_seq = seq; }
void ufs2_test_abandon(struct ufs2_core *c) {
    if (!c->open) return;
    drop_stage(c);
    block_release(c->dev);
    memset(c, 0, sizeof(*c));
}

/* ---- transactions ---- */
enum vfs_error ufs2_txn_begin(struct ufs2_core *c) {
    if (!c->open) return VFS_BAD_HANDLE;
    if (c->read_only) return VFS_READ_ONLY;
    if (c->in_txn) return VFS_BUSY;
    c->in_txn = true; c->stage_count = 0;
    return VFS_OK;
}
void ufs2_txn_abort(struct ufs2_core *c) { c->in_txn = false; c->stage_count = 0; }

enum vfs_error ufs2_txn_block(struct ufs2_core *c, uint32_t block, uint8_t **image) {
    if (!c->in_txn) return VFS_BAD_HANDLE;
    if (!ufs2_target_ok(c, block)) return VFS_CORRUPT;
    for (uint32_t i = 0; i < c->stage_count; ++i)
        if (c->stage_block[i] == block) { *image = c->stage + (size_t)i * UFS2_BLOCK; return VFS_OK; }
    if (c->stage_count >= c->sb.geo.max_txn) return VFS_NO_SPACE;       /* transaction too large */
    uint8_t *slot = c->stage + (size_t)c->stage_count * UFS2_BLOCK;
    enum vfs_error e = core_read(c, block, slot);
    if (e != VFS_OK) return e;
    c->stage_block[c->stage_count++] = block;
    *image = slot;
    return VFS_OK;
}
enum vfs_error ufs2_meta_read(struct ufs2_core *c, uint32_t block, void *buffer) {
    if (c->in_txn)
        for (uint32_t i = 0; i < c->stage_count; ++i)
            if (c->stage_block[i] == block) { memcpy(buffer, c->stage + (size_t)i * UFS2_BLOCK, UFS2_BLOCK); return VFS_OK; }
    return core_read(c, block, buffer);
}

static enum vfs_error fault(struct ufs2_core *c, enum vfs_error e, const char *reason) {
    c->faulted = true; c->read_only = true; c->ro_reason = reason;
    c->in_txn = false; c->stage_count = 0;
    return e;
}

enum vfs_error ufs2_txn_commit(struct ufs2_core *c) {
    if (!c->in_txn) return VFS_BAD_HANDLE;
    uint32_t n = c->stage_count;
    if (!n) { c->in_txn = false; return VFS_OK; }
    if (n > c->sb.geo.max_txn || n + 2 > c->sb.geo.journal_blocks || c->next_seq == UINT64_MAX) {
        ufs2_txn_abort(c);
        return VFS_NO_SPACE;                         /* rejected before any write */
    }
    uint32_t pos = c->head_pos; uint64_t seq = c->next_seq;
    /* Journal invariant J1: with synchronous checkpoints the ring is empty here. */
    memset(buf_desc, 0, UFS2_BLOCK);
    put32(buf_desc, MAGIC_DESCRIPTOR); put32(buf_desc + 4, n); put64(buf_desc + 8, seq);
    for (uint32_t i = 0; i < n; ++i) put32(buf_desc + DESC_TARGETS + 4 * i, c->stage_block[i]);
    put32(buf_desc + BLOCK_CRC_OFFSET, ufs2_crc32(c->sb.uuid, pos, buf_desc, BLOCK_CRC_OFFSET));
    uint32_t pcrc = crc32c_begin(); uint8_t sq[8]; put64(sq, seq);
    pcrc = crc32c_update(pcrc, c->sb.uuid, 16); pcrc = crc32c_update(pcrc, sq, 8); pcrc = crc32c_update(pcrc, buf_desc, UFS2_BLOCK);
    for (uint32_t i = 0; i < n; ++i) pcrc = crc32c_update(pcrc, c->stage + (size_t)i * UFS2_BLOCK, UFS2_BLOCK);
    pcrc = crc32c_end(pcrc);

    /* 2-3: descriptor and payload, then flush. */
    block_set_phase("journal-write");
    enum vfs_error e = core_write(c, ring_block(c, pos), buf_desc);
    for (uint32_t i = 0; e == VFS_OK && i < n; ++i) e = core_write(c, ring_block(c, ring_add(c, pos, 1 + i)), c->stage + (size_t)i * UFS2_BLOCK);
    if (e == VFS_OK) { block_set_phase("journal-flush"); e = device_flush(c); }
    if (e != VFS_OK) return fault(c, e, "journal write failed");
    /* 4: commit block, then flush. The transaction is committed once this flush completes. */
    memset(buf_commit, 0, UFS2_BLOCK);
    put32(buf_commit, MAGIC_COMMIT); put32(buf_commit + 4, n); put64(buf_commit + 8, seq); put32(buf_commit + 16, pcrc);
    uint32_t cpos = ring_add(c, pos, n + 1);
    put32(buf_commit + BLOCK_CRC_OFFSET, ufs2_crc32(c->sb.uuid, cpos, buf_commit, BLOCK_CRC_OFFSET));
    block_set_phase("commit-write");
    e = core_write(c, ring_block(c, cpos), buf_commit);
    if (e == VFS_OK) { block_set_phase("commit-flush"); e = device_flush(c); }
    if (e != VFS_OK) return fault(c, e, "journal commit failed");
    /* 5-6: apply to the home locations, then flush. */
    block_set_phase("home-write");
    for (uint32_t i = 0; e == VFS_OK && i < n; ++i) e = core_write(c, c->stage_block[i], c->stage + (size_t)i * UFS2_BLOCK);
    if (e == VFS_OK) { block_set_phase("home-flush"); e = device_flush(c); }
    if (e != VFS_OK) return fault(c, e, "applying a committed transaction failed; remount replays it");
    /* 7: checkpoint. Journal space is reused only after this superblock is flushed (S1). */
    c->sb.tail_pos = ring_add(c, pos, n + 2); c->sb.tail_seq = seq + 1;
    block_set_phase("checkpoint");
    e = ufs2_sb_update(c);
    if (e != VFS_OK) return fault(c, e, "checkpoint failed; remount replays the transaction");
    c->head_pos = c->sb.tail_pos; c->next_seq = seq + 1;
    c->in_txn = false; c->stage_count = 0; ++c->txns;
    block_set_phase("");
    return VFS_OK;
}
