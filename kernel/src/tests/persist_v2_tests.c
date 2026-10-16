#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/vfs.h"
#include "storage/storage_ops.h"
#include "storage/block.h"
#include "memory.h"

/* Commands for the UltriumFS 2 recovery QEMU test (tools/qemu-ufs2-recovery-test.py), run against the
   volume mounted at /volumes/disk. `big` builds a larger fixture (file over 4 MiB, fragmented file,
   nested folders, renames, block reuse); `cut-*` arms a deterministic power cut at a journal phase on
   the REAL virtio device (every later transfer and flush fails) and performs one operation; `stream`
   writes a large file until the host kills QEMU. The host then decodes the disk image independently. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define MP "/volumes/disk"
#define LARGE (6U * 1024U * 1024U)
#define FRAG_BLOCKS 40U
#define REUSE 163840U
#define STREAM_TOTAL (40U * 1024U * 1024U)

static uint8_t io[64 * 1024], expect[64 * 1024];
static void pat(uint8_t *out, uint64_t offset, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) { uint64_t o = offset + i; out[i] = (uint8_t)(seed + o * 31U + (o >> 9)); }
}
/* Rewrites the file from offset 0 up to `total` bytes (the part below the current size is an in-place overwrite). */
static enum vfs_error write_pattern(const char *path, uint64_t total, uint8_t seed, bool create, bool progress) {
    enum vfs_error e = create ? vfs_create_file(path) : VFS_OK;
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = vfs_file_open(path, VFS_OPEN_WRITE, &f);
    for (uint64_t off = 0; e == VFS_OK && off < total;) {
        size_t n = total - off < sizeof(io) ? (size_t)(total - off) : sizeof(io);
        size_t w = 0;
        pat(io, off, n, seed);
        e = vfs_file_write(&f, io, n, &w);
        if (e == VFS_OK && w != n) e = VFS_NO_SPACE;
        off += n;
        if (progress && e == VFS_OK && (off % (1024U * 1024U)) == 0) { tests_serial_text("STREAM "); tests_serial_number(off / (1024U * 1024U)); tests_serial_text("\n"); }
    }
    if (f.generation) { enum vfs_error c = vfs_file_close(&f); if (e == VFS_OK) e = c; }
    return e;
}
static bool check_pattern(const char *path, uint64_t total, uint8_t seed, bool prefix_ok) {
    struct vfs_info st;
    if (vfs_stat(path, &st) != VFS_OK || st.type != VFS_NODE_FILE) return false;
    if (prefix_ok ? st.size > total : st.size != total) return false;
    struct vfs_file f = {0};
    if (vfs_file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return false;
    uint64_t off = 0; bool ok = true;
    for (;;) {
        size_t n = 0;
        if (vfs_file_read(&f, io, sizeof(io), &n) != VFS_OK) { ok = false; break; }
        if (!n) break;
        pat(expect, off, n, seed);
        if (memcmp(io, expect, n)) { ok = false; break; }
        off += n;
    }
    (void)vfs_file_close(&f);
    return ok && off == st.size;
}
static bool present(const char *path, bool dir) {
    struct vfs_info st;
    return vfs_stat(path, &st) == VFS_OK && (st.type == VFS_NODE_DIRECTORY) == dir;
}
static bool absent(const char *path) { struct vfs_info st; return vfs_stat(path, &st) == VFS_NOT_FOUND; }
static const char *mounted_volume(void) {
    struct storage_volume vol;
    REQUIRE(storage_volume_status(&vol) && vol.version == 2, "no UltriumFS 2 volume is mounted");
    REQUIRE(!vol.faulted && !vol.read_only && !vol.repair_recommended, "volume is faulted, read-only or needs repair");
    return 0;
}

static const char *v2_big(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(vfs_create_directory(MP "/deep") == VFS_OK && vfs_create_directory(MP "/deep/a") == VFS_OK &&
            vfs_create_directory(MP "/deep/a/b") == VFS_OK && vfs_create_directory(MP "/deep/a/b/c") == VFS_OK, "nested folders failed");
    REQUIRE(write_pattern(MP "/large.bin", LARGE, 41, true, false) == VFS_OK, "6 MiB file failed");
    /* Two files grown alternately, one block at a time: both end up as many separate extents. */
    REQUIRE(vfs_create_file(MP "/frag1.bin") == VFS_OK && vfs_create_file(MP "/frag2.bin") == VFS_OK, "fragment files failed");
    for (uint32_t i = 1; i <= FRAG_BLOCKS; ++i)
        REQUIRE(write_pattern(MP "/frag1.bin", (uint64_t)i * 4096, 51, false, false) == VFS_OK &&
                write_pattern(MP "/frag2.bin", (uint64_t)i * 4096, 52, false, false) == VFS_OK, "fragmenting writes failed");
    REQUIRE(vfs_create_file(MP "/deep/a/b/c/note.txt") == VFS_OK, "nested file failed");
    struct vfs_file f = {0}; size_t n = 0;
    REQUIRE(vfs_file_open(MP "/deep/a/b/c/note.txt", VFS_OPEN_WRITE, &f) == VFS_OK && vfs_file_write(&f, "nested", 6, &n) == VFS_OK && n == 6 && vfs_file_close(&f) == VFS_OK, "nested write failed");
    REQUIRE(vfs_rename(MP "/deep/a/b/c/note.txt", MP "/deep/a/moved.txt") == VFS_OK, "cross-directory rename failed");
    REQUIRE(vfs_unlink(MP "/frag2.bin") == VFS_OK, "unlink failed");
    REQUIRE(write_pattern(MP "/reuse.bin", REUSE, 53, true, false) == VFS_OK, "write into freed blocks failed");
    REQUIRE(vfs_rename(MP "/deep/a/b", MP "/deep/b2") == VFS_OK, "directory rename failed");
    return 0;
}
static const char *v2_bigverify(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(present(MP "/deep/a", true) && present(MP "/deep/b2", true) && present(MP "/deep/b2/c", true) && present(MP "/deep/a/moved.txt", false), "nested tree wrong");
    REQUIRE(absent(MP "/deep/a/b") && absent(MP "/frag2.bin"), "renamed or removed items still present");
    REQUIRE(check_pattern(MP "/large.bin", LARGE, 41, false), "file larger than 4 MiB wrong");
    REQUIRE(check_pattern(MP "/frag1.bin", FRAG_BLOCKS * 4096ULL, 51, false), "fragmented file wrong");
    REQUIRE(check_pattern(MP "/reuse.bin", REUSE, 53, false), "file written into reused blocks wrong");
    return 0;
}

/* ---- deterministic power cuts on the real device ---- */
static const char *cut(const char *phase, bool growth) {
    const char *r = mounted_volume();
    if (r) return r;
    block_arm_power_cut(phase, 1, 0);
    enum vfs_error e = growth ? write_pattern(MP "/reuse.bin", REUSE + 20000U, 53, false, false)
                              : vfs_rename(MP "/frag1.bin", MP "/deep/frag1.bin");
    bool fired = block_power_is_cut();
    /* The cut stays in force: no further transfer reaches the disk. The host now stops QEMU and decodes the image. */
    REQUIRE(fired, "the power cut did not fire");
    REQUIRE(e != VFS_OK, "an operation reported success although power was lost during it");
    return 0;
}
static const char *v2_cut_dataflush(void) { return cut("data-flush", true); }
static const char *v2_cut_journal(void) { return cut("journal-write", false); }
static const char *v2_cut_commit(void) { return cut("commit-write", false); }
static const char *v2_cut_home(void) { return cut("home-write", false); }
static const char *v2_cut_checkpoint(void) { return cut("checkpoint", false); }

static const char *v2_recovered(void) {
    struct storage_volume vol;
    REQUIRE(storage_volume_status(&vol), "no volume mounted");
    REQUIRE(vol.unclean, "the interrupted session was not reported as unclean");
    REQUIRE(!vol.faulted && !vol.read_only && !vol.repair_recommended && vol.orphans == 0, "the recovered volume is not a healthy read-write volume");
    return 0;
}
static const char *v2_stream(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(write_pattern(MP "/stream.bin", STREAM_TOTAL, 61, true, true) == VFS_OK, "stream write failed");
    return 0;
}
static const char *v2_streamverify(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(check_pattern(MP "/stream.bin", STREAM_TOTAL, 61, true), "the interrupted stream file is not a prefix of the data that was written");
    REQUIRE(check_pattern(MP "/large.bin", LARGE, 41, false) && check_pattern(MP "/reuse.bin", REUSE, 53, false), "unrelated files damaged");
    return 0;
}

const struct test_case persist_v2_big_cases[] = {{"v2: build large, fragmented and nested fixture", v2_big}};
const struct test_case persist_v2_bigverify_cases[] = {{"v2: large, fragmented, nested, renamed and reused data", v2_bigverify}};
const struct test_case persist_v2_cut_dataflush_cases[] = {{"v2: power cut after data write, before its flush and commit", v2_cut_dataflush}};
const struct test_case persist_v2_cut_journal_cases[] = {{"v2: power cut before the journal write", v2_cut_journal}};
const struct test_case persist_v2_cut_commit_cases[] = {{"v2: power cut after journal payload, before the commit record", v2_cut_commit}};
const struct test_case persist_v2_cut_home_cases[] = {{"v2: power cut after commit, before the home-location writes", v2_cut_home}};
const struct test_case persist_v2_cut_checkpoint_cases[] = {{"v2: power cut after home writes, before the checkpoint", v2_cut_checkpoint}};
const struct test_case persist_v2_recovered_cases[] = {{"v2: interrupted session recovered read-write", v2_recovered}};
const struct test_case persist_v2_stream_cases[] = {{"v2: stream a 40 MiB file", v2_stream}};
const struct test_case persist_v2_streamverify_cases[] = {{"v2: interrupted stream is a valid prefix", v2_streamverify}};
