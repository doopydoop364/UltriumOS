#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/vfs.h"
#include "fs/ultriumfs.h"
#include "storage/block.h"
#include "memory.h"

/* Checks for the multi-boot persistence test (tools/qemu-persistence-test.py).
   Each command verifies one stage through the public VFS against the volume
   mounted at /volumes/disk. They never format or mount anything themselves,
   except `blank`, which only checks that an unformatted disk is rejected. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define MP "/volumes/disk"

static uint8_t io[4096];
static void pat(uint8_t *out, uint64_t offset, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) { uint64_t o = offset + i; out[i] = (uint8_t)(seed + o * 31U + (o >> 9)); }
}
static enum vfs_error write_pattern(const char *path, size_t total, uint8_t seed, bool create) {
    enum vfs_error e = create ? vfs_create_file(path) : VFS_OK;
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = vfs_file_open(path, VFS_OPEN_WRITE, &f);
    for (size_t off = 0; e == VFS_OK && off < total;) {
        size_t n = total - off < 3000 ? total - off : 3000, w = 0;
        pat(io, off, n, seed);
        e = vfs_file_write(&f, io, n, &w);
        if (e == VFS_OK && w != n) e = VFS_NO_SPACE;
        off += n;
    }
    if (f.generation) { enum vfs_error c = vfs_file_close(&f); if (e == VFS_OK) e = c; }
    return e;
}
static bool check_pattern(const char *path, size_t total, uint8_t seed) {
    struct vfs_info st;
    if (vfs_stat(path, &st) != VFS_OK || st.type != VFS_NODE_FILE || st.size != total) return false;
    struct vfs_file f = {0};
    if (vfs_file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return false;
    static uint8_t expect[1000];
    size_t off = 0; bool ok = true;
    for (;;) {
        size_t n = 0;
        if (vfs_file_read(&f, io, 1000, &n) != VFS_OK) { ok = false; break; }
        if (!n) break;
        pat(expect, off, n, seed);
        if (memcmp(io, expect, n)) { ok = false; break; }
        off += n;
    }
    (void)vfs_file_close(&f);
    return ok && off == total;
}
static bool check_text(const char *path, const char *text) {
    size_t len = 0; while (text[len]) ++len;
    struct vfs_info st;
    if (vfs_stat(path, &st) != VFS_OK || st.type != VFS_NODE_FILE || st.size != len) return false;
    struct vfs_file f = {0}; size_t n = 0;
    if (vfs_file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return false;
    enum vfs_error e = vfs_file_read(&f, io, sizeof(io), &n);
    (void)vfs_file_close(&f);
    return e == VFS_OK && n == len && !memcmp(io, text, len);
}
static bool absent(const char *path) { struct vfs_info st; return vfs_stat(path, &st) == VFS_NOT_FOUND; }
static bool dir_has(const char *path, unsigned expected_entries) {
    struct vfs_dir d = {0};
    if (vfs_dir_open(path, &d) != VFS_OK) return false;
    char name[40]; enum vfs_node_type t; unsigned n = 0;
    while (vfs_dir_next(&d, name, sizeof(name), &t) == VFS_OK) ++n;
    (void)vfs_dir_close(&d);
    return n == expected_entries;
}

static const char *mounted_volume(void) {
    struct ufs_info info;
    REQUIRE(ufs_mounted() && ufs_get_info(&info), "no UltriumFS volume is mounted");
    struct vfs_info st;
    REQUIRE(vfs_stat(MP, &st) == VFS_OK && st.persistent && st.filesystem && st.filesystem[0] == 'U', "mount point is not served by UltriumFS");
    REQUIRE(!info.faulted && !info.read_only, "volume is faulted or read-only");
    return 0;
}

static const char *persist_blank(void) {
    struct block_device *d = 0;
    for (size_t i = 0; i < block_count_devices(); ++i) if (block_device_at(i)->flags & BLOCK_FLAG_SCRATCH) d = block_device_at(i);
    REQUIRE(d, "no scratch disk attached");
    REQUIRE(!ufs_mounted() && d->holders == 0, "the disk is already in use");
    for (uint64_t b = 0; b < 16 && b < d->block_count; ++b) {   /* block 0 must still be blank */
        REQUIRE(block_read(d, b, 1, io, d->block_size) == BLOCK_OK, "read failed");
        for (uint32_t i = 0; i < d->block_size; ++i) REQUIRE(!io[i], "disk is not blank: it was written without an explicit format");
    }
    REQUIRE(ufs_mount(d, false) == VFS_CORRUPT, "an unformatted disk was accepted as UltriumFS");
    REQUIRE(!ufs_mounted() && d->holders == 0, "a rejected mount left the disk claimed");
    return 0;
}

static const char *persist_populate(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(write_pattern(MP "/big.bin", 20000, 11, true) == VFS_OK, "big.bin write failed");
    REQUIRE(write_pattern(MP "/medium.bin", 4096, 12, true) == VFS_OK, "medium.bin write failed");
    REQUIRE(write_pattern(MP "/projects/ultrium/odd.bin", 5000, 13, true) == VFS_OK, "odd.bin write failed");
    return 0;
}

/* Stage 1 state: created in boot one, expected unchanged in boot two. */
static const char *s1_tree(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(dir_has(MP, 5), "volume root does not list the 5 expected entries");
    REQUIRE(dir_has(MP "/projects", 2) && dir_has(MP "/projects/ultrium", 2), "nested folders wrong");
    REQUIRE(vfs_is_directory(MP "/projects/ultrium"), "nested folder missing");
    return 0;
}
static const char *s1_small(void) {
    REQUIRE(check_text(MP "/projects/notes.txt", "Persistent data"), "notes.txt wrong");
    REQUIRE(check_text(MP "/projects/ultrium/readme", "Boot one"), "readme wrong");
    REQUIRE(check_text(MP "/old-notes.txt", "Persistent data"), "renamed copy wrong");
    struct vfs_info st;
    REQUIRE(vfs_stat(MP "/empty.txt", &st) == VFS_OK && st.type == VFS_NODE_FILE && st.size == 0, "empty file wrong");
    return 0;
}
static const char *s1_big(void) {
    REQUIRE(check_pattern(MP "/big.bin", 20000, 11), "multi-block big.bin wrong");
    REQUIRE(check_pattern(MP "/medium.bin", 4096, 12), "medium.bin wrong");
    REQUIRE(check_pattern(MP "/projects/ultrium/odd.bin", 5000, 13), "nested odd.bin wrong");
    return 0;
}
static const char *s1_removed(void) {
    REQUIRE(absent(MP "/backup.txt"), "renamed source still present");
    REQUIRE(absent(MP "/tmp.txt"), "removed file still present");
    REQUIRE(absent(MP "/trash") && absent(MP "/trash/a"), "removed folder still present");
    return 0;
}

static const char *persist_modify(void) {
    const char *r = mounted_volume();
    if (r) return r;
    struct vfs_file f = {0}; size_t n = 0;
    REQUIRE(vfs_file_open(MP "/projects/notes.txt", VFS_OPEN_WRITE, &f) == VFS_OK, "open notes.txt failed");
    enum vfs_error e = vfs_file_write(&f, "PERSISTENT", 10, &n);   /* in-place update */
    REQUIRE(vfs_file_close(&f) == VFS_OK && e == VFS_OK && n == 10, "in-place update failed");
    REQUIRE(write_pattern(MP "/big.bin", 30000, 21, false) == VFS_OK, "growing big.bin failed");
    REQUIRE(vfs_unlink(MP "/old-notes.txt") == VFS_OK, "removing old-notes.txt failed");
    return 0;
}
/* Stage 3 state: after boot two's modifications (boot2.txt is written with the shell). */
static const char *s3_changes(void) {
    const char *r = mounted_volume();
    if (r) return r;
    REQUIRE(check_text(MP "/projects/notes.txt", "PERSISTENT data"), "in-place update lost");
    REQUIRE(check_text(MP "/boot2.txt", "Second boot"), "boot2.txt lost");
    REQUIRE(absent(MP "/old-notes.txt"), "removed file reappeared");
    return 0;
}
static const char *s3_growth(void) {
    REQUIRE(check_pattern(MP "/big.bin", 30000, 21), "grown big.bin wrong");
    REQUIRE(check_pattern(MP "/medium.bin", 4096, 12) && check_pattern(MP "/projects/ultrium/odd.bin", 5000, 13), "untouched files damaged");
    REQUIRE(check_text(MP "/projects/ultrium/readme", "Boot one"), "untouched readme damaged");
    return 0;
}
static const char *persist_clean(void) {
    struct ufs_info info;
    REQUIRE(ufs_get_info(&info), "no volume mounted");
    REQUIRE(!info.unclean_mount, "the previous session was not shut down cleanly");
    REQUIRE(info.orphan_inodes == 0, "orphaned items present");
    return 0;
}
static const char *persist_unclean(void) {
    struct ufs_info info;
    REQUIRE(ufs_get_info(&info), "no volume mounted");
    REQUIRE(info.unclean_mount, "an abrupt stop was not reported as an unclean session");
    return 0;
}
static const char *persist_after_crash(void) {
    REQUIRE(check_text(MP "/crash.txt", "written before the stop"), "data written before the abrupt stop was lost");
    return 0;
}

const struct test_case persist_blank_cases[] = {{"unformatted disk is rejected, never auto-formatted", persist_blank}};
const struct test_case persist_populate_cases[] = {{"populate multi-block files", persist_populate}};
const struct test_case persist_stage1_cases[] = {
    {"stage 1: folder tree", s1_tree}, {"stage 1: small files, copy, rename", s1_small},
    {"stage 1: multi-block files", s1_big}, {"stage 1: removed items stay removed", s1_removed}};
const struct test_case persist_modify_cases[] = {{"modify: update, grow and remove", persist_modify}};
const struct test_case persist_stage3_cases[] = {
    {"stage 3: changes persisted", s3_changes}, {"stage 3: file growth persisted", s3_growth}};
const struct test_case persist_clean_cases[] = {{"previous session was clean", persist_clean}};
const struct test_case persist_unclean_cases[] = {{"abrupt stop reported as unclean", persist_unclean}, {"data written before the stop survived", persist_after_crash}};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
const size_t persist_blank_count = COUNT(persist_blank_cases), persist_populate_count = COUNT(persist_populate_cases),
    persist_stage1_count = COUNT(persist_stage1_cases), persist_modify_count = COUNT(persist_modify_cases),
    persist_stage3_count = COUNT(persist_stage3_cases), persist_clean_count = COUNT(persist_clean_cases),
    persist_unclean_count = COUNT(persist_unclean_cases);
