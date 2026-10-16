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
/* Zero findings of any class (leaks, double ownership, orphans, damaged metadata) and allocation
   accounting: the mount's free-block count must agree with the blocks the inodes actually own. */
static bool verifier_clean(void) {
    struct ufs2_core *c = ufs2_fs_test_core();
    struct ufs2_fs_info info;
    if (!c || ufs2_verify(c, &report) != VFS_OK || report.count[0] || report.count[1] || report.count[2]) return false;
    return ufs2_fs_get_info(&info) && info.total_blocks - info.free_blocks == report.blocks_owned && !info.orphans;
}
static bool mount_rw(void) {
    struct ufs2_fs_info info;
    return ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && !info.read_only && !info.repair_recommended;
}

/* ---- the operations: each is a list of steps (VFS calls), each step one or more journal transactions ---- */
static enum vfs_error op_create(void) { return fs->create_file("/d/new"); }
static enum vfs_error op_mkdir(void) { return fs->create_directory("/d/sub"); }
static enum vfs_error op_unlink(void) { return fs->unlink("/f1"); }
static enum vfs_error op_rmdir(void) { return fs->unlink("/d/empty"); }
static enum vfs_error op_rename_same(void) { return fs->rename("/f2", "/f2b"); }
static enum vfs_error op_rename_cross(void) { return fs->rename("/f2", "/d/f2"); }
static enum vfs_error op_rename_dir(void) { return fs->rename("/d/inner", "/inner2"); }
static enum vfs_error op_rename_into_full(void) { return fs->rename("/f2", "/full/f2"); }
static enum vfs_error op_create_extend(void) { return fs->create_file("/full/e63"); }
static enum vfs_error op_grow(void) { return write_stream("/f2", 70U * BLOCK + 5, 2, false); }
static enum vfs_error op_promote(void) { return write_stream("/gF", 12U * BLOCK, 31, false); }
static enum vfs_error op_unlink_fragmented(void) { return fs->unlink("/gG"); }
static enum vfs_error op_create_n1(void) { return fs->create_file("/n1"); }
static enum vfs_error op_create_h1(void) { return fs->create_file("/h1"); }
static enum vfs_error op_write_h1(void) { return write_stream("/h1", 11U * BLOCK, 61, false); }
static enum vfs_error op_sync(void) { return ufs2_fs_sync(); }
static enum vfs_error op_unmount(void) { return ufs2_fs_unmount(); }

#define MAX_STEPS 3
/* A step list is NOT atomic as a whole: the recovered tree may be the state before the step that was
   running when power failed or the state after it. A `grown` file is written in chunks (one transaction
   each), so it is excluded from the tree digests and checked separately: once `grown_from` steps have
   completed it exists, its size lies between old_size and new_size, and every byte is the written data. */
struct op { const char *name; enum vfs_error (*step[MAX_STEPS])(void); const char *grown; uint8_t seed; uint64_t old_size, new_size; unsigned grown_from; };
enum { O_CREATE, O_MKDIR, O_UNLINK, O_RMDIR, O_RENAME, O_RENAME_CROSS, O_RENAME_DIR, O_RENAME_FULL, O_FULL_DIR, O_FRAG_UNLINK,
       O_GROW, O_PROMOTE, O_INODE_REUSE, O_BLOCK_REUSE, O_UNMOUNT };
static const struct op ops[] = {
    [O_CREATE] = {"create file", {op_create}, 0, 0, 0, 0, 0},
    [O_MKDIR] = {"mkdir", {op_mkdir}, 0, 0, 0, 0, 0},
    [O_UNLINK] = {"unlink file", {op_unlink}, 0, 0, 0, 0, 0},
    [O_RMDIR] = {"rmdir", {op_rmdir}, 0, 0, 0, 0, 0},
    [O_RENAME] = {"rename in place", {op_rename_same}, 0, 0, 0, 0, 0},
    [O_RENAME_CROSS] = {"rename across directories", {op_rename_cross}, 0, 0, 0, 0, 0},
    [O_RENAME_DIR] = {"rename a directory", {op_rename_dir}, 0, 0, 0, 0, 0},
    [O_RENAME_FULL] = {"rename into a full directory", {op_rename_into_full}, 0, 0, 0, 0, 0},
    [O_FULL_DIR] = {"create in a full directory", {op_create_extend}, 0, 0, 0, 0, 0},
    [O_FRAG_UNLINK] = {"unlink a fragmented file", {op_unlink_fragmented}, 0, 0, 0, 0, 0},
    [O_GROW] = {"grow a file over two chunks", {op_grow}, "/f2", 2, 5000, 70U * BLOCK + 5, 0},
    [O_PROMOTE] = {"grow past 11 extents", {op_promote}, "/gF", 31, 11U * BLOCK, 12U * BLOCK, 0},
    [O_INODE_REUSE] = {"unlink then create (inode reuse)", {op_unlink, op_create_n1}, 0, 0, 0, 0, 0},
    [O_BLOCK_REUSE] = {"unlink, create, write into freed blocks", {op_unlink_fragmented, op_create_h1, op_write_h1}, "/h1", 61, 0, 11U * BLOCK, 2},
    [O_UNMOUNT] = {"create, sync, unmount", {op_create, op_sync, op_unmount}, 0, 0, 0, 0, 0},
};
#define OP_COUNT (sizeof(ops) / sizeof(ops[0]))
#define BIT(o) (1U << (o))

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

static char message[260];
static void put_number(char *out, int64_t point) {
    char num[12]; unsigned nd = 0; uint64_t v = (uint64_t)point;
    do { num[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 11);
    for (unsigned i = 0; i < nd; ++i) out[i] = num[nd - 1 - i];
    out[nd] = 0;
}
static const char *fail(const char *op, int64_t point, const char *phase, unsigned policy, const char *what) {
    unsigned n = 0;
    char digits[12]; put_number(digits, point);
    const char *parts[] = {"op '", op, "' event ", digits, " phase '", phase, "' policy ", policy_name[policy], ": ", what};
    for (unsigned i = 0; i < 10; ++i) for (unsigned k = 0; parts[i][k] && n < sizeof(message) - 1; ++k) message[n++] = parts[i][k];
    message[n] = 0;
    return message;
}

/* Fixture, built once and snapshotted. `head` >= 0 positions the journal ring head so that the
   operations under test straddle the end of the ring (the ring is empty between operations). */
static const char *build_fixture(int32_t head) {
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
    if (head >= 0) {
        struct ufs2_core *c = ufs2_fs_test_core();
        if (!c || head >= (int32_t)c->sb.geo.journal_blocks || ufs2_test_set_tail(c, (uint32_t)head, c->next_seq) != VFS_OK) return "fixture journal position failed";
    }
    ufs2_fs_test_abandon();
    return 0;
}

/* Run an operation's steps; stops at the first failure. Returns the number of steps that completed. */
static unsigned run_steps(const struct op *op, enum vfs_error *last) {
    unsigned done = 0;
    *last = VFS_OK;
    for (unsigned i = 0; i < MAX_STEPS && op->step[i]; ++i) {
        *last = op->step[i]();
        if (*last != VFS_OK) break;
        ++done;
    }
    return done;
}
static unsigned step_count(const struct op *op) { unsigned n = 0; while (n < MAX_STEPS && op->step[n]) ++n; return n; }
static void abandon_if_mounted(void) { if (ufs2_fs_mounted()) ufs2_fs_test_abandon(); }

struct discovery { uint32_t allowed[MAX_STEPS + 1]; uint64_t events; };
static const char *discover(const struct op *op, struct discovery *dis) {
    block_memory_restore(dev);
    if (!mount_rw()) return "fixture does not mount";
    dis->allowed[0] = tree_digest(op->grown);
    block_memory_crash_after(dev, -1);
    unsigned n = step_count(op);
    for (unsigned i = 0; i < n; ++i) {
        if (op->step[i]() != VFS_OK) return "the operation fails without a crash";
        dis->allowed[i + 1] = ufs2_fs_mounted() ? tree_digest(op->grown) : dis->allowed[i];
    }
    dis->events = block_memory_events(dev);
    struct vfs_info st;
    bool grown_ok = !op->grown || (ufs2_fs_mounted() && fs->stat(op->grown, &st) == VFS_OK && st.size == op->new_size);
    abandon_if_mounted();
    if (!grown_ok) return "the grown file does not reach its final size";
    if (!op->grown && dis->allowed[n] == dis->allowed[0]) return "the operation changed nothing";
    if (dis->events < 6) return "the operation performed suspiciously few I/O events";
    return 0;
}

/* The recovered volume must be consistent, usable, and in an allowed state. Returns an error text or 0. */
static const char *check_recovered(const struct op *op, const struct discovery *dis, unsigned done, bool crashed, enum vfs_error r,
                                   int64_t point, const char *phase, unsigned policy) {
    if (!mount_rw()) return fail(op->name, point, phase, policy, "the volume does not mount read-write after the power cut");
    if (!verifier_clean()) return fail(op->name, point, phase, policy, "the recovered volume has findings (leak, double ownership, orphan, accounting or damaged metadata)");
    unsigned n = step_count(op);
    uint32_t d = tree_digest(op->grown);
    bool ok = d == dis->allowed[done] || (done + 1 <= n && d == dis->allowed[done + 1]);
    if (crashed && !ok) return fail(op->name, point, phase, policy, "the recovered tree is not an allowed state (a step lost or half-applied, or an unrelated file changed)");
    if (!crashed && d != dis->allowed[n]) return fail(op->name, point, phase, policy, "an operation that returned success was lost");
    if (!crashed && r != VFS_OK) return fail(op->name, point, phase, policy, "operation failed without a crash");
    if (op->grown) {
        struct vfs_info st;
        if (fs->stat(op->grown, &st) != VFS_OK) {
            if (done >= op->grown_from) return fail(op->name, point, phase, policy, "the grown file disappeared");
        } else {
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
    }
    /* The recovered volume stays usable, consistent and recoverable. */
    if (fs->create_file("/zz-after") != VFS_OK || !verifier_clean() || fs->unlink("/zz-after") != VFS_OK) return fail(op->name, point, phase, policy, "the recovered volume is not usable");
    ufs2_fs_test_abandon();
    return 0;
}

static const char *run_matrix(unsigned mask, unsigned policies, unsigned long *runs_out) {
    unsigned long runs = 0;
    for (unsigned o = 0; o < OP_COUNT; ++o) {
        if (!(mask & BIT(o))) continue;
        const struct op *op = &ops[o];
        struct discovery dis;
        const char *r0 = discover(op, &dis);
        if (r0) return r0;
        for (int64_t point = 0; point <= (int64_t)dis.events; ++point) {
            for (unsigned policy = 0; policy < policies; ++policy) {
                block_memory_restore(dev);
                if (!mount_rw()) return fail(op->name, point, "", policy, "fixture does not mount");
                block_memory_crash_after(dev, point);
                enum vfs_error r;
                unsigned done = run_steps(op, &r);
                bool crashed = block_memory_crashed(dev);
                const char *phase = crashed ? block_memory_crash_phase(dev) : "none";
                abandon_if_mounted();
                power_cycle(policy);
                const char *bad = check_recovered(op, &dis, done, crashed, r, point, phase, policy);
                if (bad) return bad;
                ++runs;
            }
        }
    }
    *runs_out += runs;
    return 0;
}

/* Power cut at every I/O event of a mount (superblock "mounted" update, replay of a clean journal). */
static const char *mount_matrix(unsigned policies, unsigned long *runs_out) {
    block_memory_restore(dev);
    block_memory_crash_after(dev, -1);
    if (!mount_rw()) return "fixture does not mount";
    uint32_t old = tree_digest(0);
    uint64_t events = block_memory_events(dev);
    abandon_if_mounted();
    if (!events) return "mount performed no I/O";
    for (int64_t point = 0; point <= (int64_t)events; ++point) {
        for (unsigned policy = 0; policy < policies; ++policy) {
            block_memory_restore(dev);
            block_memory_crash_after(dev, point);
            (void)ufs2_fs_mount(dev, true, &report);
            const char *phase = block_memory_crashed(dev) ? block_memory_crash_phase(dev) : "none";
            abandon_if_mounted();
            power_cycle(policy);
            if (!mount_rw()) return fail("mount", point, phase, policy, "the volume does not mount after a cut during mount");
            if (!verifier_clean() || tree_digest(0) != old) return fail("mount", point, phase, policy, "a cut during mount changed or damaged the volume");
            ufs2_fs_test_abandon();
            ++*runs_out;
        }
    }
    return 0;
}

/* Power cut during the journal replay that follows a power cut. The first cut leaves a committed,
   not yet checkpointed transaction; the mount then replays it and is itself interrupted at every
   I/O event, under several policies. Replay must be idempotent and finish on the next mount. */
static struct block_device *dev2;
static const unsigned nested_first[2] = {0, 4};      /* first-cut policies: keep-all, random-1 */
static const unsigned nested_second[3] = {1, 4, 6};  /* replay-cut policies: lose-all, random-1, keep-last-1 */
static bool committed_phase(const char *phase) {
    return same(phase, "commit-flush") || same(phase, "home-write") || same(phase, "home-flush") || same(phase, "checkpoint");
}
static const char *replay_matrix(unsigned mask, unsigned long *runs_out) {
    for (unsigned o = 0; o < OP_COUNT; ++o) {
        if (!(mask & BIT(o))) continue;
        const struct op *op = &ops[o];
        struct discovery dis;
        const char *r0 = discover(op, &dis);
        if (r0) return r0;
        for (int64_t point = 0; point <= (int64_t)dis.events; ++point) {
            for (unsigned a = 0; a < 2; ++a) {
                unsigned p1 = nested_first[a];
                block_memory_restore(dev);
                if (!mount_rw()) return fail(op->name, point, "", p1, "fixture does not mount");
                block_memory_crash_after(dev, point);
                enum vfs_error r;
                unsigned done = run_steps(op, &r);
                bool crashed = block_memory_crashed(dev);
                const char *phase = crashed ? block_memory_crash_phase(dev) : "none";
                abandon_if_mounted();
                if (!crashed || !committed_phase(phase)) continue;
                power_cycle(p1);
                if (!block_memory_copy(dev2, dev)) return "cannot copy the crashed image";
                /* How many I/O events does the replay mount perform? */
                block_memory_crash_after(dev, -1);
                if (ufs2_fs_mount(dev, true, &report) != VFS_OK) return fail(op->name, point, phase, p1, "the first recovery mount failed");
                uint64_t replay_events = block_memory_events(dev);
                abandon_if_mounted();
                for (int64_t q = 0; q < (int64_t)replay_events; ++q) {
                    for (unsigned b = 0; b < 3; ++b) {
                        unsigned p2 = nested_second[b];
                        if (!block_memory_copy(dev, dev2)) return "cannot restore the crashed image";
                        block_memory_crash_after(dev, q);
                        (void)ufs2_fs_mount(dev, true, &report);
                        const char *phase2 = block_memory_crashed(dev) ? block_memory_crash_phase(dev) : "none";
                        abandon_if_mounted();
                        power_cycle(p2);
                        const char *bad = check_recovered(op, &dis, done, true, VFS_IO, q, phase2, p2);
                        if (bad) return bad;
                        ++*runs_out;
                    }
                }
            }
        }
    }
    return 0;
}

static unsigned long total_runs;
static const char *matrix_setup(int32_t head, bool second) {
    fs = ufs2_fs_backend();
    dev = block_memory_create("u2crash", 4096, 1024, false);
    REQUIRE(dev, "memory device creation failed");
    if (second) { dev2 = block_memory_create("u2crash2", 4096, 1024, false); REQUIRE(dev2, "second memory device creation failed"); }
    if (!block_memory_enable_cache(dev)) return "cache model unavailable";
    const char *r = build_fixture(head);
    if (r) return r;
    if (!block_memory_snapshot(dev)) return "snapshot unavailable";
    return 0;
}
static const char *matrix_down(const char *result) {
    abandon_if_mounted();
    block_set_phase("");
    if (dev2) { if (!block_memory_destroy(dev2) && !result) result = "second device destroy failed"; dev2 = 0; }
    if (dev) { if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    return result;
}

enum kind { K_MATRIX, K_MOUNT, K_REPLAY };
static const char *run_case(enum kind kind, unsigned mask, unsigned policies, int32_t head) {
    uint64_t frames = pmm_free_pages();
    const char *r = matrix_setup(head, kind == K_REPLAY);
    if (!r) r = kind == K_MATRIX ? run_matrix(mask, policies, &total_runs) : kind == K_MOUNT ? mount_matrix(policies, &total_runs) : replay_matrix(mask, &total_runs);
    r = matrix_down(r);
    if (!r && pmm_free_pages() != frames) r = "frame accounting changed";
    return r;
}

static const char *m_create_remove(void) { return run_case(K_MATRIX, BIT(O_CREATE) | BIT(O_MKDIR) | BIT(O_UNLINK) | BIT(O_RMDIR), POLICIES, -1); }
static const char *m_rename(void) { return run_case(K_MATRIX, BIT(O_RENAME) | BIT(O_RENAME_CROSS) | BIT(O_RENAME_DIR) | BIT(O_RENAME_FULL), POLICIES, -1); }
static const char *m_extension(void) { return run_case(K_MATRIX, BIT(O_FULL_DIR) | BIT(O_FRAG_UNLINK), POLICIES, -1); }
static const char *m_growth(void) { return run_case(K_MATRIX, BIT(O_GROW) | BIT(O_PROMOTE), 5, -1); }
static const char *m_reuse(void) { return run_case(K_MATRIX, BIT(O_INODE_REUSE) | BIT(O_BLOCK_REUSE), POLICIES, -1); }
static const char *m_sync_unmount(void) { return run_case(K_MATRIX, BIT(O_UNMOUNT), POLICIES, -1); }
static const char *m_mount(void) { return run_case(K_MOUNT, 0, POLICIES, -1); }
/* Ring head 59 of 60: every transaction's descriptor, payload and commit straddle the wrap. */
static const char *m_wrap_end(void) { return run_case(K_MATRIX, BIT(O_CREATE) | BIT(O_RENAME_CROSS) | BIT(O_UNLINK) | BIT(O_INODE_REUSE), 5, 59); }
static const char *m_wrap_mid(void) { return run_case(K_MATRIX, BIT(O_FULL_DIR) | BIT(O_GROW) | BIT(O_PROMOTE) | BIT(O_UNMOUNT), 4, 52); }
static const char *m_replay(void) { return run_case(K_REPLAY, BIT(O_CREATE) | BIT(O_RENAME_CROSS) | BIT(O_UNLINK) | BIT(O_FULL_DIR), 0, -1); }
static const char *m_replay_wrap(void) { return run_case(K_REPLAY, BIT(O_CREATE) | BIT(O_MKDIR), 0, 58); }
static const char *m_replay_growth(void) { return run_case(K_REPLAY, BIT(O_PROMOTE) | BIT(O_BLOCK_REUSE), 0, -1); }

const struct test_case ufs2_crash_cases[] = {
    {"ufs2 recovery matrix: create, mkdir, unlink, rmdir", m_create_remove},
    {"ufs2 recovery matrix: renames (in place, across, directory, into a full one)", m_rename},
    {"ufs2 recovery matrix: directory extension, fragmented unlink", m_extension},
    {"ufs2 recovery matrix: chunked growth and extent-map promotion", m_growth},
    {"ufs2 recovery matrix: inode reuse and block reuse", m_reuse},
    {"ufs2 recovery matrix: sync, unmount and superblock updates", m_sync_unmount},
    {"ufs2 recovery matrix: interrupted mount", m_mount},
    {"ufs2 recovery matrix: journal ring wrap-around (head at the end)", m_wrap_end},
    {"ufs2 recovery matrix: journal ring wrap-around (mid-ring)", m_wrap_mid},
    {"ufs2 recovery matrix: power cut during journal replay", m_replay},
    {"ufs2 recovery matrix: power cut during replay across the ring end", m_replay_wrap},
    {"ufs2 recovery matrix: power cut during replay of growth and reuse", m_replay_growth},
};
const size_t ufs2_crash_case_count = sizeof(ufs2_crash_cases) / sizeof(ufs2_crash_cases[0]);
unsigned long ufs2_crash_total_runs(void) { return total_runs; }
