#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/ufs2_fs.h"
#include "fs/crc32c.h"
#include "storage/block.h"
#include "memory/pmm.h"
#include "memory.h"

/* Recovery matrix for UltriumFS 2 file operations. For every operation, a power
   cut is injected before every I/O event it performs (writes and flushes), under
   several loss policies of the volatile-cache model (see docs/ultriumfs-v2-crash-testing.md).
   After each cut the volume is remounted (replaying the journal) and must:
     - mount read-write and verify with ZERO findings (no leaked or doubly owned blocks, no orphans),
     - equal exactly the tree before the operation or the tree after it (atomicity),
     - equal the "after" tree when the operation had already returned success,
     - remain usable and consistent for a following change.
   Growth operations are chunked (one transaction per chunk), so they may land on
   any chunk boundary; their content is checked byte for byte instead. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define BLOCK 4096U
#define POLICIES 8

static const struct vfs_backend_ops *fs;
static struct block_device *dev;
static const uint8_t UUID[16] = {0xc0, 0xff, 0xee, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static struct ufs2_report report;
static uint8_t chunk[BLOCK], expect[BLOCK], bigbuf[64 * BLOCK];

static void pat(uint8_t *out, uint64_t offset, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) { uint64_t o = offset + i; out[i] = (uint8_t)(seed + o * 31U + (o >> 9)); }
}
static enum vfs_error write_stream(const char *path, uint64_t total, uint8_t seed, bool create) {
    enum vfs_error e = create ? fs->create_file(path) : VFS_OK;
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = fs->file_open(path, VFS_OPEN_WRITE, &f);
    if (e != VFS_OK) return e;
    for (uint64_t off = 0; off < total && e == VFS_OK;) {
        size_t n = sizeof(bigbuf) < total - off ? sizeof(bigbuf) : (size_t)(total - off), w = 0;
        pat(bigbuf, off, n, seed);
        e = fs->file_write(&f, bigbuf, n, &w);
        off += w;
        if (e == VFS_OK && w != n) { e = VFS_IO; break; }
    }
    enum vfs_error c = fs->file_close(&f);
    return e != VFS_OK ? e : c;
}

/* ---- tree digest: order-independent hash of every path, type, size and file content ---- */
static uint32_t entry_hash(const char *path, int type, uint64_t size, uint32_t content) {
    uint32_t c = crc32c_begin();
    unsigned n = 0; while (path[n]) ++n;
    c = crc32c_update(c, path, n);
    uint8_t tail[13] = {(uint8_t)type, (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24),
                        (uint8_t)(size >> 32), 0, 0, 0, (uint8_t)content, (uint8_t)(content >> 8), (uint8_t)(content >> 16), (uint8_t)(content >> 24)};
    return crc32c_end(crc32c_update(c, tail, sizeof(tail)));
}
static uint32_t file_crc(const char *path) {
    struct vfs_file f = {0};
    if (fs->file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return 0xdeadbeef;
    uint32_t c = crc32c_begin();
    for (;;) {
        size_t got = 0;
        if (fs->file_read(&f, chunk, 1000, &got) != VFS_OK) { c ^= 0x55; break; }
        if (!got) break;
        c = crc32c_update(c, chunk, got);
    }
    (void)fs->file_close(&f);
    return crc32c_end(c);
}
static bool same(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static uint32_t tree_digest(const char *exclude) {
    static char stack[16][64];
    unsigned top = 0;
    stack[top][0] = '/'; stack[top][1] = 0; ++top;
    uint32_t sum = 0;
    while (top) {
        char dir[64]; unsigned dl = 0;
        for (; stack[top - 1][dl]; ++dl) dir[dl] = stack[top - 1][dl];
        dir[dl] = 0; --top;
        struct vfs_dir d = {0};
        if (fs->dir_open(dir, &d) != VFS_OK) return 0xbadbad00;
        char name[40]; enum vfs_node_type type;
        while (fs->dir_next(&d, name, sizeof(name), &type) == VFS_OK) {
            char child[64]; unsigned n = 0;
            for (unsigned i = 0; dir[i]; ++i) child[n++] = dir[i];
            if (n != 1) child[n++] = '/';
            for (unsigned i = 0; name[i] && n < 63; ++i) child[n++] = name[i];
            child[n] = 0;
            if (exclude && same(child, exclude)) continue;
            struct vfs_info st;
            if (fs->stat(child, &st) != VFS_OK) { sum += 0x1111; continue; }
            sum += entry_hash(child, type == VFS_NODE_DIRECTORY, type == VFS_NODE_DIRECTORY ? 0 : st.size,
                              type == VFS_NODE_DIRECTORY ? 0 : file_crc(child));
            if (type == VFS_NODE_DIRECTORY && top < 16) { unsigned k = 0; for (; child[k]; ++k) stack[top][k] = child[k]; stack[top][k] = 0; ++top; }
        }
        (void)fs->dir_close(&d);
    }
    return sum;
}
static bool verifier_clean(void) {
    struct ufs2_core *c = ufs2_fs_test_core();
    return c && ufs2_verify(c, &report) == VFS_OK && !report.count[0] && !report.count[1] && !report.count[2];
}
static bool mount_rw(void) {
    struct ufs2_fs_info info;
    return ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && !info.read_only && !info.repair_recommended;
}

/* ---- the operations ---- */
static enum vfs_error op_create(void) { return fs->create_file("/d/new"); }
static enum vfs_error op_mkdir(void) { return fs->create_directory("/d/sub"); }
static enum vfs_error op_unlink(void) { return fs->unlink("/f1"); }
static enum vfs_error op_rmdir(void) { return fs->unlink("/d/empty"); }
static enum vfs_error op_rename_same(void) { return fs->rename("/f2", "/f2b"); }
static enum vfs_error op_rename_cross(void) { return fs->rename("/f2", "/d/f2"); }
static enum vfs_error op_rename_dir(void) { return fs->rename("/d/inner", "/inner2"); }
static enum vfs_error op_create_extend(void) { return fs->create_file("/full/e63"); }
static enum vfs_error op_grow(void) { return write_stream("/f2", 70U * BLOCK + 5, 2, false); }
static enum vfs_error op_promote(void) { return write_stream("/gF", 12U * BLOCK, 31, false); }
static enum vfs_error op_unlink_fragmented(void) { return fs->unlink("/gG"); }

struct op { const char *name; enum vfs_error (*run)(void); const char *grown; uint8_t seed; uint64_t old_size, new_size; };
static const struct op ops[] = {
    {"create file", op_create, 0, 0, 0, 0},
    {"mkdir", op_mkdir, 0, 0, 0, 0},
    {"unlink file", op_unlink, 0, 0, 0, 0},
    {"rmdir", op_rmdir, 0, 0, 0, 0},
    {"rename in place", op_rename_same, 0, 0, 0, 0},
    {"rename across directories", op_rename_cross, 0, 0, 0, 0},
    {"rename a directory", op_rename_dir, 0, 0, 0, 0},
    {"create in a full directory", op_create_extend, 0, 0, 0, 0},
    {"unlink a fragmented file", op_unlink_fragmented, 0, 0, 0, 0},
    {"grow a file over two chunks", op_grow, "/f2", 2, 5000, 70U * BLOCK + 5},
    {"grow past 11 extents", op_promote, "/gF", 31, 11U * BLOCK, 12U * BLOCK},
};

static void power_cycle(unsigned policy) {
    switch (policy) {
        case 0: block_memory_power_cycle(dev, POWER_KEEP_ALL, 0); break;
        case 1: block_memory_power_cycle(dev, POWER_LOSE_ALL, 0); break;
        case 2: block_memory_power_cycle(dev, POWER_KEEP_PREFIX, 2); break;
        case 3: block_memory_power_cycle(dev, POWER_TORN, 1); break;
        case 4: block_memory_power_cycle(dev, POWER_RANDOM, 1); break;
        case 5: block_memory_power_cycle(dev, POWER_RANDOM, 2); break;
        case 6: block_memory_power_cycle(dev, POWER_KEEP_SUFFIX, 1); break;
        default: block_memory_power_cycle(dev, POWER_KEEP_SUFFIX, 3); break;
    }
}
static const char *policy_name[POLICIES] = {"keep-all", "lose-all", "keep-prefix-2", "torn-1", "random-1", "random-2", "keep-last-1", "keep-last-3"};

static char message[240];
static const char *fail(const char *op, int64_t point, const char *phase, unsigned policy, const char *what) {
    unsigned n = 0;
    const char *parts[] = {"op '", op, "' event ", 0, " phase '", phase, "' policy ", policy_name[policy], ": ", what};
    char num[12]; unsigned nd = 0; uint64_t v = (uint64_t)point;
    do { num[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 11);
    char digits[12]; for (unsigned i = 0; i < nd; ++i) digits[i] = num[nd - 1 - i]; digits[nd] = 0;
    parts[3] = digits;
    for (unsigned i = 0; i < 10; ++i) for (unsigned k = 0; parts[i][k] && n < sizeof(message) - 1; ++k) message[n++] = parts[i][k];
    message[n] = 0;
    return message;
}

/* Fixture, built once and snapshotted. */
static const char *build_fixture(void) {
    if (ufs2_fs_format(dev, "CRASH", UUID) != VFS_OK || !mount_rw()) return "fixture format/mount failed";
    if (fs->create_directory("/d") != VFS_OK || fs->create_directory("/d/inner") != VFS_OK || fs->create_directory("/d/empty") != VFS_OK) return "fixture dirs failed";
    if (write_stream("/f1", 3U * BLOCK, 1, true) != VFS_OK) return "fixture f1 failed";
    if (write_stream("/f2", 5000, 2, true) != VFS_OK) return "fixture f2 failed";
    if (fs->create_directory("/full") != VFS_OK) return "fixture full dir failed";
    char name[16] = "/full/e00";
    for (unsigned i = 0; i < 63; ++i) { name[7] = (char)('0' + i / 10); name[8] = (char)('0' + i % 10); if (fs->create_file(name) != VFS_OK) return "fixture full-directory entries failed"; }
    /* Two files written alternately: /gF ends up with 11 separate one-block extents. */
    if (fs->create_file("/gF") != VFS_OK || fs->create_file("/gG") != VFS_OK) return "fixture fragmentation files failed";
    for (unsigned i = 0; i < 11; ++i) {
        if (write_stream("/gF", (uint64_t)(i + 1) * BLOCK, 31, false) != VFS_OK || write_stream("/gG", (uint64_t)(i + 1) * BLOCK, 47, false) != VFS_OK) return "fixture fragmentation failed";
    }
    if (ufs2_fs_sync() != VFS_OK) return "fixture sync failed";
    ufs2_fs_test_abandon();
    return 0;
}

static const char *run_matrix(unsigned first_op, unsigned last_op, unsigned policies, unsigned long *runs_out) {
    unsigned long runs = 0;
    for (unsigned o = first_op; o < last_op; ++o) {
        const struct op *op = &ops[o];
        block_memory_restore(dev);
        if (!mount_rw()) return "fixture does not mount";
        uint32_t old_digest = tree_digest(0), old_rest = op->grown ? tree_digest(op->grown) : 0;
        block_memory_crash_after(dev, -1);
        if (op->run() != VFS_OK) return "the operation fails without a crash";
        uint64_t total_events = block_memory_events(dev);
        uint32_t new_digest = tree_digest(0);
        if (new_digest == old_digest) return "the operation changed nothing";
        ufs2_fs_test_abandon();
        if (total_events < 6) return "the operation performed suspiciously few I/O events";
        for (int64_t point = 0; point <= (int64_t)total_events; ++point) {
            for (unsigned policy = 0; policy < policies; ++policy) {
                block_memory_restore(dev);
                if (!mount_rw()) return fail(op->name, point, "", policy, "fixture does not mount");
                block_memory_crash_after(dev, point);
                enum vfs_error r = op->run();
                bool crashed = block_memory_crashed(dev);
                const char *phase = crashed ? block_memory_crash_phase(dev) : "none";
                ufs2_fs_test_abandon();
                power_cycle(policy);
                if (!mount_rw()) return fail(op->name, point, phase, policy, "the volume does not mount read-write after the power cut");
                if (!verifier_clean()) return fail(op->name, point, phase, policy, "the recovered volume has findings (leak, double ownership, orphan or damaged metadata)");
                if (!op->grown) {
                    uint32_t d = tree_digest(0);
                    if (d != old_digest && d != new_digest) return fail(op->name, point, phase, policy, "the recovered tree is neither the old nor the new state (operation not atomic)");
                    if (!crashed && d != new_digest) return fail(op->name, point, phase, policy, "an operation that returned success was lost");
                    if (!crashed && r != VFS_OK) return fail(op->name, point, phase, policy, "operation failed without a crash");
                } else {
                    if (tree_digest(op->grown) != old_rest) return fail(op->name, point, phase, policy, "an unrelated file changed");
                    struct vfs_info st;
                    if (fs->stat(op->grown, &st) != VFS_OK) return fail(op->name, point, phase, policy, "the grown file disappeared");
                    if (st.size < op->old_size || st.size > op->new_size) return fail(op->name, point, phase, policy, "the grown file has an impossible size");
                    if (!crashed && st.size != op->new_size) return fail(op->name, point, phase, policy, "a completed write was lost");
                    /* Ordered data: every byte below the committed size must be the data that was written. */
                    struct vfs_file f = {0};
                    if (fs->file_open(op->grown, VFS_OPEN_READ, &f) != VFS_OK) return fail(op->name, point, phase, policy, "cannot open the grown file");
                    uint64_t off = 0; bool bad = false;
                    for (;;) {
                        size_t got = 0;
                        if (fs->file_read(&f, chunk, BLOCK, &got) != VFS_OK) { bad = true; break; }
                        if (!got) break;
                        pat(expect, off, got, op->seed);
                        if (memcmp(chunk, expect, got)) { bad = true; break; }
                        off += got;
                    }
                    (void)fs->file_close(&f);
                    if (bad || off != st.size) return fail(op->name, point, phase, policy, "committed file data is not the data that was written (ordered-data violation)");
                }
                /* The recovered volume stays usable, consistent and recoverable. */
                if (fs->create_file("/zz-after") != VFS_OK || !verifier_clean() || fs->unlink("/zz-after") != VFS_OK) return fail(op->name, point, phase, policy, "the recovered volume is not usable");
                ufs2_fs_test_abandon();
                ++runs;
            }
        }
    }
    *runs_out += runs;
    return 0;
}

static unsigned long total_runs;
static const char *matrix_setup(void) {
    fs = ufs2_fs_backend();
    dev = block_memory_create("u2crash", 4096, 1024, false);
    REQUIRE(dev, "memory device creation failed");
    if (!block_memory_enable_cache(dev)) return "cache model unavailable";
    const char *r = build_fixture();
    if (r) return r;
    if (!block_memory_snapshot(dev)) return "snapshot unavailable";
    return 0;
}
static const char *matrix_down(const char *result) {
    if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
    block_set_phase("");
    if (dev) { if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    return result;
}

#define MATRIX_CASE(name_, first_, last_, policies_) \
    static const char *name_(void) { \
        uint64_t frames = pmm_free_pages(); \
        const char *r = matrix_setup(); \
        if (!r) r = run_matrix(first_, last_, policies_, &total_runs); \
        r = matrix_down(r); \
        if (!r && pmm_free_pages() != frames) r = "frame accounting changed"; \
        return r; }
MATRIX_CASE(m_create_remove, 0, 4, POLICIES)
MATRIX_CASE(m_rename, 4, 7, POLICIES)
MATRIX_CASE(m_directory_extension_and_fragmented, 7, 9, POLICIES)
MATRIX_CASE(m_growth, 9, 11, 5)

const struct test_case ufs2_crash_cases[] = {
    {"ufs2 recovery matrix: create, mkdir, unlink, rmdir", m_create_remove},
    {"ufs2 recovery matrix: renames", m_rename},
    {"ufs2 recovery matrix: directory extension, fragmented unlink", m_directory_extension_and_fragmented},
    {"ufs2 recovery matrix: chunked growth and extent promotion", m_growth}
};
const size_t ufs2_crash_case_count = sizeof(ufs2_crash_cases) / sizeof(ufs2_crash_cases[0]);
unsigned long ufs2_crash_total_runs(void) { return total_runs; }
