#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/ufs2_fs.h"
#include "fs/ufs2_meta.h"
#include "fs/ufs2_check.h"
#include "fs/ultriumfs.h"
#include "fs/crc32c.h"
#include "ush.h"
#include "storage/storage_ops.h"
#include "storage/block.h"
#include "memory/pmm.h"
#include "memory.h"

/* UltriumFS 2 filesystem tests: operations through the backend, extents,
   directories, verification and refusal of damaged volumes. Corruption cases
   edit the unmounted device with the metadata encoders. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define BLOCK 4096U

static const struct vfs_backend_ops *fs;
static struct block_device *dev;
static uint64_t frames0;
static const uint8_t UUID[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
static uint8_t chunk[BLOCK], expect[BLOCK], raw[BLOCK];
static struct ufs2_report report;

static const char *setup(uint32_t block_size, uint64_t device_blocks, bool cache) {
    fs = ufs2_fs_backend();
    frames0 = pmm_free_pages();
    dev = block_memory_create("u2fs", block_size, device_blocks, false);
    REQUIRE(dev, "memory device creation failed");
    if (cache && !block_memory_enable_cache(dev)) return "cache model unavailable";
    if (ufs2_fs_format(dev, "FSTEST", UUID) != VFS_OK) return "format failed";
    if (ufs2_fs_mount(dev, true, &report) != VFS_OK) return "mount failed";
    return 0;
}
static const char *teardown(const char *result) {
    if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
    block_set_phase("");
    if (dev) { if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    if (!result && pmm_free_pages() != frames0) result = "frame accounting changed";
    return result;
}
#define SETUP(bs, blocks) do { const char *s_ = setup(bs, blocks, false); if (s_) return teardown(s_); } while (0)
#define FAIL(text) return teardown(text)
#define CHECK(cond, text) do { if (!(cond)) FAIL(text); } while (0)

static void pat(uint8_t *out, uint64_t offset, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; ++i) { uint64_t o = offset + i; out[i] = (uint8_t)(seed + o * 31U + (o >> 9)); }
}
static enum vfs_error stream_write(const char *path, uint64_t total, uint8_t seed, size_t step, bool create) {
    enum vfs_error e = create ? fs->create_file(path) : VFS_OK;
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = fs->file_open(path, VFS_OPEN_WRITE, &f);
    if (e != VFS_OK) return e;
    for (uint64_t off = 0; off < total && e == VFS_OK;) {
        size_t n = step; if (n > BLOCK) n = BLOCK; if (n > total - off) n = (size_t)(total - off);
        pat(chunk, off, n, seed);
        size_t w = 0;
        e = fs->file_write(&f, chunk, n, &w);
        if (e == VFS_OK && w != n) e = VFS_IO;
        off += n;
    }
    enum vfs_error c = fs->file_close(&f);
    return e != VFS_OK ? e : c;
}
/* Large writes in one call (exercises the 64-block chunking). */
static uint8_t big[64 * BLOCK];
static enum vfs_error stream_write_big(const char *path, uint64_t total, uint8_t seed, bool create) {
    enum vfs_error e = create ? fs->create_file(path) : VFS_OK;
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = fs->file_open(path, VFS_OPEN_WRITE, &f);
    if (e != VFS_OK) return e;
    for (uint64_t off = 0; off < total && e == VFS_OK;) {
        size_t n = sizeof(big) < total - off ? sizeof(big) : (size_t)(total - off), w = 0;
        pat(big, off, n, seed);
        e = fs->file_write(&f, big, n, &w);
        if (e == VFS_OK && w != n) e = VFS_IO;
        off += n;
    }
    enum vfs_error c = fs->file_close(&f);
    return e != VFS_OK ? e : c;
}
static const char *stream_verify(const char *path, uint64_t total, uint8_t seed) {
    struct vfs_info info;
    if (fs->stat(path, &info) != VFS_OK || info.type != VFS_NODE_FILE || info.size != total) return "size or type mismatch";
    struct vfs_file f = {0};
    if (fs->file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return "open for verify failed";
    const char *r = 0; uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        enum vfs_error e = fs->file_read(&f, chunk, 3000, &got);
        if (e != VFS_OK) { r = "read failed"; break; }
        if (!got) break;
        pat(expect, off, got, seed);
        if (memcmp(chunk, expect, got)) { r = "content mismatch"; break; }
        off += got;
    }
    if (!r && off != total) r = "short read";
    (void)fs->file_close(&f);
    return r;
}
static unsigned count_entries(const char *path, bool *has_dir) {
    struct vfs_dir d = {0};
    if (fs->dir_open(path, &d) != VFS_OK) return 9999;
    char name[40]; enum vfs_node_type t; unsigned n = 0;
    while (fs->dir_next(&d, name, sizeof(name), &t) == VFS_OK) { ++n; if (has_dir && t == VFS_NODE_DIRECTORY) *has_dir = true; }
    (void)fs->dir_close(&d);
    return n;
}
/* The mounted volume must verify with no findings at all. */
static bool clean(void) {
    struct ufs2_core *c = ufs2_fs_test_core();
    return c && ufs2_verify(c, &report) == VFS_OK && !report.count[0] && !report.count[1] && !report.count[2];
}
static bool remount(bool want_write) {
    if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
    return ufs2_fs_mount(dev, want_write, &report) == VFS_OK;
}

/* On-disk inspection helpers (the volume is mounted; reads see the committed state). */
static struct ufs2_geometry G;
static void refresh_geometry(void) { ufs2_geometry_for((uint32_t)(dev->block_count / (BLOCK / dev->block_size)), &G); }
static bool find_file_inode(uint64_t size, struct ufs2_inode *out) {
    refresh_geometry();
    uint32_t per = BLOCK / dev->block_size;
    for (uint32_t t = 0; t < G.inode_blocks; ++t) {
        (void)block_read(dev, (uint64_t)(G.inode_start + t) * per, per, raw, BLOCK);
        for (unsigned k = 0; k < UFS2_INODES_PER_BLOCK; ++k) {
            struct ufs2_inode in;
            if (ufs2_inode_decode(UUID, t * UFS2_INODES_PER_BLOCK + k + 1, raw + k * UFS2_INODE_SIZE, &in) != VFS_OK) continue;
            if (in.type == UFS2_FILE && in.size == size) { *out = in; return true; }
        }
    }
    return false;
}

/* ---- basics ---- */
static const char *f_format_mount(void) {
    SETUP(4096, 2048);
    struct ufs2_fs_info info;
    CHECK(ufs2_fs_get_info(&info) && !info.read_only && !info.unclean && !info.repair_recommended, "mount state wrong");
    CHECK(info.label[0] == 'F' && !memcmp(info.label, "FSTEST", 7), "label not stored");
    CHECK(info.free_blocks == info.total_blocks - 1 && info.free_inodes == info.inode_count - 1 && info.orphans == 0, "free-space accounting wrong after format");
    CHECK(clean(), "fresh volume does not verify cleanly");
    struct vfs_info st;
    CHECK(fs->stat("/", &st) == VFS_OK && st.type == VFS_NODE_DIRECTORY && count_entries("/", 0) == 0, "root wrong");
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_BUSY, "second mount accepted");
    CHECK(ufs2_fs_format(dev, "X", UUID) == VFS_BUSY, "format of a mounted volume accepted");
    CHECK(!block_memory_destroy(dev) && dev->holders == 1, "mounted device could be destroyed");
    CHECK(ufs2_fs_unmount() == VFS_OK && dev->holders == 0, "unmount failed");
    return teardown(0);
}
static const char *f_tree_errors(void) {
    SETUP(4096, 2048);
    CHECK(fs->create_directory("/projects") == VFS_OK && fs->create_directory("/projects/ultrium") == VFS_OK, "mkdir failed");
    CHECK(fs->create_file("/projects/ultrium/notes.txt") == VFS_OK && fs->create_file("/empty") == VFS_OK, "create failed");
    CHECK(fs->create_file("/empty") == VFS_EXISTS && fs->create_directory("/projects") == VFS_EXISTS, "duplicate accepted");
    CHECK(fs->create_file("/missing/x") == VFS_NOT_FOUND && fs->create_file("/empty/x") == VFS_NOT_DIRECTORY, "bad parent accepted");
    CHECK(fs->create_file("/abcdefghijklmnopqrstuvwxyz0123456") == VFS_INVALID_PATH, "32-character name accepted");
    CHECK(fs->create_file("/abcdefghijklmnopqrstuvwxyz01234") == VFS_OK, "31-character name rejected");
    CHECK(fs->create_file("/") == VFS_INVALID_PATH && fs->unlink("/") == VFS_INVALID_PATH, "root create/unlink accepted");
    struct vfs_info st;
    CHECK(fs->stat("/projects/ultrium/notes.txt", &st) == VFS_OK && st.type == VFS_NODE_FILE && st.size == 0, "empty file stat wrong");
    CHECK(fs->stat("/nothing", &st) == VFS_NOT_FOUND, "stat of a missing path succeeded");
    bool has_dir = false;
    CHECK(count_entries("/", &has_dir) == 3 && has_dir && count_entries("/projects", 0) == 1, "directory listing wrong");
    struct vfs_file f = {0}; struct vfs_dir d = {0};
    CHECK(fs->file_open("/projects", VFS_OPEN_READ, &f) == VFS_IS_DIRECTORY && fs->dir_open("/empty", &d) == VFS_NOT_DIRECTORY, "type confusion accepted");
    CHECK(fs->unlink("/projects") == VFS_NOT_EMPTY, "non-empty directory removed");
    CHECK(clean(), "volume not clean after creating a tree");
    CHECK(fs->unlink("/projects/ultrium/notes.txt") == VFS_OK && fs->unlink("/projects/ultrium") == VFS_OK && fs->unlink("/projects") == VFS_OK, "bottom-up removal failed");
    CHECK(fs->unlink("/projects") == VFS_NOT_FOUND && clean(), "removal left the volume inconsistent");
    struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info) && info.free_inodes == info.inode_count - 3, "inode accounting wrong after removal");
    return teardown(0);
}
static const char *f_file_io(void) {
    SETUP(4096, 2048);
    CHECK(fs->create_file("/small") == VFS_OK, "create failed");
    struct vfs_file f = {0}; size_t n;
    CHECK(fs->file_open("/small", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
    CHECK(fs->file_write(&f, "Persistent data", 15, &n) == VFS_OK && n == 15 && fs->file_write(&f, " and more", 9, &n) == VFS_OK, "write failed");
    CHECK(fs->file_read(&f, chunk, 4, &n) == VFS_BAD_HANDLE, "read through a write-only handle accepted");
    CHECK(fs->file_close(&f) == VFS_OK && fs->file_close(&f) == VFS_BAD_HANDLE && fs->file_write(&f, "x", 1, &n) == VFS_BAD_HANDLE, "handle lifetime wrong");
    CHECK(fs->file_open("/small", VFS_OPEN_READ, &f) == VFS_OK && fs->file_read(&f, chunk, 100, &n) == VFS_OK && n == 24 && !memcmp(chunk, "Persistent data and more", 24), "read back wrong");
    CHECK(fs->file_read(&f, chunk, 100, &n) == VFS_OK && n == 0 && fs->file_close(&f) == VFS_OK, "EOF wrong");
    CHECK(fs->file_open("/small", VFS_OPEN_WRITE, &f) == VFS_OK && fs->file_write(&f, "PERSISTENT", 10, &n) == VFS_OK && fs->file_close(&f) == VFS_OK, "overwrite failed");
    CHECK(fs->file_open("/small", VFS_OPEN_READ, &f) == VFS_OK && fs->file_read(&f, chunk, 100, &n) == VFS_OK && n == 24 && !memcmp(chunk, "PERSISTENT data and more", 24), "overwrite content wrong");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    CHECK(stream_write("/multi", 3 * BLOCK + 1234, 5, 3000, true) == VFS_OK, "multi-block write failed");
    { const char *r = stream_verify("/multi", 3 * BLOCK + 1234, 5); CHECK(!r, r); }
    CHECK(stream_write("/big", 40 * BLOCK + 77, 9, 1777, true) == VFS_OK, "write failed");
    { const char *r = stream_verify("/big", 40 * BLOCK + 77, 9); CHECK(!r, r); }
    CHECK(stream_write("/big", 60 * BLOCK, 9, 4096, false) == VFS_OK, "growth write failed");   /* rewrite larger: overwrite then append */
    { const char *r = stream_verify("/big", 60 * BLOCK, 9); CHECK(!r, r); }
    CHECK(clean(), "volume not clean after file I/O");
    return teardown(0);
}
static const char *f_large_file(void) {
    SETUP(4096, 3072);                                         /* 12 MiB device */
    uint64_t size = 5U * 1024 * 1024 + 4321;                   /* above UltriumFS 1's 4 MiB limit */
    CHECK(stream_write_big("/large", size, 7, true) == VFS_OK, "5 MiB file write failed");
    { const char *r = stream_verify("/large", size, 7); CHECK(!r, r); }
    CHECK(clean(), "volume not clean after a large file");
    CHECK(remount(true), "remount failed");
    { const char *r = stream_verify("/large", size, 7); CHECK(!r, r); }
    struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info), "info failed");
    uint64_t used = info.total_blocks - info.free_blocks;
    CHECK(used >= size / BLOCK && used <= size / BLOCK + 3, "extent allocation used an unexpected number of blocks");
    CHECK(report.extents <= 6, "an unfragmented file used many extents");
    CHECK(fs->unlink("/large") == VFS_OK && clean(), "removal of a large file failed");
    CHECK(ufs2_fs_get_info(&info) && info.free_blocks == info.total_blocks - 1, "blocks not reclaimed after deleting a large file");
    return teardown(0);
}
static const char *f_fragmentation(void) {
    SETUP(4096, 2048);
    char name[16] = "/s000";
    unsigned count = 360;
    for (unsigned i = 0; i < count; ++i) {   /* one-block files back to back, then free every other one */
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        CHECK(stream_write(name, BLOCK, (uint8_t)i, BLOCK, true) == VFS_OK, "small file write failed");
    }
    for (unsigned i = 0; i < count; i += 2) {
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        CHECK(fs->unlink(name) == VFS_OK, "unlink failed");
    }
    CHECK(clean(), "volume not clean after freeing alternate blocks");
    struct ufs2_fs_info before; CHECK(ufs2_fs_get_info(&before), "info failed");
    uint64_t size = 175U * BLOCK + 100;                        /* 176 blocks: single-block holes first */
    CHECK(stream_write_big("/frag", size, 3, true) == VFS_OK, "write into fragmented space failed");
    { const char *r = stream_verify("/frag", size, 3); CHECK(!r, r); }
    CHECK(clean(), "volume not clean after a fragmented write");
    struct ufs2_inode in;
    CHECK(find_file_inode(size, &in) && in.depth == 1 && in.extent_count > 11 && in.extent_count <= UFS2_LEAF_EXTENTS, "the file did not use a one-leaf extent map");
    CHECK(in.block_count == 176 + 1, "block count must be the data blocks plus the leaf");
    for (unsigned i = 1; i < count; i += 2) {                  /* surviving small files are intact */
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        const char *r = stream_verify(name, BLOCK, (uint8_t)i); CHECK(!r, r);
    }
    CHECK(remount(true) && clean(), "remount failed");
    CHECK(fs->unlink("/frag") == VFS_OK && clean(), "removal of a fragmented file failed");
    struct ufs2_fs_info after;
    CHECK(ufs2_fs_get_info(&after) && after.free_blocks == before.free_blocks, "deleting the fragmented file did not return every block (data and leaf)");
    return teardown(0);
}
static const char *f_many_extents(void) {
    SETUP(4096, 3072);
    char name[16] = "/t000";
    unsigned count = 640;
    for (unsigned i = 0; i < count; ++i) {
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        enum vfs_error e = stream_write(name, BLOCK, (uint8_t)i, BLOCK, true);
        if (e == VFS_NO_SPACE) { count = i; break; }
        CHECK(e == VFS_OK, "small file write failed");
    }
    CHECK(count >= 500, "could not create enough files to fragment the volume");
    unsigned freed = 0;
    for (unsigned i = 0; i < count; i += 2, ++freed) {
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        CHECK(fs->unlink(name) == VFS_OK, "unlink failed");
    }
    CHECK(freed > 260, "not enough holes to exceed one extent leaf");
    struct ufs2_fs_info before; CHECK(ufs2_fs_get_info(&before), "info failed");
    uint64_t blocks = (uint64_t)freed + 40, size = (blocks - 1) * BLOCK + 7;   /* more than the holes: ends in a contiguous run */
    CHECK(stream_write_big("/many", size, 5, true) == VFS_OK, "write needing more than 253 extents failed");
    { const char *r = stream_verify("/many", size, 5); CHECK(!r, r); }
    struct ufs2_inode in;
    CHECK(find_file_inode(size, &in) && in.depth == 1 && in.extent_count > UFS2_LEAF_EXTENTS && in.root_entries >= 2, "the extent map did not span two leaf blocks");
    CHECK(in.block_count == blocks + in.root_entries, "block count must be the data blocks plus one block per leaf");
    CHECK(clean(), "volume not clean with a two-leaf extent map");
    CHECK(remount(true) && clean(), "remount failed");
    { const char *r = stream_verify("/many", size, 5); CHECK(!r, r); }
    CHECK(fs->unlink("/many") == VFS_OK && clean(), "removal of a file with two leaf blocks failed");
    struct ufs2_fs_info after;
    CHECK(ufs2_fs_get_info(&after) && after.free_blocks == before.free_blocks, "leaf blocks were not reclaimed");
    return teardown(0);
}
static const char *f_disk_full(void) {
    SETUP(4096, 1024);
    char name[16] = "/f000";
    unsigned made = 0; enum vfs_error e = VFS_OK;
    for (; made < 250 && e == VFS_OK; ++made) {
        name[2] = (char)('0' + made / 100); name[3] = (char)('0' + made / 10 % 10); name[4] = (char)('0' + made % 10);
        e = stream_write(name, 64 * 1024, (uint8_t)made, BLOCK, true);
    }
    CHECK(e == VFS_NO_SPACE && made >= 8, "disk full not reported as no space");
    unsigned partial = made - 1;
    name[2] = (char)('0' + partial / 100); name[3] = (char)('0' + partial / 10 % 10); name[4] = (char)('0' + partial % 10);
    CHECK(fs->unlink(name) == VFS_OK && clean(), "the partially written file is not removable, or the volume is inconsistent");
    for (unsigned i = 0; i < partial; ++i) {
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        const char *r = stream_verify(name, 64 * 1024, (uint8_t)i); CHECK(!r, r);
    }
    for (unsigned i = 0; i < 3; ++i) { name[2] = '0'; name[3] = '0'; name[4] = (char)('0' + i); CHECK(fs->unlink(name) == VFS_OK, "unlink failed"); }
    CHECK(stream_write("/reuse", 128 * 1024, 77, BLOCK, true) == VFS_OK, "freed space was not reusable");
    { const char *r = stream_verify("/reuse", 128 * 1024, 77); CHECK(!r, r); }
    CHECK(clean(), "volume not clean after reuse");
    return teardown(0);
}
static const char *f_metadata_exhaustion(void) {
    SETUP(4096, 512);                                          /* 64 inodes */
    struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info) && info.inode_count == 128, "unexpected inode count");
    char name[8] = "/a000"; unsigned made = 0; enum vfs_error e = VFS_OK;
    for (; made < 200 && e == VFS_OK; ++made) { name[2] = (char)('0' + made / 100); name[3] = (char)('0' + made / 10 % 10); name[4] = (char)('0' + made % 10); e = fs->create_file(name); }
    CHECK(e == VFS_NO_SPACE && made == 128, "inode exhaustion not reported at 127 files");
    CHECK(ufs2_fs_get_info(&info) && info.free_inodes == 0 && info.free_blocks > 0 && clean(), "metadata exhaustion left the volume inconsistent");
    CHECK(fs->unlink("/a000") == VFS_OK && fs->create_file("/again") == VFS_OK, "freed inode not reusable");
    CHECK(remount(true) && clean(), "volume at inode capacity does not remount");
    return teardown(0);
}
static const char *f_directory_growth(void) {
    SETUP(4096, 2048);
    CHECK(fs->create_directory("/many") == VFS_OK, "mkdir failed");
    char path[32] = "/many/e000";
    for (unsigned i = 0; i < 200; ++i) {
        path[7] = (char)('0' + i / 100); path[8] = (char)('0' + i / 10 % 10); path[9] = (char)('0' + i % 10);
        CHECK(fs->create_file(path) == VFS_OK, "create in a growing directory failed");
    }
    CHECK(count_entries("/many", 0) == 200 && fs->unlink("/many") == VFS_NOT_EMPTY, "growing directory wrong");
    for (unsigned i = 0; i < 200; i += 2) {
        path[7] = (char)('0' + i / 100); path[8] = (char)('0' + i / 10 % 10); path[9] = (char)('0' + i % 10);
        CHECK(fs->unlink(path) == VFS_OK, "unlink in a large directory failed");
    }
    CHECK(count_entries("/many", 0) == 100 && fs->create_file("/many/zzz") == VFS_OK, "freed directory slot not reused");
    CHECK(clean() && remount(true) && count_entries("/many", 0) == 101 && clean(), "large directory lost on remount");
    return teardown(0);
}
static const char *f_rename_busy(void) {
    SETUP(4096, 2048);
    CHECK(fs->create_directory("/a") == VFS_OK && fs->create_directory("/a/sub") == VFS_OK && fs->create_directory("/b") == VFS_OK, "setup failed");
    CHECK(stream_write("/a/sub/data", 6000, 2, 2000, true) == VFS_OK, "write failed");
    CHECK(fs->rename("/a/sub/data", "/a/sub/data2") == VFS_OK && fs->stat("/a/sub/data", &(struct vfs_info){0}) == VFS_NOT_FOUND, "same-directory rename failed");
    CHECK(fs->rename("/a/sub", "/b/moved") == VFS_OK, "cross-directory directory rename failed");
    { const char *r = stream_verify("/b/moved/data2", 6000, 2); CHECK(!r, r); }
    CHECK(clean(), "volume not clean after renames (parent links)");
    CHECK(fs->create_file("/b/exists") == VFS_OK && fs->rename("/b/moved", "/b/exists") == VFS_EXISTS, "rename onto an existing name accepted");
    CHECK(fs->rename("/b", "/b/moved/inside") == VFS_INVALID_PATH && fs->rename("/b", "/b") == VFS_INVALID_PATH && fs->rename("/", "/x") == VFS_INVALID_PATH, "invalid rename accepted");
    CHECK(fs->create_directory("/b/moved/deep") == VFS_OK && fs->rename("/b", "/b/moved/deep/b2") == VFS_INVALID_PATH, "directory moved below itself accepted");
    CHECK(fs->rename("/missing", "/y") == VFS_NOT_FOUND && fs->rename("/b/exists", "/nodir/z") == VFS_NOT_FOUND, "bad rename accepted");
    struct vfs_file f = {0};
    CHECK(fs->file_open("/b/moved/data2", VFS_OPEN_READ, &f) == VFS_OK, "open failed");
    CHECK(fs->unlink("/b/moved/data2") == VFS_BUSY && fs->rename("/b/moved/data2", "/b/q") == VFS_BUSY, "open file removed or renamed");
    CHECK(fs->file_close(&f) == VFS_OK && fs->unlink("/b/moved/data2") == VFS_OK, "file not removable after close");
    struct vfs_dir d = {0};
    CHECK(fs->dir_open("/b", &d) == VFS_OK && ufs2_fs_unmount() == VFS_BUSY && fs->dir_close(&d) == VFS_OK, "unmount with an open directory accepted");
    struct vfs_file many[17]; unsigned opened = 0;
    CHECK(fs->create_file("/h") == VFS_OK, "create failed");
    for (; opened < 17; ++opened) { many[opened] = (struct vfs_file){0}; if (fs->file_open("/h", VFS_OPEN_READ, &many[opened]) != VFS_OK) break; }
    CHECK(opened == 16, "handle limit is not 16");
    for (unsigned i = 0; i < opened; ++i) CHECK(fs->file_close(&many[i]) == VFS_OK, "close failed");
    CHECK(clean(), "volume not clean at the end");
    return teardown(0);
}
static const char *f_persistence(void) {
    SETUP(4096, 2048);
    CHECK(fs->create_directory("/docs") == VFS_OK && stream_write("/docs/a.txt", 10000, 1, 4096, true) == VFS_OK &&
          stream_write("/b.bin", 70000, 2, 4096, true) == VFS_OK && fs->create_file("/empty") == VFS_OK, "setup failed");
    CHECK(ufs2_fs_sync() == VFS_OK && ufs2_fs_unmount() == VFS_OK, "sync/unmount failed");
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK, "remount failed");
    struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info) && !info.unclean && info.orphans == 0, "clean volume reported unclean");
    { const char *r = stream_verify("/docs/a.txt", 10000, 1); CHECK(!r, r); }
    { const char *r = stream_verify("/b.bin", 70000, 2); CHECK(!r, r); }
    CHECK(stream_write("/c.bin", 5000, 8, 4096, true) == VFS_OK, "write failed");
    ufs2_fs_test_abandon();                                     /* a session that never synchronized or unmounted */
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && info.unclean && !info.repair_recommended, "unclean session not reported (or wrongly demands repair)");
    { const char *r = stream_verify("/c.bin", 5000, 8); CHECK(!r, r); }
    CHECK(clean() && ufs2_fs_sync() == VFS_OK && ufs2_fs_unmount() == VFS_OK && ufs2_fs_mount(dev, true, &report) == VFS_OK, "sync/remount failed");
    CHECK(ufs2_fs_get_info(&info) && !info.unclean, "sync did not mark the volume clean");
    for (unsigned i = 0; i < 20; ++i) CHECK(ufs2_fs_unmount() == VFS_OK && ufs2_fs_mount(dev, true, &report) == VFS_OK, "mount cycle failed");
    CHECK(stream_write("/after", 100, 1, 100, true) == VFS_OK, "write after sync failed (state not re-marked)");
    ufs2_fs_test_abandon();
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && info.unclean, "a change after a sync was not tracked as unclean");
    return teardown(0);
}
static const char *f_read_only(void) {
    SETUP(4096, 2048);
    CHECK(stream_write("/keep", 9000, 4, 4096, true) == VFS_OK && ufs2_fs_unmount() == VFS_OK, "setup failed");
    CHECK(ufs2_fs_mount(dev, false, &report) == VFS_OK, "read-only mount failed");
    struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info) && info.read_only, "read-only flag not reported");
    CHECK(fs->create_file("/x") == VFS_READ_ONLY && fs->create_directory("/y") == VFS_READ_ONLY && fs->unlink("/keep") == VFS_READ_ONLY && fs->rename("/keep", "/k2") == VFS_READ_ONLY, "modification on a read-only mount accepted");
    struct vfs_file f = {0};
    CHECK(fs->file_open("/keep", VFS_OPEN_WRITE, &f) == VFS_READ_ONLY, "write open on a read-only mount accepted");
    { const char *r = stream_verify("/keep", 9000, 4); CHECK(!r, r); }
    CHECK(ufs2_fs_sync() == VFS_OK && ufs2_fs_unmount() == VFS_OK, "read-only unmount failed");
    /* A device without flush mounts read-only with the reason; a read-only device likewise. */
    dev->flags &= ~BLOCK_FLAG_FLUSH;
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && info.read_only && info.ro_reason, "device without flush mounted read-write");
    CHECK(fs->create_file("/x") == VFS_READ_ONLY && ufs2_fs_unmount() == VFS_OK, "write accepted without flush support");
    dev->flags |= BLOCK_FLAG_FLUSH | BLOCK_FLAG_READ_ONLY;
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && info.read_only && fs->create_file("/x") == VFS_READ_ONLY, "read-only device not honoured");
    CHECK(ufs2_fs_unmount() == VFS_OK, "unmount failed");
    dev->flags &= ~BLOCK_FLAG_READ_ONLY;
    return teardown(0);
}
static const char *f_block_sizes(void) {
    static const uint32_t sizes[] = {512, 1024, 2048, 4096};
    for (unsigned i = 0; i < 4; ++i) {
        const char *s = setup(sizes[i], (4ULL << 20) / sizes[i], false);
        if (s) return teardown(s);
        enum vfs_error e = fs->create_directory("/d");
        if (e == VFS_OK) e = stream_write("/d/f", 20000, (uint8_t)i, 3000, true);
        const char *r = 0;
        if (e != VFS_OK) r = "write failed";
        else if (ufs2_fs_sync() != VFS_OK || !remount(true)) r = "remount failed";
        else r = stream_verify("/d/f", 20000, (uint8_t)i);
        if (!r && !clean()) r = "volume not clean";
        if (r) return teardown(r);
        r = teardown(0);
        if (r) return r;
    }
    return 0;
}
static const char *f_generations(void) {
    SETUP(4096, 512);                                          /* 128 inodes: easy to exhaust and recycle */
    refresh_geometry();
    uint32_t per = BLOCK / dev->block_size;
    char name[8] = "/g000"; unsigned made = 0; enum vfs_error e = VFS_OK;
    for (; made < 200 && e == VFS_OK; ++made) { name[2] = (char)('0' + made / 100); name[3] = (char)('0' + made / 10 % 10); name[4] = (char)('0' + made % 10); e = fs->create_file(name); }
    CHECK(e == VFS_NO_SPACE && made == 128, "inode table not exhausted as expected");
    struct ufs2_inode in;
    (void)block_read(dev, (uint64_t)G.inode_start * per, per, raw, BLOCK);
    CHECK(ufs2_inode_decode(UUID, 2, raw + UFS2_INODE_SIZE, &in) == VFS_OK && in.type == UFS2_FILE && in.generation == 1, "first allocation must have generation 1");
    CHECK(fs->unlink("/g000") == VFS_OK, "unlink failed");
    (void)block_read(dev, (uint64_t)G.inode_start * per, per, raw, BLOCK);
    CHECK(ufs2_inode_decode(UUID, 2, raw + UFS2_INODE_SIZE, &in) == VFS_OK && in.type == UFS2_FREE && in.generation == 1 && !ufs2_slot_pristine(raw + UFS2_INODE_SIZE),
          "a freed inode must keep its generation with a valid checksum");
    CHECK(fs->create_file("/recycled") == VFS_OK, "create failed");
    (void)block_read(dev, (uint64_t)G.inode_start * per, per, raw, BLOCK);
    CHECK(ufs2_inode_decode(UUID, 2, raw + UFS2_INODE_SIZE, &in) == VFS_OK && in.type == UFS2_FILE && in.generation == 2, "reallocation must increment the generation");
    CHECK(clean(), "volume not clean after recycling an inode");
    return teardown(0);
}

/* ---- damaged volumes ---- */
static uint32_t T_per(void) { return BLOCK / dev->block_size; }
static void rd(uint32_t b) { (void)block_read(dev, (uint64_t)b * T_per(), T_per(), raw, BLOCK); }
static void wr(uint32_t b) { (void)block_write(dev, (uint64_t)b * T_per(), T_per(), raw, BLOCK); }
static uint32_t inode_block(uint32_t ino) { return G.inode_start + (ino - 1) / UFS2_INODES_PER_BLOCK; }
static uint8_t *inode_raw(uint32_t ino) { return raw + ((ino - 1) % UFS2_INODES_PER_BLOCK) * UFS2_INODE_SIZE; }
static struct ufs2_inode get_inode(uint32_t ino) { struct ufs2_inode in; rd(inode_block(ino)); (void)ufs2_inode_decode(UUID, ino, inode_raw(ino), &in); return in; }
static void put_inode(uint32_t ino, const struct ufs2_inode *in) { rd(inode_block(ino)); ufs2_inode_encode(UUID, ino, in, inode_raw(ino)); wr(inode_block(ino)); }
static uint32_t root_block(void) { return get_inode(1).slot[0].physical; }
static void dir_edit(uint32_t block, uint32_t owner, uint32_t gen, uint32_t logical, void (*edit)(uint8_t *), bool reseal) {
    rd(block); edit(raw);
    if (reseal) ufs2_dirblock_seal(UUID, block, raw, owner, gen, logical);
    wr(block);
}
static void bitmap_edit(uint32_t k, void (*edit)(uint8_t *), bool reseal) {
    rd(G.bitmap_start + k); edit(raw);
    if (reseal) ufs2_bitmap_seal(UUID, G.bitmap_start + k, raw);
    wr(G.bitmap_start + k);
}

enum damage { C_INODE_CRC, C_EXTENT_RANGE, C_OVERLAP, C_ZERO_LENGTH, C_LOGICAL_GAP, C_SIZE, C_BLOCK_COUNT, C_DANGLING, C_ENTRY_GEN, C_ENTRY_TYPE,
              C_DUP_REF, C_PARENT, C_DIR_FOOTER, C_DIR_CRC, C_BAD_NAME, C_ROOT_GONE, C_LEAF, C_RESERVED, C_DUP_NAME, C_CYCLE, C_COUNT };
static void e_entry0_ino(uint8_t *p) { p[0] = 100; p[1] = 0; p[2] = 0; p[3] = 0; }
static void e_entry0_gen(uint8_t *p) { p[4] += 1; }
static void e_entry0_type(uint8_t *p) { p[8] = p[8] == 1 ? 2 : 1; }
static void e_entry_dup(uint8_t *p) { memcpy(p + 64, p, 8); p[64 + 8] = p[8]; }          /* second entry names the first entry's inode */
static void e_footer(uint8_t *p) { p[4036] ^= 1; }
static void e_crc(uint8_t *p) { p[4092] ^= 1; }
static void e_name_slash(uint8_t *p) { p[12] = '/'; }
static void e_dup_name(uint8_t *p) { memcpy(p + 64 + 12, p + 12, 52); p[64 + 9] = p[9]; }  /* two entries with one name */
static void e_leak(uint8_t *p) { p[200] |= 0x01; }             /* block 1600: marked used, owned by nobody */
static void e_crc_bitmap(uint8_t *p) { p[4092] ^= 1; }
static void e_padding(uint8_t *p) { p[4087] &= 0x7f; }

static const char *apply_damage(enum damage kind) {
    /* Fixture inodes: 1 root, 2 /dir, 3 /f1 (3 blocks), 4 /f2 (2 blocks), 5 /dir/inner. */
    struct ufs2_inode in;
    uint32_t rb = root_block();
    switch (kind) {
        case C_INODE_CRC: rd(inode_block(3)); inode_raw(3)[10] ^= 1; wr(inode_block(3)); break;
        case C_EXTENT_RANGE: in = get_inode(3); in.slot[0].physical = G.total; put_inode(3, &in); break;
        case C_OVERLAP: { struct ufs2_inode a = get_inode(3); in = get_inode(4); in.slot[0].physical = a.slot[0].physical; put_inode(4, &in); break; }
        case C_ZERO_LENGTH: in = get_inode(3); in.slot[0].length = 0; put_inode(3, &in); break;
        case C_LOGICAL_GAP: in = get_inode(3); in.slot[0].logical = 1; put_inode(3, &in); break;
        case C_SIZE: in = get_inode(3); in.size = 100000; put_inode(3, &in); break;
        case C_BLOCK_COUNT: in = get_inode(3); in.block_count = 99; put_inode(3, &in); break;
        case C_DANGLING: dir_edit(rb, 1, 1, 0, e_entry0_ino, true); break;
        case C_ENTRY_GEN: dir_edit(rb, 1, 1, 0, e_entry0_gen, true); break;
        case C_ENTRY_TYPE: dir_edit(rb, 1, 1, 0, e_entry0_type, true); break;
        case C_DUP_REF: dir_edit(rb, 1, 1, 0, e_entry_dup, true); break;
        case C_PARENT: in = get_inode(3); in.parent = 2; put_inode(3, &in); break;
        case C_DIR_FOOTER: dir_edit(rb, 1, 1, 0, e_footer, false); break;
        case C_DIR_CRC: dir_edit(rb, 1, 1, 0, e_crc, false); break;
        case C_BAD_NAME: dir_edit(rb, 1, 1, 0, e_name_slash, true); break;
        case C_ROOT_GONE: rd(inode_block(1)); memset(inode_raw(1), 0, UFS2_INODE_SIZE); wr(inode_block(1)); break;
        case C_LEAF: {   /* a depth-1 inode whose leaf block belongs to another inode */
            in = get_inode(4); uint32_t leaf = G.data_start + 900;
            struct ufs2_extent x = in.slot[0];
            rd(leaf); ufs2_leaf_encode(UUID, leaf, raw, 3 /* wrong owner */, in.generation, 0, 1, &x); wr(leaf);
            in.depth = 1; in.slot[0] = (struct ufs2_extent){0, leaf, 1, 0}; in.block_count += 1; put_inode(4, &in);
            break;
        }
        case C_RESERVED: rd(inode_block(3)); inode_raw(3)[1] = 1; { uint32_t c = ufs2_crc32(UUID, 3, inode_raw(3), 252); inode_raw(3)[252] = (uint8_t)c; inode_raw(3)[253] = (uint8_t)(c >> 8); inode_raw(3)[254] = (uint8_t)(c >> 16); inode_raw(3)[255] = (uint8_t)(c >> 24); } wr(inode_block(3)); break;
        case C_DUP_NAME: dir_edit(rb, 1, 1, 0, e_dup_name, true); break;
        case C_CYCLE: {   /* two directories that contain each other and are unreachable from the root */
            struct ufs2_inode d2 = get_inode(2), d5 = get_inode(5);
            rd(rb); ufs2_dirent_clear(raw, 0); ufs2_dirent_clear(raw, 1); ufs2_dirent_clear(raw, 2); ufs2_dirent_clear(raw, 3);
            ufs2_dirblock_seal(UUID, rb, raw, 1, 1, 0); wr(rb);
            rd(d2.slot[0].physical); ufs2_dirent_clear(raw, 0); ufs2_dirent_set(raw, 0, 5, d5.generation, UFS2_DIR, "inner", 5);
            ufs2_dirblock_seal(UUID, d2.slot[0].physical, raw, 2, d2.generation, 0); wr(d2.slot[0].physical);
            rd(d5.slot[0].physical); ufs2_dirent_set(raw, 0, 2, d2.generation, UFS2_DIR, "outer", 5);
            ufs2_dirblock_seal(UUID, d5.slot[0].physical, raw, 5, d5.generation, 0); wr(d5.slot[0].physical);
            d2.parent = 5; put_inode(2, &d2);
            break;
        }
        default: return "unknown damage";
    }
    return 0;
}
static const uint8_t expected_code[C_COUNT] = {UF_INODE_CRC, UF_EXTENT_RANGE, UF_EXTENT_OVERLAP, UF_EXTENT_RANGE, UF_EXTENT_ORDER, UF_EXTENT_COUNT, UF_EXTENT_COUNT,
    UF_DIR_ENTRY, UF_DIR_ENTRY, UF_DIR_ENTRY, UF_DIR_DUPLICATE_REF, UF_DIR_PARENT, UF_DIR_BLOCK, UF_DIR_BLOCK, UF_DIR_ENTRY, UF_ROOT_INVALID, UF_LEAF_INVALID,
    UF_INODE_CRC, UF_DIR_DUPLICATE_NAME, UF_DIR_CYCLE};
static const char *build_fixture(void) {
    const char *s = setup(4096, 2048, false);
    if (s) return s;
    if (fs->create_directory("/dir") != VFS_OK || stream_write("/f1", 9000, 1, BLOCK, true) != VFS_OK ||
        stream_write("/f2", 5000, 2, BLOCK, true) != VFS_OK || fs->create_directory("/dir/inner") != VFS_OK) return "fixture failed";
    if (ufs2_fs_sync() != VFS_OK || ufs2_fs_unmount() != VFS_OK) return "fixture unmount failed";
    refresh_geometry();
    return 0;
}
static bool has_code(uint8_t code) { for (uint32_t i = 0; i < report.stored; ++i) if (report.item[i].code == code) return true; return false; }

static const char *f_structural_damage(void) {
    for (unsigned k = 0; k < C_COUNT; ++k) {
        const char *s = build_fixture();
        if (s) return teardown(s);
        s = apply_damage((enum damage)k);
        if (s) return teardown(s);
        uint64_t frames = pmm_free_pages();
        enum vfs_error e = ufs2_fs_mount(dev, true, &report);
        if (e != VFS_CORRUPT) FAIL("a structurally damaged volume mounted or returned the wrong error");
        if (!report.count[2] || !has_code(expected_code[k])) FAIL("the damage was refused for the wrong reason");
        CHECK(!ufs2_fs_mounted() && dev->holders == 0 && pmm_free_pages() == frames, "a refused mount left resources claimed or leaked memory");
        CHECK(ufs2_fs_mount(dev, false, &report) == VFS_CORRUPT, "damaged volume mounted read-only");
        const char *t = teardown(0);
        if (t) return t;
    }
    return 0;
}
static const char *f_repairable_damage(void) {
    struct { void (*edit)(uint8_t *); const char *name; uint8_t code; } cases[] = {
        {e_leak, "leaked block", UF_BITMAP_LEAK}, {e_crc_bitmap, "bitmap checksum", UF_BITMAP_CRC}, {e_padding, "padding bits", UF_BITMAP_PADDING}};
    for (unsigned k = 0; k < 3; ++k) {
        const char *s = build_fixture();
        if (s) return teardown(s);
        bitmap_edit(0, cases[k].edit, k == 0 || k == 2);
        enum vfs_error e = ufs2_fs_mount(dev, true, &report);
        if (e != VFS_OK) FAIL(cases[k].name);
        struct ufs2_fs_info info;
        CHECK(ufs2_fs_get_info(&info) && info.read_only && info.repair_recommended && report.count[0] && !report.count[2] && has_code(cases[k].code), "repairable damage not reported");
        CHECK(fs->create_file("/new") == VFS_READ_ONLY, "a volume needing repair accepted a write");
        { const char *r = stream_verify("/f1", 9000, 1); CHECK(!r, r); }   /* read-only inspection works */
        const char *t = teardown(0);
        if (t) return t;
    }
    /* An owned block marked free (dangerous: it could be handed out twice). */
    const char *s = build_fixture();
    if (s) return teardown(s);
    struct ufs2_inode f1 = get_inode(3);
    uint32_t k = f1.slot[0].physical / UFS2_BITMAP_BITS, off = f1.slot[0].physical % UFS2_BITMAP_BITS;
    rd(G.bitmap_start + k); raw[off >> 3] &= (uint8_t)~(1U << (off & 7)); ufs2_bitmap_seal(UUID, G.bitmap_start + k, raw); wr(G.bitmap_start + k);
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && report.bitmap_unmarked >= 1, "an owned block marked free was not detected");
    struct ufs2_fs_info info;
    CHECK(ufs2_fs_get_info(&info) && info.read_only && fs->create_file("/n") == VFS_READ_ONLY, "volume with an unmarked block accepted writes");
    return teardown(0);
}
static const char *f_orphans(void) {
    const char *s = build_fixture();
    if (s) return teardown(s);
    uint32_t rb = root_block();
    rd(rb); ufs2_dirent_clear(raw, 1); ufs2_dirblock_seal(UUID, rb, raw, 1, 1, 0); wr(rb);   /* /f1 loses its name */
    enum vfs_error e = ufs2_fs_mount(dev, true, &report);
    CHECK(e == VFS_OK, "an unreferenced inode must not block mounting");
    struct ufs2_fs_info info;
    CHECK(ufs2_fs_get_info(&info) && info.orphans == 1 && !info.read_only && report.count[1] == 1, "orphan not counted (or the volume went read-only)");
    CHECK(fs->stat("/f1", &(struct vfs_info){0}) == VFS_NOT_FOUND, "orphan still visible");
    { const char *r = stream_verify("/f2", 5000, 2); CHECK(!r, r); }
    CHECK(fs->create_file("/new") == VFS_OK, "orphans prevented writing");
    return teardown(0);
}
static const char *f_stale_generation(void) {
    /* /f1 is deleted and its inode number recycled for another file, but a stale directory
       entry that still carries the old generation remains: it must be detected as dangling. */
    const char *s = build_fixture();
    if (s) return teardown(s);
    struct ufs2_inode in = get_inode(3);
    in.generation += 1; put_inode(3, &in);                      /* the inode was recycled: the entry's generation is now stale */
    enum vfs_error e = ufs2_fs_mount(dev, true, &report);
    CHECK(e == VFS_CORRUPT && has_code(UF_DIR_ENTRY), "a stale-generation directory entry was not detected");
    return teardown(0);
}
static const char *f_stale_state(void) {
    const char *s = setup(4096, 2048, false);
    if (s) return teardown(s);
    CHECK(stream_write("/a", 100, 1, 100, true) == VFS_OK, "write failed");
    ufs2_fs_test_abandon();
    CHECK(ufs2_fs_mount(dev, false, &report) == VFS_OK && has_code(UF_STALE_STATE) && report.count[0] == 1, "stale mounted state not reported on a read-only open");
    return teardown(0);
}


/* A crafted file at the extent limit: 11 full leaf blocks (2783 one-block extents). */
static const char *f_extent_limit(void) {
    SETUP(4096, 3072);
    refresh_geometry();
    CHECK(ufs2_fs_unmount() == VFS_OK, "unmount failed");
    const uint32_t extents = UFS2_MAX_EXTENTS, base = G.data_start + 1 + 11;   /* after the root block and the 11 leaves */
    CHECK(base + extents < G.total - 1, "volume too small for the fixture");
    uint32_t root_blk = G.data_start;
    /* Data: logical k lives at physical base + (extents-1-k), so no two extents are adjacent in order. */
    for (uint32_t k = 0; k < extents; ++k) { pat(chunk, (uint64_t)k * BLOCK, BLOCK, 11); memcpy(raw, chunk, BLOCK); wr(base + (extents - 1 - k)); }
    struct ufs2_inode file = {0};
    file.type = UFS2_FILE; file.generation = 1; file.parent = 1; file.size = (uint64_t)extents * BLOCK; file.depth = 1;
    file.root_entries = UFS2_ROOT_SLOTS; file.extent_count = extents; file.block_count = extents + UFS2_ROOT_SLOTS;
    for (uint32_t i = 0; i < UFS2_ROOT_SLOTS; ++i) {
        uint32_t leaf = G.data_start + 1 + i;
        static struct ufs2_extent list[UFS2_LEAF_EXTENTS];
        for (uint32_t j = 0; j < UFS2_LEAF_EXTENTS; ++j) { uint32_t k = i * UFS2_LEAF_EXTENTS + j; list[j] = (struct ufs2_extent){k, base + (extents - 1 - k), 1, 0}; }
        ufs2_leaf_encode(UUID, leaf, raw, 2, 1, i * UFS2_LEAF_EXTENTS, UFS2_LEAF_EXTENTS, list); wr(leaf);
        file.slot[i] = (struct ufs2_extent){i * UFS2_LEAF_EXTENTS, leaf, UFS2_LEAF_EXTENTS, 0};
    }
    put_inode(2, &file);
    rd(root_blk); ufs2_dirent_set(raw, 0, 2, 1, UFS2_FILE, "limit", 5); ufs2_dirblock_seal(UUID, root_blk, raw, 1, 1, 0); wr(root_blk);
    /* Bitmap: mark the leaves and the data blocks. */
    for (uint32_t b = G.data_start + 1; b < base + extents; ++b) {
        uint32_t k = b / UFS2_BITMAP_BITS, off = b % UFS2_BITMAP_BITS;
        rd(G.bitmap_start + k); raw[off >> 3] |= (uint8_t)(1U << (off & 7)); ufs2_bitmap_seal(UUID, G.bitmap_start + k, raw); wr(G.bitmap_start + k);
    }
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK, "a maximal valid extent map was refused");
    CHECK(clean() && report.extents >= extents, "maximal extent map does not verify cleanly");
    /* Read it back through the 11 leaf blocks. */
    { const char *r = stream_verify("/limit", (uint64_t)extents * BLOCK, 11); CHECK(!r, r); }
    /* One more block would need a 2784th extent: refused cleanly, nothing changes. */
    struct ufs2_fs_info before; CHECK(ufs2_fs_get_info(&before), "info failed");
    struct vfs_file f = {0}; size_t n = 99;
    CHECK(fs->file_open("/limit", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
    for (uint32_t k = 0; k < extents; ++k) { pat(chunk, (uint64_t)k * BLOCK, BLOCK, 11); CHECK(fs->file_write(&f, chunk, BLOCK, &n) == VFS_OK && n == BLOCK, "positioning write failed"); }
    CHECK(fs->file_write(&f, "x", 1, &n) == VFS_NO_SPACE && n == 0, "an append beyond the extent limit was not refused");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    struct ufs2_fs_info after;
    CHECK(ufs2_fs_get_info(&after) && after.free_blocks == before.free_blocks, "the refused append leaked blocks");
    CHECK(clean(), "the refused append left the volume inconsistent");
    CHECK(remount(true) && clean(), "remount failed");
    CHECK(fs->unlink("/limit") == VFS_OK && clean(), "removal of a maximal file failed");
    CHECK(ufs2_fs_get_info(&after) && after.free_blocks == after.total_blocks - 1, "removing a maximal file did not reclaim every block");
    return teardown(0);
}


/* ---- checker and repair ---- */
static struct ufs2_check_result cr;
static uint32_t device_digest(void) {                 /* CRC-32C over the whole device image */
    uint32_t c = crc32c_begin();
    static uint8_t blk[4096];
    uint32_t per = BLOCK / dev->block_size;
    for (uint64_t b = 0; b + per <= dev->block_count; b += per) { (void)block_read(dev, b, per, blk, BLOCK); c = crc32c_update(c, blk, BLOCK); }
    return crc32c_end(c);
}
static bool cr_has(const struct ufs2_report *r, uint8_t code) { for (uint32_t i = 0; i < r->stored; ++i) if (r->item[i].code == code) return true; return false; }
static bool mounts_rw(void) {
    struct ufs2_fs_info info;
    bool ok = ufs2_fs_mount(dev, true, &report) == VFS_OK && ufs2_fs_get_info(&info) && !info.read_only && !info.repair_recommended;
    return ok;
}

static const char *k_clean_check(void) {
    const char *s = build_fixture();
    if (s) return teardown(s);
    uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.status == VFS_OK, "check failed to run");
    CHECK(!cr.before.count[0] && !cr.before.count[1] && !cr.before.count[2] && !cr.journal_pending && cr.safe_to_mount_writable, "a consistent volume was reported as damaged");
    CHECK(cr.before.inodes_used == 5 && cr.before.directories == 3 && cr.before.files == 2, "the check miscounted the fixture");
    CHECK(block_memory_events(dev) == events && device_digest() == before, "a read-only check wrote to the device");
    CHECK(mounts_rw(), "the checked volume does not mount");
    CHECK(ufs2_check(dev, false, &cr) == VFS_BUSY, "a mounted volume was checked");
    ufs2_fs_test_abandon();
    return teardown(0);
}
static const char *k_check_damage_never_writes(void) {
    static const unsigned kinds[] = {C_OVERLAP, C_DANGLING, C_DUP_REF, C_PARENT, C_CYCLE, C_DIR_FOOTER, C_INODE_CRC};
    for (unsigned i = 0; i < 7; ++i) {
        const char *s = build_fixture();
        if (s) return teardown(s);
        s = apply_damage((enum damage)kinds[i]);
        if (s) return teardown(s);
        uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
        CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.status == VFS_OK && cr.before.count[2] && !cr.safe_to_mount_writable, "damage not reported as structural");
        CHECK(cr_has(&cr.before, expected_code[kinds[i]]), "the check named the wrong problem");
        CHECK(block_memory_events(dev) == events && device_digest() == before, "the check modified a damaged volume");
        /* --repair must refuse structural damage and leave the image byte-identical. */
        CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_refused && !cr.repair_ran && cr.refusal, "repair of structural damage was not refused");
        CHECK(block_memory_events(dev) == events && device_digest() == before, "a refused repair modified the device");
        const char *t = teardown(0);
        if (t) return t;
    }
    return 0;
}
static const char *k_repair_bitmap(void) {
    struct { void (*edit)(uint8_t *); bool reseal; const char *name; } cases[] = {
        {e_leak, true, "leaked block"}, {e_crc_bitmap, false, "bitmap checksum"}, {e_padding, true, "padding bits"}};
    for (unsigned k = 0; k < 4; ++k) {
        const char *s = build_fixture();
        if (s) return teardown(s);
        if (k < 3) bitmap_edit(0, cases[k].edit, cases[k].reseal);
        else {   /* an owned block marked free */
            struct ufs2_inode f1 = get_inode(3);
            uint32_t bk = f1.slot[0].physical / UFS2_BITMAP_BITS, off = f1.slot[0].physical % UFS2_BITMAP_BITS;
            rd(G.bitmap_start + bk); raw[off >> 3] &= (uint8_t)~(1U << (off & 7)); ufs2_bitmap_seal(UUID, G.bitmap_start + bk, raw); wr(G.bitmap_start + bk);
        }
        uint32_t before = device_digest();
        CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.before.count[0] && !cr.before.count[2] && !cr.safe_to_mount_writable, "repairable damage not reported");
        CHECK(device_digest() == before, "the check wrote to the device");
        CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_ran && !cr.repair_refused && cr.fully_repaired && cr.action_count >= 1, "repair did not complete");
        CHECK(!cr.after.count[0] && !cr.after.count[2], "problems remain after a reported full repair");
        CHECK(ufs2_check(dev, false, &cr) == VFS_OK && !cr.before.count[0] && !cr.before.count[2] && cr.safe_to_mount_writable, "the repaired volume still checks as damaged");
        CHECK(mounts_rw(), "the repaired volume does not mount read-write");
        { const char *r = stream_verify("/f1", 9000, 1); CHECK(!r, r); }
        { const char *r = stream_verify("/f2", 5000, 2); CHECK(!r, r); }
        CHECK(fs->create_file("/after-repair") == VFS_OK && clean(), "the repaired volume is not writable or not clean");
        ufs2_fs_test_abandon();
        /* Repair is idempotent. */
        CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_ran, "second repair failed");
        const char *t = teardown(0);
        if (t) return t;
    }
    return 0;
}
static const char *k_repair_superblock_and_state(void) {
    const char *s = build_fixture();
    if (s) return teardown(s);
    /* Damaged superblock copy B plus a stale mounted state. */
    CHECK(ufs2_fs_mount(dev, true, &report) == VFS_OK && fs->create_file("/touch") == VFS_OK, "setup failed");
    ufs2_fs_test_abandon();                                      /* leaves state = mounted */
    uint32_t last = G.total - 1;
    rd(last); raw[100] ^= 1; wr(last);
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr_has(&cr.before, UF_SB_COPY) && cr_has(&cr.before, UF_STALE_STATE) && !cr.safe_to_mount_writable, "damaged copy / stale state not reported");
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_ran && cr.fully_repaired, "repair did not complete");
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && !cr.before.count[0] && !cr.before.count[2], "superblock or state problem remains after repair");
    struct ufs2_core probe;
    CHECK(ufs2_core_inspect(&probe, dev) == VFS_OK && !probe.sb_was_degraded && probe.sb.state == UFS2_STATE_CLEAN, "superblock copies or state not repaired");
    ufs2_test_abandon(&probe);
    CHECK(mounts_rw(), "volume does not mount after repair");
    { struct ufs2_fs_info info; CHECK(ufs2_fs_get_info(&info) && !info.unclean, "stale state not cleared"); }
    ufs2_fs_test_abandon();
    /* Both copies damaged: reported, nothing written, refused. */
    rd(0); raw[100] ^= 1; wr(0); rd(last); raw[100] ^= 1; wr(last);
    uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.superblock_unreadable && !cr.safe_to_mount_writable, "both superblock copies damaged but reported fine");
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.superblock_unreadable && cr.repair_refused, "repair attempted without a valid superblock");
    CHECK(block_memory_events(dev) == events && device_digest() == before, "a check of an unrecoverable volume wrote to it");
    return teardown(0);
}
static const char *k_repair_journal(void) {
    const char *s = setup(4096, 2048, true);
    if (s) return teardown(s);
    refresh_geometry();
    /* Crash a create exactly after its commit is durable but before its home writes: the journal holds one transaction.
       Every iteration starts from a freshly formatted volume so earlier crashes cannot leave state behind. */
    int64_t window = -1;
    for (int64_t point = 0; point < 60 && window < 0; ++point) {
        if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
        block_memory_power_cycle(dev, POWER_KEEP_ALL, 0);
        CHECK(ufs2_fs_format(dev, "FSTEST", UUID) == VFS_OK && ufs2_fs_mount(dev, true, &report) == VFS_OK, "format/mount failed");
        CHECK(fs->create_directory("/dir") == VFS_OK && ufs2_fs_sync() == VFS_OK, "setup failed");
        block_memory_crash_after(dev, point);
        (void)fs->create_file("/dir/pending");
        bool crashed = block_memory_crashed(dev);
        const char *phase = block_memory_crash_phase(dev);
        ufs2_fs_test_abandon();
        if (crashed && phase[0] == 'h' && phase[1] == 'o' && phase[2] == 'm' && phase[3] == 'e' && phase[4] == '-' && phase[5] == 'w') window = point;
        else if (!crashed) break;
    }
    CHECK(window >= 0, "home-write window not found");
    /* Re-run the winning iteration so the device holds exactly that state. */
    block_memory_power_cycle(dev, POWER_KEEP_ALL, 0);
    CHECK(ufs2_fs_format(dev, "FSTEST", UUID) == VFS_OK && ufs2_fs_mount(dev, true, &report) == VFS_OK, "format/mount failed");
    CHECK(fs->create_directory("/dir") == VFS_OK && ufs2_fs_sync() == VFS_OK, "setup failed");
    block_memory_crash_after(dev, window);
    (void)fs->create_file("/dir/pending");
    CHECK(block_memory_crashed(dev), "the crash point did not fire");
    ufs2_fs_test_abandon();
    block_memory_power_cycle(dev, POWER_LOSE_ALL, 0);               /* only the flushed (committed) state survives */
    uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.journal_pending && cr.pending_transactions == 1 && cr_has(&cr.before, UF_JOURNAL_PENDING), "a committed, unapplied transaction was not reported");
    CHECK(block_memory_events(dev) == events && device_digest() == before, "the check replayed or wrote");
    CHECK(ufs2_fs_mount(dev, false, &report) == VFS_RECOVERY && dev->holders == 0, "a read-only mount of a volume needing replay must say recovery is required");
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_ran && cr.fully_repaired && cr.action_count >= 1, "journal repair did not complete");
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && !cr.journal_pending && !cr.before.count[0] && !cr.before.count[2], "journal still pending after repair");
    CHECK(mounts_rw() && fs->stat("/dir/pending", &(struct vfs_info){0}) == VFS_OK && clean(), "the committed create was not recovered");
    ufs2_fs_test_abandon();
    return teardown(0);
}
static const char *k_repair_policy(void) {
    /* Orphans are reported but never deleted; a read-only device and a device without flush refuse repairs. */
    const char *s = build_fixture();
    if (s) return teardown(s);
    uint32_t rb = root_block();
    rd(rb); ufs2_dirent_clear(raw, 1); ufs2_dirblock_seal(UUID, rb, raw, 1, 1, 0); wr(rb);
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.before.count[1] == 1 && cr.safe_to_mount_writable, "orphan not reported (or made the volume look unsafe)");
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.after.count[1] == 1, "repair must not delete an orphan");
    CHECK(ufs2_check(dev, false, &cr) == VFS_OK && cr.before.count[1] == 1, "the orphan disappeared");
    bitmap_edit(0, e_leak, true);
    uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
    dev->flags |= BLOCK_FLAG_READ_ONLY;
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_refused && !cr.repair_ran, "repair on a read-only device not refused");
    dev->flags &= ~BLOCK_FLAG_READ_ONLY; dev->flags &= ~BLOCK_FLAG_FLUSH;
    CHECK(ufs2_check(dev, true, &cr) == VFS_OK && cr.repair_refused && !cr.repair_ran, "repair on a device without flush not refused");
    dev->flags |= BLOCK_FLAG_FLUSH;
    CHECK(block_memory_events(dev) == events && device_digest() == before, "refused repairs modified the device");
    return teardown(0);
}

/* ---- shell integration and the v1 compatibility policy ---- */
static const char *k_shell_commands(void) {
    (void)ush_execute("go /");
    uint64_t frames = pmm_free_pages();
    CHECK(vfs_create_directory("/volumes") == VFS_OK && vfs_create_directory("/volumes/disk") == VFS_OK, "mount point setup failed");
    dev = block_memory_create("ufst", 512, 16384, false);
    CHECK(dev, "device creation failed");
    uint32_t blank = device_digest();
    CHECK(ush_execute("ultrium storage mount ufst /volumes/disk") == USH_FS_ERROR && device_digest() == blank && dev->holders == 0, "a blank disk was mounted or modified");
    CHECK(ush_execute("ultrium storage check ufst") == USH_FS_ERROR && device_digest() == blank, "checking a blank disk succeeded or wrote");
    CHECK(ush_execute("ultrium storage format ufst -y") == USH_OK, "format failed");
    CHECK(storage_probe(dev) == STORAGE_V2, "formatted disk not recognised as UltriumFS 2");
    CHECK(ush_execute("ultrium storage check ufst") == USH_OK, "a fresh volume failed its check");
    CHECK(ush_execute("ultrium storage mount ufst /volumes/disk") == USH_OK, "mount failed");
    CHECK(ush_execute("write /volumes/disk/note \"hello\"") == USH_OK && ush_execute("ultrium storage info /volumes/disk") == USH_OK, "write/info failed");
    CHECK(ush_execute("ultrium storage check ufst") == USH_FS_ERROR, "a mounted volume was checked");
    CHECK(ush_execute("ultrium storage format ufst -y") == USH_FS_ERROR, "a mounted volume was formatted");
    CHECK(ush_execute("ultrium storage unmount /volumes/disk") == USH_OK && ush_execute("ultrium storage check ufst --repair") == USH_OK, "unmount or repair of a consistent volume failed");
    CHECK(ush_execute("ultrium storage check ufst --bogus") == USH_USAGE && ush_execute("ultrium storage check") == USH_USAGE, "check argument validation missing");
    struct vfs_remove_report rr;
    (void)vfs_remove_tree("/volumes", &rr);
    CHECK(block_memory_destroy(dev), "device destroy failed");
    dev = 0;
    return pmm_free_pages() == frames ? 0 : "frame accounting changed";
}
static const char *k_v1_read_only_policy(void) {
    (void)ush_execute("go /");
    uint64_t frames = pmm_free_pages();
    if (vfs_create_directory("/volumes") != VFS_OK || vfs_create_directory("/volumes/disk") != VFS_OK) return "mount point setup failed";
    dev = block_memory_create("ufst", 512, 16384, false);
    if (!dev) return "device creation failed";
    const char *r = 0;
    /* Build a v1 volume with the v1 driver directly: shell formatting creates v2 only. */
    const struct vfs_backend_ops *v1 = ufs_backend();
    if (ufs_format(dev, "V1") != VFS_OK || ufs_mount(dev, false) != VFS_OK) r = "v1 setup failed";
    if (!r) {
        struct vfs_file f = {0}; size_t n;
        if (v1->create_directory("/old") != VFS_OK || v1->create_file("/old/data") != VFS_OK || v1->file_open("/old/data", VFS_OPEN_WRITE, &f) != VFS_OK ||
            v1->file_write(&f, "legacy data", 11, &n) != VFS_OK || v1->file_close(&f) != VFS_OK) r = "v1 fixture failed";
        if (ufs_sync() != VFS_OK || ufs_unmount() != VFS_OK) r = "v1 unmount failed";
    }
    uint32_t before = device_digest(); uint64_t events = block_memory_events(dev);
    if (!r && storage_probe(dev) != STORAGE_V1) r = "a v1 volume was not recognised";
    if (!r && ush_execute("ultrium storage check ufst") != USH_FS_ERROR) r = "the v2 checker accepted a v1 volume";
    if (!r && ush_execute("ultrium storage mount ufst /volumes/disk") != USH_OK) r = "a v1 volume could not be mounted read-only";
    if (!r) {
        struct storage_volume vol;
        struct vfs_info st; struct vfs_file f = {0}; size_t n;
        if (!storage_volume_status(&vol) || vol.version != 1 || !vol.read_only) r = "v1 volume not read-only";
        else if (vfs_stat("/volumes/disk/old/data", &st) != VFS_OK || st.size != 11) r = "v1 data unreadable";
        else if (vfs_file_open("/volumes/disk/old/data", VFS_OPEN_READ, &f) != VFS_OK || vfs_file_read(&f, chunk, 64, &n) != VFS_OK || n != 11 || memcmp(chunk, "legacy data", 11)) r = "v1 content wrong";
        if (f.generation) (void)vfs_file_close(&f);
        if (!r && (vfs_create_file("/volumes/disk/new") != VFS_READ_ONLY || vfs_unlink("/volumes/disk/old/data") != VFS_READ_ONLY || vfs_rename("/volumes/disk/old", "/volumes/disk/o2") != VFS_READ_ONLY)) r = "a v1 volume accepted a change";
        if (!r && ush_execute("ultrium storage sync") != USH_OK) r = "sync of a read-only v1 volume failed";
        if (!r && ush_execute("ultrium storage unmount /volumes/disk") != USH_OK) r = "v1 unmount failed";
    }
    if (!r && (block_memory_events(dev) != events || device_digest() != before)) r = "mounting a v1 volume modified the device image";
    if (!r && ush_execute("ultrium storage format ufst -y") != USH_OK) r = "an explicit confirmed format of a v1 disk failed";
    if (!r && storage_probe(dev) != STORAGE_V2) r = "reformat did not create UltriumFS 2";
    struct vfs_remove_report rr;
    (void)vfs_remove_tree("/volumes", &rr);
    if (ufs_mounted()) ufs_test_abandon();
    if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
    if (!block_memory_destroy(dev) && !r) r = "device destroy failed";
    dev = 0;
    if (!r && pmm_free_pages() != frames) r = "frame accounting changed";
    return r;
}

/* ---- I/O errors: every operation, every transfer ---- */
enum { OP_CREATE, OP_MKDIR, OP_WRITE, OP_RENAME, OP_UNLINK, OP_COUNT };
static enum vfs_error run_op(unsigned op) {
    switch (op) {
        case OP_CREATE: return fs->create_file("/dir/new");
        case OP_MKDIR: return fs->create_directory("/dir/sub");
        case OP_WRITE: return stream_write("/f1", 30000, 6, 30000, false);
        case OP_RENAME: return fs->rename("/f2", "/dir/moved");
        default: return fs->unlink("/f2");
    }
}
static const char *f_io_errors(void) {
    for (unsigned op = 0; op < OP_COUNT; ++op) {
        bool completed = false;
        for (int32_t k = 0; k < 400 && !completed; ++k) {
            const char *s = setup(4096, 1024, false);
            if (s) return teardown(s);
            if (fs->create_directory("/dir") != VFS_OK || stream_write("/f1", 9000, 1, BLOCK, true) != VFS_OK || stream_write("/f2", 5000, 2, BLOCK, true) != VFS_OK) FAIL("fixture failed");
            block_test_fail_after(k);
            enum vfs_error e = run_op(op);
            block_test_fail_after(-1);
            if (e == VFS_OK) completed = true;
            ufs2_fs_test_abandon();
            if (ufs2_fs_mount(dev, true, &report) != VFS_OK) FAIL("volume does not mount after an injected I/O error");
            if (!clean()) FAIL("volume inconsistent after an injected I/O error");
            { const char *r = stream_verify("/f2", 5000, 2); if (e != VFS_OK && op != OP_RENAME && op != OP_UNLINK && r) FAIL("an unrelated file was damaged"); }
            const char *t = teardown(0);
            if (t) return t;
        }
        if (!completed) return "I/O error sweep did not reach success";
    }
    return 0;
}

const struct test_case ufs2_fs_cases[] = {
    {"ufs2 fs format and mount", f_format_mount},
    {"ufs2 fs tree and error paths", f_tree_errors},
    {"ufs2 fs file I/O and growth", f_file_io},
    {"ufs2 fs file larger than 4 MiB", f_large_file},
    {"ufs2 fs fragmentation and reclamation", f_fragmentation},
    {"ufs2 fs extent map across leaf blocks", f_many_extents},
    {"ufs2 fs disk full and reuse", f_disk_full},
    {"ufs2 fs metadata exhaustion", f_metadata_exhaustion},
    {"ufs2 fs directory growth", f_directory_growth},
    {"ufs2 fs rename, busy items, handles", f_rename_busy},
    {"ufs2 fs persistence and unclean mount", f_persistence},
    {"ufs2 fs read-only and flush policy", f_read_only},
    {"ufs2 fs block sizes", f_block_sizes},
    {"ufs2 fs inode generations", f_generations},
    {"ufs2 fs extent limit (2783 extents, 11 leaves)", f_extent_limit},
    {"ufs2 fs structural damage is refused", f_structural_damage},
    {"ufs2 fs repairable damage mounts read-only", f_repairable_damage},
    {"ufs2 fs orphans are tolerated", f_orphans},
    {"ufs2 fs stale-generation entry is dangling", f_stale_generation},
    {"ufs2 fs stale state on read-only open", f_stale_state},
    {"ufs2 fs I/O errors in every operation", f_io_errors},
    {"ufs2 check: clean volume and read-only guarantee", k_clean_check},
    {"ufs2 check: damage reported, never written", k_check_damage_never_writes},
    {"ufs2 repair: bitmap", k_repair_bitmap},
    {"ufs2 repair: superblock copy and stale state", k_repair_superblock_and_state},
    {"ufs2 repair: committed journal transaction", k_repair_journal},
    {"ufs2 repair: orphans and device policy", k_repair_policy},
    {"ufs2 shell: format, mount, check, unmount", k_shell_commands},
    {"ufs2 v1 volumes: read-only, byte-identical", k_v1_read_only_policy}
};
const size_t ufs2_fs_case_count = sizeof(ufs2_fs_cases) / sizeof(ufs2_fs_cases[0]);
