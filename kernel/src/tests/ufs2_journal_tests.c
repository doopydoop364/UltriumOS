#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/ufs2.h"
#include "fs/crc32c.h"
#include "storage/block.h"
#include "memory/pmm.h"
#include "memory.h"

/* Tests for the UltriumFS 2 journal layer: geometry, superblock pair,
   transactions, replay and crash recovery. They run on a memory device with
   the volatile-cache model, so unflushed writes really can be lost. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define BLOCK 4096U

static struct block_device *dev;
static struct ufs2_core core;
static uint64_t frames0;
static const uint8_t UUID[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
static uint8_t img[BLOCK], img2[BLOCK];

static const char *rig_up(uint32_t block_size, uint64_t device_blocks) {
    frames0 = pmm_free_pages();
    dev = block_memory_create("u2j", block_size, device_blocks, false);
    REQUIRE(dev, "memory device creation failed");
    if (!block_memory_enable_cache(dev)) return "cache model unavailable";
    if (ufs2_core_format(dev, "JTEST", UUID) != VFS_OK) return "format failed";
    return 0;
}
static const char *rig_down(const char *result) {
    if (core.open) ufs2_test_abandon(&core);
    block_set_phase("");
    if (dev) { if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    if (!result && pmm_free_pages() != frames0) result = "frame accounting changed";
    return result;
}
#define RIG(bs, blocks) do { const char *r_ = rig_up(bs, blocks); if (r_) return rig_down(r_); } while (0)
#define FAIL(text) return rig_down(text)
#define CHECK(cond, text) do { if (!(cond)) FAIL(text); } while (0)

static void fill(uint8_t *out, uint8_t tag) { for (unsigned i = 0; i < BLOCK; ++i) out[i] = (uint8_t)(tag + i * 3U); }
static uint32_t data_block(unsigned k) { return core.sb.geo.data_start + k; }

/* One transaction writing `tag`-patterned images to `count` blocks. */
static enum vfs_error txn(const uint32_t *blocks, unsigned count, uint8_t tag) {
    enum vfs_error e = ufs2_txn_begin(&core);
    for (unsigned i = 0; e == VFS_OK && i < count; ++i) {
        uint8_t *image;
        e = ufs2_txn_block(&core, blocks[i], &image);
        if (e == VFS_OK) fill(image, (uint8_t)(tag + i));
    }
    if (e != VFS_OK) { ufs2_txn_abort(&core); return e; }
    return ufs2_txn_commit(&core);
}
static bool block_is(uint32_t block, uint8_t tag) {   /* tag 0xff => expect an all-zero block */
    if (ufs2_block_read(&core, block, img) != VFS_OK) return false;
    if (tag == 0xff) { for (unsigned i = 0; i < BLOCK; ++i) if (img[i]) return false; return true; }
    fill(img2, tag);
    return !memcmp(img, img2, BLOCK);
}

/* ---- geometry ---- */
static const char *g_geometry(void) {
    struct ufs2_geometry g;
    REQUIRE(!ufs2_geometry_for(511, &g) && !ufs2_geometry_for(UFS2_MAX_BLOCKS + 1, &g) && !ufs2_geometry_for(0, &g), "invalid sizes accepted");
    static const uint32_t sizes[] = {512, 513, 1024, 16384, 100000, UFS2_MAX_BLOCKS};
    for (unsigned i = 0; i < 6; ++i) {
        REQUIRE(ufs2_geometry_for(sizes[i], &g), "valid size rejected");
        REQUIRE(g.journal_start == 1 && g.bitmap_start == 1 + g.journal_blocks && g.inode_start == g.bitmap_start + g.bitmap_blocks &&
                g.data_start == g.inode_start + g.inode_blocks, "regions are not contiguous in order");
        REQUIRE(g.data_start + 8 < sizes[i], "no room for data");
        REQUIRE(g.bitmap_blocks == (sizes[i] + 32703) / 32704 && g.max_txn == 12 + g.bitmap_blocks && g.max_txn <= UFS2_TXN_LIMIT, "bitmap or transaction bound wrong");
        REQUIRE(g.journal_blocks >= 4 * (g.max_txn + 2) && g.journal_blocks <= 4096, "journal size out of range");
        REQUIRE(g.inode_count >= 64 && g.inode_count <= 65536 && g.inode_count % 16 == 0 && g.inode_blocks * 16 == g.inode_count, "inode count wrong");
    }
    REQUIRE(ufs2_geometry_for(UFS2_MAX_BLOCKS, &g) && g.bitmap_blocks == 33 && g.max_txn == 45 && g.journal_blocks == 4096, "4 GiB limits differ from the specification");
    REQUIRE(ufs2_geometry_for(512, &g) && g.journal_blocks == 60 && g.max_txn == 13, "2 MiB limits differ from the specification");
    return 0;
}

/* ---- format, open, flush policy ---- */
static const char *g_format_open(void) {
    RIG(4096, 1024);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && !core.read_only && !core.unclean && !core.sb_degraded, "open failed");
    CHECK(core.sb.geo.total == 1024 && core.sb.geo.journal_blocks == 60 && !memcmp(core.sb.label, "JTEST", 6), "superblock fields wrong");
    CHECK(!memcmp(core.sb.uuid, UUID, 16) && core.sb.tail_seq == 1 && core.sb.tail_pos == 0 && core.next_seq == 1, "journal start state wrong");
    CHECK(core.sb.state == UFS2_STATE_MOUNTED && core.sb.generation == 2, "open did not mark the volume mounted");
    CHECK(dev->holders == 1 && ufs2_core_format(dev, "X", UUID) == VFS_BUSY, "claimed device could be formatted");
    CHECK(ufs2_core_close(&core) == VFS_OK && dev->holders == 0, "close failed");
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && !core.unclean, "clean volume reported unclean");
    ufs2_test_abandon(&core);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.unclean, "abandoned session not reported unclean");
    return rig_down(0);
}

static const char *g_format_safety(void) {
    RIG(4096, 1024);
    dev->flags &= ~BLOCK_FLAG_SCRATCH;
    CHECK(ufs2_core_format(dev, "X", UUID) == VFS_DENIED, "non-scratch device formatted");
    dev->flags |= BLOCK_FLAG_SCRATCH; dev->flags &= ~BLOCK_FLAG_FLUSH;
    CHECK(ufs2_core_format(dev, "X", UUID) == VFS_UNSUPPORTED, "device without flush formatted");
    dev->flags |= BLOCK_FLAG_FLUSH | BLOCK_FLAG_READ_ONLY;
    CHECK(ufs2_core_format(dev, "X", UUID) == VFS_READ_ONLY, "read-only device formatted");
    dev->flags &= ~BLOCK_FLAG_READ_ONLY;
    struct block_device *tiny = block_memory_create("u2tiny", 4096, 256, false);
    CHECK(tiny, "tiny device creation failed");
    enum vfs_error e = ufs2_core_format(tiny, "X", UUID);
    CHECK(block_memory_destroy(tiny) && e == VFS_NO_SPACE, "device below 2 MiB formatted");
    return rig_down(0);
}

static const char *g_flush_policy(void) {
    RIG(4096, 1024);
    dev->flags &= ~BLOCK_FLAG_FLUSH;
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.read_only && core.ro_reason, "device without flush mounted read-write");
    uint8_t *image;
    CHECK(ufs2_txn_begin(&core) == VFS_READ_ONLY && ufs2_txn_block(&core, data_block(0), &image) != VFS_OK, "read-only open accepted a transaction");
    ufs2_test_abandon(&core);
    dev->flags |= BLOCK_FLAG_FLUSH | BLOCK_FLAG_READ_ONLY;
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.read_only, "read-only device mounted read-write");
    ufs2_test_abandon(&core);
    dev->flags &= ~BLOCK_FLAG_READ_ONLY;
    CHECK(ufs2_core_open(&core, dev, false) == VFS_OK && core.read_only, "read-only request ignored");
    CHECK(ufs2_sb_update(&core) == VFS_READ_ONLY, "read-only open wrote a superblock");
    return rig_down(0);
}

/* ---- superblock pair ---- */
static void corrupt_block(uint32_t fs_block, unsigned offset) {
    uint32_t per = BLOCK / dev->block_size;
    (void)block_read(dev, (uint64_t)fs_block * per, per, img, BLOCK);
    img[offset] ^= 0x40;
    (void)block_write(dev, (uint64_t)fs_block * per, per, img, BLOCK);
}
static const char *g_superblock_pair(void) {
    RIG(4096, 1024);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
    uint32_t a = core.sb_block[0], b = core.sb_block[1];
    CHECK(a == 0 && b == 1023, "copy locations wrong");
    int first = core.sb_current; uint64_t gen = core.sb.generation;
    CHECK(ufs2_sb_update(&core) == VFS_OK && core.sb_current == (first ^ 1) && core.sb.generation == gen + 1, "update did not alternate copies");
    CHECK(ufs2_sb_update(&core) == VFS_OK && core.sb_current == first && core.sb.generation == gen + 2, "second update did not alternate back");
    CHECK(ufs2_core_close(&core) == VFS_OK, "close failed");
    /* Damage each copy in turn: the other must serve, and the next update heals the damaged one. */
    for (int damaged = 0; damaged < 2; ++damaged) {
        corrupt_block(damaged ? b : a, 100);
        uint64_t gen_before = 0;
        CHECK(ufs2_core_open(&core, dev, false) == VFS_OK, "one damaged copy refused a read-only mount");
        CHECK(core.sb_degraded && core.sb_was_degraded && core.sb_current == (damaged ? 0 : 1), "degraded state or selection wrong");
        CHECK(ufs2_sb_update(&core) == VFS_READ_ONLY, "read-only open wrote a superblock");
        gen_before = core.sb.generation;
        ufs2_test_abandon(&core);
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.sb_was_degraded && !core.sb_degraded, "read-write open did not heal the damaged copy");
        CHECK(core.sb.generation > gen_before, "healing did not write a new generation");
        CHECK(ufs2_core_close(&core) == VFS_OK, "close failed");
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && !core.sb_degraded, "healed volume still degraded");
        CHECK(ufs2_core_close(&core) == VFS_OK, "close failed");
    }
    /* Both damaged: unmountable and nothing is written. */
    corrupt_block(a, 100); corrupt_block(b, 100);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_CORRUPT && dev->holders == 0, "both copies damaged but the volume mounted");
    CHECK(ufs2_core_format(dev, "AGAIN", UUID) == VFS_OK && ufs2_core_open(&core, dev, true) == VFS_OK, "device unusable after unrecoverable damage");
    ufs2_test_abandon(&core);
    return rig_down(0);
}

static void rewrite_sb(uint32_t fs_block, void (*edit)(uint8_t *), bool recrc) {
    uint32_t per = BLOCK / dev->block_size;
    (void)block_read(dev, (uint64_t)fs_block * per, per, img, BLOCK);
    edit(img);
    if (recrc) {   /* superblock checksum: CRC-32C over block number (u64) then bytes 0..251 */
        uint8_t ctx[8] = {(uint8_t)fs_block, (uint8_t)(fs_block >> 8), (uint8_t)(fs_block >> 16), (uint8_t)(fs_block >> 24), 0, 0, 0, 0};
        uint32_t c = crc32c_begin(); c = crc32c_update(c, ctx, 8); c = crc32c_update(c, img, 252); c = crc32c_end(c);
        img[252] = (uint8_t)c; img[253] = (uint8_t)(c >> 8); img[254] = (uint8_t)(c >> 16); img[255] = (uint8_t)(c >> 24);
    }
    (void)block_write(dev, (uint64_t)fs_block * per, per, img, BLOCK);
}
static void e_version(uint8_t *p) { p[8] = 3; }
static void e_incompat(uint8_t *p) { p[40] |= 0x80; }
static void e_rocompat(uint8_t *p) { p[36] |= 0x01; }
static void e_compat(uint8_t *p) { p[32] |= 0x01; }
static void e_label(uint8_t *p) { p[64] = 'Z'; }
static void e_geometry(uint8_t *p) { p[100] += 1; }
static void e_missing_required(uint8_t *p) { p[40] = 1; }
static void e_reserved(uint8_t *p) { p[200] = 1; }
static void e_tail(uint8_t *p) { p[152] = 0xff; p[153] = 0xff; }

static const char *g_superblock_policy(void) {
    struct { void (*edit)(uint8_t *); bool both; enum vfs_error expect; bool readonly; const char *name; } cases[] = {
        {e_version, true, VFS_UNSUPPORTED, false, "unsupported version"},
        {e_incompat, true, VFS_UNSUPPORTED, false, "unknown incompatible feature"},
        {e_missing_required, true, VFS_UNSUPPORTED, false, "missing required feature"},
        {e_rocompat, true, VFS_OK, true, "unknown read-only-compatible feature"},
        {e_compat, true, VFS_OK, false, "unknown compatible feature"},
        {e_geometry, true, VFS_CORRUPT, false, "non-canonical geometry"},
        {e_reserved, true, VFS_CORRUPT, false, "reserved bytes set"},
        {e_tail, true, VFS_CORRUPT, false, "journal tail out of range"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        RIG(4096, 1024);
        rewrite_sb(0, cases[i].edit, true); rewrite_sb(1023, cases[i].edit, true);
        enum vfs_error e = ufs2_core_open(&core, dev, true);
        if (e != cases[i].expect) { FAIL(cases[i].name); }
        if (e == VFS_OK && core.read_only != cases[i].readonly) FAIL("feature policy produced the wrong mode");
        if (e != VFS_OK && dev->holders) FAIL("failed open left the device claimed");
        const char *r = rig_down(0);
        if (r) return r;
    }
    /* Copies that disagree on static fields are corruption, not a choice. */
    RIG(4096, 1024);
    rewrite_sb(1023, e_label, true);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_CORRUPT, "copies with different static fields were accepted");
    return rig_down(0);
}

/* ---- transactions ---- */
static const char *g_txn_basic(void) {
    RIG(4096, 1024);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
    uint32_t blocks[3] = {data_block(0), data_block(1), core.sb.geo.bitmap_start};
    CHECK(txn(blocks, 3, 0x10) == VFS_OK, "commit failed");
    CHECK(block_is(blocks[0], 0x10) && block_is(blocks[1], 0x11) && block_is(blocks[2], 0x12), "home blocks do not hold the committed images");
    CHECK(core.sb.tail_seq == 2 && core.sb.tail_pos == 5 && core.head_pos == 5 && core.next_seq == 2 && core.txns == 1, "checkpoint did not advance the tail");
    /* Reads inside a transaction see staged data; abort leaves the disk untouched. */
    uint8_t *image;
    CHECK(ufs2_txn_begin(&core) == VFS_OK && ufs2_txn_block(&core, blocks[0], &image) == VFS_OK, "begin failed");
    CHECK(ufs2_txn_begin(&core) == VFS_BUSY, "nested transaction accepted");
    fill(image, 0x77);
    CHECK(ufs2_meta_read(&core, blocks[0], img2) == VFS_OK && img2[0] == 0x77, "staged data not visible to reads");
    ufs2_txn_abort(&core);
    CHECK(block_is(blocks[0], 0x10), "abort modified the disk");
    CHECK(ufs2_txn_begin(&core) == VFS_OK && ufs2_txn_commit(&core) == VFS_OK && core.txns == 1, "empty transaction should be a no-op");
    /* Many transactions: sequence numbers and positions advance. */
    for (unsigned i = 0; i < 20; ++i) CHECK(txn(blocks, 1 + i % 3, (uint8_t)(i * 5)) == VFS_OK, "repeated commit failed");
    CHECK(core.next_seq == 22 && core.txns == 21, "sequence numbers did not advance by one per transaction");
    ufs2_test_abandon(&core);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && !core.incomplete_tail && core.replayed == 0, "stale ring records were reported as a damaged tail");
    CHECK(ufs2_core_close(&core) == VFS_OK, "close failed");
    return rig_down(0);
}

static const char *g_txn_limits(void) {
    RIG(4096, 1024);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
    uint32_t max = core.sb.geo.max_txn;
    uint8_t *image;
    CHECK(ufs2_txn_begin(&core) == VFS_OK, "begin failed");
    for (uint32_t i = 0; i < max; ++i) { CHECK(ufs2_txn_block(&core, data_block(i), &image) == VFS_OK, "block within the bound rejected"); fill(image, 0x31); }
    CHECK(ufs2_txn_block(&core, data_block(0), &image) == VFS_OK, "re-staging a staged block must be free");
    uint64_t before = block_memory_events(dev);
    CHECK(ufs2_txn_block(&core, data_block(max), &image) == VFS_NO_SPACE, "transaction larger than max_txn_blocks accepted");
    ufs2_txn_abort(&core);
    for (uint32_t i = 0; i < max; ++i) CHECK(block_is(data_block(i), 0xff), "rejected transaction changed the disk");
    (void)before;
    /* A transaction exactly at the bound commits. */
    CHECK(ufs2_txn_begin(&core) == VFS_OK, "begin failed");
    for (uint32_t i = 0; i < max; ++i) { CHECK(ufs2_txn_block(&core, data_block(i), &image) == VFS_OK, "stage failed"); fill(image, 0x42); }
    CHECK(ufs2_txn_commit(&core) == VFS_OK && block_is(data_block(max - 1), 0x42), "maximum-size transaction failed");
    /* Illegal home locations are refused before staging. */
    CHECK(ufs2_txn_begin(&core) == VFS_OK, "begin failed");
    CHECK(ufs2_txn_block(&core, 0, &image) == VFS_CORRUPT && ufs2_txn_block(&core, core.sb.geo.total - 1, &image) == VFS_CORRUPT, "superblock copy accepted as a home location");
    CHECK(ufs2_txn_block(&core, core.sb.geo.journal_start + 3, &image) == VFS_CORRUPT, "journal block accepted as a home location");
    CHECK(ufs2_txn_block(&core, core.sb.geo.total, &image) == VFS_CORRUPT && ufs2_txn_block(&core, UINT32_MAX, &image) == VFS_CORRUPT, "out-of-range home location accepted");
    ufs2_txn_abort(&core);
    /* Sequence-number exhaustion is refused before any write. */
    ufs2_test_set_next_seq(&core, UINT64_MAX);
    uint32_t b0 = data_block(0);
    CHECK(txn(&b0, 1, 0x99) == VFS_NO_SPACE && block_is(b0, 0x42), "sequence overflow not refused");
    return rig_down(0);
}

/* ---- crash recovery ---- */
enum { POLICIES = 14 };
static const char *policy_name[POLICIES] = {"keep-all", "lose-all", "keep-prefix-1", "keep-prefix-2", "keep-prefix-4",
    "random-1", "random-2", "random-3", "torn-0", "torn-1", "torn-3", "keep-last-1", "keep-last-2", "keep-last-3"};
static void power_cycle(unsigned policy) {
    switch (policy) {
        case 0: block_memory_power_cycle(dev, POWER_KEEP_ALL, 0); break;
        case 1: block_memory_power_cycle(dev, POWER_LOSE_ALL, 0); break;
        case 2: block_memory_power_cycle(dev, POWER_KEEP_PREFIX, 1); break;
        case 3: block_memory_power_cycle(dev, POWER_KEEP_PREFIX, 2); break;
        case 4: block_memory_power_cycle(dev, POWER_KEEP_PREFIX, 4); break;
        case 5: block_memory_power_cycle(dev, POWER_RANDOM, 1); break;
        case 6: block_memory_power_cycle(dev, POWER_RANDOM, 2); break;
        case 7: block_memory_power_cycle(dev, POWER_RANDOM, 3); break;
        case 8: block_memory_power_cycle(dev, POWER_TORN, 0); break;
        case 9: block_memory_power_cycle(dev, POWER_TORN, 1); break;
        case 10: block_memory_power_cycle(dev, POWER_TORN, 3); break;
        case 11: block_memory_power_cycle(dev, POWER_KEEP_SUFFIX, 1); break;
        case 12: block_memory_power_cycle(dev, POWER_KEEP_SUFFIX, 2); break;
        default: block_memory_power_cycle(dev, POWER_KEEP_SUFFIX, 3); break;
    }
}

/* Failure messages that name the crash window and the loss policy. */
static char message[220];
static const char *window_fail(const char *phase, unsigned policy, const char *what) {
    unsigned n = 0;
    const char *parts[] = {"window '", phase, "' policy ", policy_name[policy], ": ", what};
    for (unsigned i = 0; i < 6; ++i) for (unsigned k = 0; parts[i][k] && n < sizeof(message) - 1; ++k) message[n++] = parts[i][k];
    message[n] = 0;
    return message;
}

/* Re-format the persistent test device and clear the data blocks the tests use. */
static const char *rig_reset(void) {
    if (core.open) ufs2_test_abandon(&core);
    block_memory_power_cycle(dev, POWER_KEEP_ALL, 0);
    if (ufs2_core_format(dev, "JTEST", UUID) != VFS_OK) return "reformat failed";
    memset(img, 0, BLOCK);
    uint32_t per = BLOCK / dev->block_size, ds = 0;
    struct ufs2_geometry g; if (!ufs2_geometry_for((uint32_t)(dev->block_count / per), &g)) return "geometry";
    ds = g.data_start;
    for (unsigned b = 0; b < 16; ++b) (void)block_write(dev, (uint64_t)(ds + b) * per, per, img, BLOCK);
    (void)block_flush(dev);
    return 0;
}

/* Three transactions over six data blocks; state j is "first j transactions applied". */
static const uint32_t S1[] = {0, 1, 2}, S2[] = {1, 3}, S3[] = {0, 4, 5};
static const uint32_t *script_set[3] = {S1, S2, S3};
static const unsigned script_len[3] = {3, 2, 3};
static const uint8_t script_tag[3] = {0x10, 0x30, 0x50};
static int expected_tag(unsigned state, unsigned block) {   /* -1 = zero block */
    int tag = -1;
    for (unsigned t = 0; t < state; ++t)
        for (unsigned i = 0; i < script_len[t]; ++i) if (script_set[t][i] == block) tag = script_tag[t] + (int)i;
    return tag;
}
static bool state_matches(unsigned state, unsigned blocks) {
    for (unsigned b = 0; b < blocks; ++b) {
        int tag = expected_tag(state, b);
        if (!block_is(data_block(b), tag < 0 ? 0xff : (uint8_t)tag)) return false;
    }
    return true;
}
static int recovered_state(void) {   /* which prefix state the disk matches, or -1 if torn */
    for (unsigned state = 0; state <= 3; ++state) if (state_matches(state, 6)) return (int)state;
    return -1;
}
static enum vfs_error run_script(unsigned *completed) {
    *completed = 0;
    for (unsigned t = 0; t < 3; ++t) {
        uint32_t blocks[3];
        for (unsigned i = 0; i < script_len[t]; ++i) blocks[i] = data_block(script_set[t][i]);
        enum vfs_error e = txn(blocks, script_len[t], script_tag[t]);
        if (e != VFS_OK) return e;
        ++*completed;
    }
    return VFS_OK;
}

/* Every I/O event of the three-transaction script x every loss policy. */
static const char *g_crash_matrix(void) {
    RIG(4096, 1024);
    unsigned runs = 0; int64_t events = -1;
    for (int64_t point = 0; point < 400; ++point) {
        bool complete = false;
        for (unsigned policy = 0; policy < POLICIES; ++policy) {
            const char *r = rig_reset();
            if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            block_memory_crash_after(dev, point);
            unsigned completed = 0;
            enum vfs_error e = run_script(&completed);
            if (!block_memory_crashed(dev)) {   /* the whole script ran before this crash point */
                CHECK(e == VFS_OK && completed == 3, "script failed without a crash");
                complete = true; events = point;
                break;
            }
            const char *phase = block_memory_crash_phase(dev);
            ufs2_test_abandon(&core);
            power_cycle(policy);
            if (ufs2_core_open(&core, dev, true) != VFS_OK) FAIL(window_fail(phase, policy, "volume did not mount after a power cut"));
            int state = recovered_state();
            if (state < 0) FAIL(window_fail(phase, policy, "recovered state is not a committed state (torn transaction)"));
            if ((unsigned)state < completed) FAIL(window_fail(phase, policy, "a transaction that had returned success was lost"));
            if (state > (int)completed + 1) FAIL(window_fail(phase, policy, "recovered state is ahead of any started transaction"));
            /* The recovered volume must be usable, and recover again. */
            uint32_t b = data_block(9);
            if (txn(&b, 1, 0x70) != VFS_OK || !block_is(b, 0x70)) FAIL(window_fail(phase, policy, "volume not writable after recovery"));
            ufs2_test_abandon(&core);
            if (ufs2_core_open(&core, dev, true) != VFS_OK || !block_is(b, 0x70)) FAIL(window_fail(phase, policy, "second recovery lost data"));
            ++runs;
        }
        if (complete) break;
    }
    CHECK(events > 30 && runs > 300, "matrix too small to be meaningful");
    return rig_down(0);
}

/* Expected outcome of a crash at the first event of each phase of one 2-block transaction. */
enum outcome { OLD, NEW, EITHER };
struct window { const char *phase; enum outcome outcome; };
static const struct window windows[] = {
    {"journal-write", OLD}, {"journal-flush", OLD}, {"commit-write", OLD}, {"commit-flush", EITHER},
    {"home-write", NEW}, {"home-flush", NEW}, {"sb-write", NEW}, {"sb-flush", NEW}};
static bool phase_is(const char *at, const char *name) { unsigned i = 0; while (name[i] && at[i] == name[i]) ++i; return !name[i] && !at[i]; }

static const char *g_crash_windows(void) {
    RIG(4096, 1024);
    uint32_t b[2];
    for (unsigned w = 0; w < sizeof(windows) / sizeof(windows[0]); ++w) {
        /* Locate the first event index of this phase for a 2-block transaction. */
        int64_t at = -1;
        for (int64_t point = 0; point < 40 && at < 0; ++point) {
            const char *r = rig_reset(); if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            b[0] = data_block(0); b[1] = data_block(1);
            block_memory_crash_after(dev, point);
            (void)txn(b, 2, 0x10);
            if (!block_memory_crashed(dev)) break;
            if (phase_is(block_memory_crash_phase(dev), windows[w].phase)) at = point;
        }
        CHECK(at >= 0, "failure window not found in the transaction");
        for (unsigned policy = 0; policy < POLICIES; ++policy) {
            const char *r = rig_reset(); if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            b[0] = data_block(0); b[1] = data_block(1);
            block_memory_crash_after(dev, at);
            enum vfs_error e = txn(b, 2, 0x10);
            if (e == VFS_OK || !block_memory_crashed(dev)) FAIL(window_fail(windows[w].phase, policy, "crash point did not fire"));
            ufs2_test_abandon(&core);
            power_cycle(policy);
            if (ufs2_core_open(&core, dev, true) != VFS_OK) FAIL(window_fail(windows[w].phase, policy, "volume did not mount"));
            bool is_old = block_is(b[0], 0xff) && block_is(b[1], 0xff), is_new = block_is(b[0], 0x10) && block_is(b[1], 0x11);
            if (!is_old && !is_new) FAIL(window_fail(windows[w].phase, policy, "transaction was torn"));
            if (windows[w].outcome == OLD && !is_old) FAIL(window_fail(windows[w].phase, policy, "uncommitted transaction became visible"));
            if (windows[w].outcome == NEW && !is_new) FAIL(window_fail(windows[w].phase, policy, "committed transaction was lost"));
            if ((w == 4 || w == 5) && core.replayed != 1) FAIL(window_fail(windows[w].phase, policy, "committed transaction was not replayed"));
        }
    }
    return rig_down(0);
}

/* Replay must be idempotent: a power cut at every event of recovery itself, then recover again. */
static const char *g_replay_idempotent(void) {
    RIG(4096, 1024);
    uint32_t b[2] = {0, 0};
    int64_t home = -1;
    for (int64_t point = 0; point < 40 && home < 0; ++point) {
        const char *r = rig_reset(); if (r) FAIL(r);
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
        b[0] = data_block(0); b[1] = data_block(1);
        block_memory_crash_after(dev, point);
        (void)txn(b, 2, 0x10);
        if (block_memory_crashed(dev) && phase_is(block_memory_crash_phase(dev), "home-write")) home = point;
    }
    CHECK(home >= 0, "home-write window not found");
    unsigned sweep = 0;
    for (int64_t k = 0; k < 60; ++k) {
        for (unsigned policy = 0; policy < POLICIES; ++policy) {
            const char *r = rig_reset(); if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            block_memory_crash_after(dev, home);
            (void)txn(b, 2, 0x10);
            ufs2_test_abandon(&core);
            block_memory_power_cycle(dev, POWER_KEEP_ALL, 0);
            /* Now recover, but lose power at event k of the recovery. */
            block_memory_crash_after(dev, k);
            enum vfs_error e = ufs2_core_open(&core, dev, true);
            bool crashed = block_memory_crashed(dev);
            if (!crashed) {
                if (e != VFS_OK) FAIL(window_fail("replay", policy, "recovery failed without a crash"));
                ufs2_test_abandon(&core);
                if (policy) continue;
                CHECK(sweep > 3, "replay sweep too small");
                return rig_down(0);
            }
            if (core.open) ufs2_test_abandon(&core);
            power_cycle(policy);
            if (ufs2_core_open(&core, dev, true) != VFS_OK) FAIL(window_fail("replay", policy, "volume did not mount after an interrupted recovery"));
            if (!block_is(b[0], 0x10) || !block_is(b[1], 0x11)) FAIL(window_fail("replay", policy, "interrupted recovery lost the committed transaction"));
            ufs2_test_abandon(&core);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && block_is(b[0], 0x10) && block_is(b[1], 0x11) && core.replayed == 0, "repeated recovery changed the result");
            ++sweep;
        }
    }
    return rig_down("replay sweep did not terminate");
}

/* Damaged journal records are never applied, and the volume stays usable. */
enum value_kind { LITERAL, FIRST_TARGET, JOURNAL_BLOCK };
struct tamper {
    const char *name;
    unsigned record;            /* ring position within the 2-block transaction: 0 descriptor, 1-2 payload, 3 commit */
    bool set;                   /* false: xor one byte; true: store a 32-bit value */
    unsigned offset; uint32_t value; enum value_kind kind;
    bool recrc;                 /* recompute the descriptor/commit checksum so only the logic check can catch it */
    bool reported;              /* expected to be reported as a damaged tail (a wrong sequence is indistinguishable from a stale record) */
};
static void tamper_apply(const struct tamper *t, uint32_t journal_start, uint32_t first_target) {
    uint32_t per = BLOCK / dev->block_size, block = journal_start + t->record;
    (void)block_read(dev, (uint64_t)block * per, per, img, BLOCK);
    uint32_t v = t->kind == FIRST_TARGET ? first_target : t->kind == JOURNAL_BLOCK ? journal_start + 5 : t->value;
    if (t->set) { img[t->offset] = (uint8_t)v; img[t->offset + 1] = (uint8_t)(v >> 8); img[t->offset + 2] = (uint8_t)(v >> 16); img[t->offset + 3] = (uint8_t)(v >> 24); }
    else img[t->offset] ^= (uint8_t)t->value;
    if (t->recrc) {
        uint32_t c = ufs2_crc32(UUID, t->record, img, 4092);
        img[4092] = (uint8_t)c; img[4093] = (uint8_t)(c >> 8); img[4094] = (uint8_t)(c >> 16); img[4095] = (uint8_t)(c >> 24);
    }
    (void)block_write(dev, (uint64_t)block * per, per, img, BLOCK);
    (void)block_flush(dev);
}
static const char *g_journal_tamper(void) {
    RIG(4096, 1024);
    struct ufs2_geometry g; ufs2_geometry_for(1024, &g);
    static const struct tamper cases[] = {
        {"payload bit flip", 1, false, 100, 1, LITERAL, false, true},
        {"commit checksum", 3, false, 4092, 1, LITERAL, false, true},
        {"commit payload crc", 3, false, 16, 1, LITERAL, false, true},
        {"descriptor checksum", 0, false, 4092, 1, LITERAL, false, true},
        {"descriptor sequence", 0, false, 8, 1, LITERAL, true, false},
        {"descriptor count zero", 0, true, 4, 0, LITERAL, true, true},
        {"descriptor count over max", 0, true, 4, 99, LITERAL, true, true},
        {"target in journal", 0, true, 24, 0, JOURNAL_BLOCK, true, true},
        {"target superblock A", 0, true, 24, 0, LITERAL, true, true},
        {"target superblock B", 0, true, 24, 1023, LITERAL, true, true},
        {"target out of range", 0, true, 24, 5000, LITERAL, true, true},
        {"duplicate targets", 0, true, 28, 0, FIRST_TARGET, true, true},
        {"reserved descriptor bytes", 0, false, 16, 1, LITERAL, true, true},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t b[2] = {0, 0};
        int64_t home = -1;
        for (int64_t point = 0; point < 40 && home < 0; ++point) {
            const char *r = rig_reset(); if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            b[0] = data_block(0); b[1] = data_block(1);
            block_memory_crash_after(dev, point);
            (void)txn(b, 2, 0x10);
            if (block_memory_crashed(dev) && phase_is(block_memory_crash_phase(dev), "home-write")) home = point;
        }
        CHECK(home >= 0, "home-write window not found");
        ufs2_test_abandon(&core);
        block_memory_power_cycle(dev, POWER_KEEP_ALL, 0);   /* a committed transaction sits in the journal, unapplied */
        tamper_apply(&cases[i], g.journal_start, b[0]);
        enum vfs_error e = ufs2_core_open(&core, dev, true);
        if (e != VFS_OK) FAIL(cases[i].name);
        if (core.replayed != 0) FAIL("a damaged transaction was replayed");
        if (cases[i].reported && !core.incomplete_tail) FAIL("a damaged transaction was not reported");
        if (!cases[i].reported && core.incomplete_tail) FAIL("a record with another sequence was reported as damage");
        if (!block_is(b[0], 0xff) || !block_is(b[1], 0xff)) FAIL("a damaged transaction reached the home locations");
        /* The volume still accepts work, reusing the damaged ring position. */
        if (txn(b, 2, 0x20) != VFS_OK || !block_is(b[1], 0x21)) FAIL("volume unusable after ignoring a damaged transaction");
        ufs2_test_abandon(&core);
        if (ufs2_core_open(&core, dev, true) != VFS_OK || !block_is(b[0], 0x20) || core.replayed != 0) FAIL("state lost after reusing the position");
    }
    /* A stale lower-sequence record at the tail is ignored. */
    const char *r = rig_reset(); if (r) FAIL(r);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
    uint32_t b[2] = {data_block(0), data_block(1)};
    CHECK(txn(b, 2, 0x10) == VFS_OK && txn(b, 2, 0x30) == VFS_OK, "setup failed");
    CHECK(ufs2_test_set_tail(&core, 0, 2) == VFS_OK, "set tail failed");
    ufs2_test_abandon(&core);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.replayed == 0 && block_is(b[0], 0x30), "stale transaction was replayed");
    return rig_down(0);
}

/* The ring wraps: a transaction that straddles the end replays correctly. */
static unsigned wrap_history(uint32_t *one, uint32_t *three) {
    /* One 1-block transaction (3 ring positions), then 3-block ones (5 positions) until the next would straddle the end. */
    unsigned count = 0;
    if (txn(one, 1, 0x01) != VFS_OK) return 0xffff;
    while (core.head_pos + 5 <= core.sb.geo.journal_blocks) {
        if (txn(three, 3, (uint8_t)(0x10 + count)) != VFS_OK) return 0xffff;
        ++count;
    }
    return count;
}
static const char *g_journal_wrap(void) {
    RIG(4096, 1024);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
    uint32_t j = core.sb.geo.journal_blocks;
    uint32_t one = data_block(0), three[3] = {data_block(1), data_block(2), data_block(3)};
    unsigned count = wrap_history(&one, three);
    CHECK(count != 0xffff && core.head_pos + 5 > j, "could not position the head near the end of the ring");
    uint32_t straddle_at = core.head_pos;
    int64_t home = -1;
    for (int64_t point = 0; point < 80 && home < 0; ++point) {   /* same history every time, crash inside the next transaction */
        const char *r = rig_reset(); if (r) FAIL(r);
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
        CHECK(wrap_history(&one, three) == count && core.head_pos == straddle_at, "history is not deterministic");
        block_memory_crash_after(dev, point);
        (void)txn(three, 3, 0x7e);
        if (block_memory_crashed(dev) && phase_is(block_memory_crash_phase(dev), "home-write")) home = point;
    }
    CHECK(home >= 0, "home-write window not found");
    ufs2_test_abandon(&core);
    block_memory_power_cycle(dev, POWER_LOSE_ALL, 0);   /* the commit was flushed, so it must survive */
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.replayed == 1, "straddling transaction not replayed");
    CHECK(block_is(three[0], 0x7e) && block_is(three[1], 0x7f) && block_is(three[2], 0x80), "replay across the wrap applied the wrong images");
    CHECK(core.head_pos == (straddle_at + 5) % j && core.head_pos < straddle_at, "the head did not wrap");
    /* Several full laps. */
    for (unsigned i = 0; i < 80; ++i) CHECK(txn(three, 3, (uint8_t)i) == VFS_OK, "commit failed during laps");
    ufs2_test_abandon(&core);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.replayed == 0 && block_is(three[0], 79) && block_is(three[2], 81), "state lost across several laps");
    return rig_down(0);
}

/* Device errors during a commit fault the volume; remounting recovers a consistent state. */
static const char *g_commit_io_errors(void) {
    RIG(4096, 1024);
    unsigned faults = 0;
    for (int32_t k = 0; k < 60; ++k) {
        const char *r = rig_reset(); if (r) FAIL(r);
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
        uint32_t b[2] = {data_block(0), data_block(1)};
        block_test_fail_after(k);
        enum vfs_error e = txn(b, 2, 0x10);
        block_test_fail_after(-1);
        if (e == VFS_OK) {
            CHECK(faults > 5 && block_is(b[1], 0x11), "sweep never failed or lost the committed data");
            return rig_down(0);
        }
        CHECK(e == VFS_IO, "injected device failure not reported as an I/O error");
        if (!core.faulted) {   /* failed while staging: nothing was written and the volume is still writable */
            CHECK(!core.read_only && block_is(b[0], 0xff) && block_is(b[1], 0xff), "failed staging changed the disk");
            CHECK(txn(b, 2, 0x20) == VFS_OK && block_is(b[1], 0x21), "volume unusable after a failed staging read");
            continue;
        }
        ++faults;
        CHECK(core.read_only && ufs2_txn_begin(&core) == VFS_READ_ONLY, "faulted volume accepted a transaction");
        CHECK(ufs2_core_close(&core) == VFS_IO, "faulted volume closed cleanly");
        CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "volume did not remount after a device error");
        bool is_old = block_is(b[0], 0xff) && block_is(b[1], 0xff), is_new = block_is(b[0], 0x10) && block_is(b[1], 0x11);
        CHECK(is_old || is_new, "device error left a torn transaction");
    }
    return rig_down("I/O error sweep did not reach success");
}

/* Sub-4096-byte device blocks (several device writes per filesystem block). */
static const char *g_small_blocks(void) {
    RIG(512, 8192);
    CHECK(ufs2_core_open(&core, dev, true) == VFS_OK && core.per == 8, "open with 512-byte blocks failed");
    uint32_t b[3] = {data_block(0), data_block(1), data_block(2)};
    CHECK(txn(b, 3, 0x10) == VFS_OK && block_is(b[2], 0x12), "commit with 512-byte blocks failed");
    ufs2_test_abandon(&core);
    for (unsigned policy = 0; policy < POLICIES; policy += 2) {
        for (int64_t point = 0; point < 120; point += 7) {
            const char *r = rig_reset(); if (r) FAIL(r);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "open failed");
            block_memory_crash_after(dev, point);
            (void)txn(b, 3, 0x10);
            bool crashed = block_memory_crashed(dev);
            ufs2_test_abandon(&core);
            power_cycle(policy);
            CHECK(ufs2_core_open(&core, dev, true) == VFS_OK, "volume did not mount after a power cut (512-byte blocks)");
            bool old = block_is(b[0], 0xff) && block_is(b[1], 0xff) && block_is(b[2], 0xff);
            bool nw = block_is(b[0], 0x10) && block_is(b[1], 0x11) && block_is(b[2], 0x12);
            CHECK(old || nw, "torn transaction with 512-byte blocks");
            if (!crashed) CHECK(nw, "completed transaction lost");
        }
    }
    return rig_down(0);
}

/* The v2 driver never treats other data as v2 and never writes during a failed open. */
static const char *g_not_v2(void) {
    RIG(4096, 1024);
    static const char v1[] = "ULTRIUMF";
    memset(img, 0, BLOCK); memcpy(img, v1, 8);      /* a v1 superblock at the start, junk at the end */
    (void)block_write(dev, 0, 1, img, BLOCK);
    (void)block_write(dev, 1023, 1, img, BLOCK);
    (void)block_flush(dev);
    uint64_t events = block_memory_events(dev);
    enum vfs_error e = ufs2_core_open(&core, dev, true);
    CHECK(e == VFS_CORRUPT && dev->holders == 0 && !core.open, "v1 or blank volume accepted as v2");
    CHECK(block_memory_events(dev) == events, "a rejected open wrote to the device");
    memset(img, 0, BLOCK);
    (void)block_write(dev, 0, 1, img, BLOCK); (void)block_write(dev, 1023, 1, img, BLOCK);
    CHECK(ufs2_core_open(&core, dev, false) == VFS_CORRUPT, "a blank disk was accepted as v2");
    return rig_down(0);
}

const struct test_case ufs2_journal_cases[] = {
    {"ufs2 geometry", g_geometry},
    {"ufs2 format and open", g_format_open},
    {"ufs2 format safety", g_format_safety},
    {"ufs2 flush and read-only policy", g_flush_policy},
    {"ufs2 superblock pair", g_superblock_pair},
    {"ufs2 superblock feature policy", g_superblock_policy},
    {"ufs2 transaction basics", g_txn_basic},
    {"ufs2 transaction limits", g_txn_limits},
    {"ufs2 crash matrix (every event x 14 loss policies)", g_crash_matrix},
    {"ufs2 crash windows (named phases)", g_crash_windows},
    {"ufs2 replay idempotence", g_replay_idempotent},
    {"ufs2 damaged journal records", g_journal_tamper},
    {"ufs2 journal wrap-around", g_journal_wrap},
    {"ufs2 commit I/O errors", g_commit_io_errors},
    {"ufs2 512-byte device blocks", g_small_blocks},
    {"ufs2 rejects non-v2 data", g_not_v2}
};
const size_t ufs2_journal_case_count = sizeof(ufs2_journal_cases) / sizeof(ufs2_journal_cases[0]);
