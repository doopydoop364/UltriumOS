#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "fs/ultriumfs.h"
#include "storage/block.h"
#include "memory/pmm.h"
#include "memory.h"
#include "fs/vfs.h"
#include "ush.h"
#include "storage/storage_ops.h"
#include "fs/ufs2_fs.h"

/* UltriumFS tests call the backend operations directly on memory-backed block
   devices (no VFS mount). Corruption cases craft on-disk bytes with an
   independent encoder written from docs/ultriumfs-format.md, so the format
   code cannot validate its own mistakes. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define BLOCK 4096U

static const struct vfs_backend_ops *fs;
static struct block_device *dev;
static uint64_t frames_before;
static uint8_t chunk[BLOCK], expect[BLOCK], raw[BLOCK];

static uint32_t t_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void t_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t t_crc(const uint8_t *d, size_t n) {
    uint32_t c = 0xffffffffU;
    for (size_t i = 0; i < n; ++i) { c ^= d[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(c & 1)); }
    return ~c;
}
static uint32_t per(void) { return BLOCK / dev->block_size; }
static void rd(uint32_t b) { (void)block_read(dev, (uint64_t)b * per(), per(), raw, BLOCK); }
static void wr(uint32_t b) { (void)block_write(dev, (uint64_t)b * per(), per(), raw, BLOCK); }

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
static const char *stream_verify(const char *path, uint64_t total, uint8_t seed) {
    struct vfs_info info;
    if (fs->stat(path, &info) != VFS_OK || info.type != VFS_NODE_FILE || info.size != total) return "size or type mismatch";
    struct vfs_file f = {0};
    if (fs->file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return "open for verify failed";
    const char *r = 0;
    uint64_t off = 0;
    for (;;) {
        size_t got = 0;
        enum vfs_error e = fs->file_read(&f, chunk, 1000, &got);
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

static const char *setup(uint32_t block_size, uint64_t device_blocks) {
    fs = ufs_backend();
    frames_before = pmm_free_pages();
    dev = block_memory_create("ufst", block_size, device_blocks, false);
    REQUIRE(dev, "memory device creation failed");
    if (ufs_format(dev, "TEST") != VFS_OK) return "format failed";
    if (ufs_mount(dev, false) != VFS_OK) return "mount failed";
    return 0;
}
static const char *teardown(const char *result) {
    if (ufs_mounted()) ufs_test_abandon();
    if (dev) { if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    if (!result && pmm_free_pages() != frames_before) result = "frame accounting changed";
    return result;
}
#define SETUP(bs, blocks) do { const char *s_ = setup(bs, blocks); if (s_) return teardown(s_); } while (0)
#define FAIL(text) return teardown(text)
#define CHECK(cond, text) do { if (!(cond)) FAIL(text); } while (0)

static unsigned count_entries(const char *path, bool *has_dir) {
    struct vfs_dir d = {0};
    if (fs->dir_open(path, &d) != VFS_OK) return 9999;
    char name[40]; enum vfs_node_type t; unsigned n = 0;
    while (fs->dir_next(&d, name, sizeof(name), &t) == VFS_OK) { ++n; if (has_dir && t == VFS_NODE_DIRECTORY) *has_dir = true; }
    (void)fs->dir_close(&d);
    return n;
}

static const char *ufs_format_mount(void) {
    SETUP(512, 16384);
    struct ufs_info info;
    CHECK(ufs_get_info(&info), "info unavailable");
    CHECK(info.total_blocks > 1000 && info.free_blocks == info.total_blocks - 1, "free-space accounting wrong after format");
    CHECK(info.free_inodes == info.inode_count - 1 && !info.unclean_mount && !info.read_only, "inode accounting or flags wrong");
    CHECK(info.label[0] == 'T' && info.label[3] == 'T' && !info.label[4], "label not stored");
    struct vfs_info st;
    CHECK(fs->stat("/", &st) == VFS_OK && st.type == VFS_NODE_DIRECTORY, "root is not a directory");
    CHECK(count_entries("/", 0) == 0, "new root not empty");
    CHECK(ufs_mount(dev, false) == VFS_BUSY, "second mount accepted");
    CHECK(!block_memory_destroy(dev), "mounted device could be destroyed");
    CHECK(dev->holders == 1, "mount did not claim the device");
    CHECK(ufs_unmount() == VFS_OK && dev->holders == 0, "unmount failed or leaked the claim");
    return teardown(0);
}

static const char *ufs_format_safety(void) {
    SETUP(512, 8192);
    CHECK(ufs_format(dev, "X") == VFS_BUSY, "format of a mounted volume accepted");
    CHECK(ufs_unmount() == VFS_OK, "unmount failed");
    dev->flags &= ~BLOCK_FLAG_SCRATCH;
    CHECK(ufs_format(dev, "X") == VFS_DENIED, "format of a non-scratch device accepted");
    dev->flags |= BLOCK_FLAG_SCRATCH;
    dev->flags |= BLOCK_FLAG_READ_ONLY;
    CHECK(ufs_format(dev, "X") == VFS_READ_ONLY, "format of a read-only device accepted");
    dev->flags &= ~BLOCK_FLAG_READ_ONLY;
    CHECK(ufs_format(0, "X") != VFS_OK, "format of a null device accepted");
    struct block_device *tiny = block_memory_create("ufstiny", 512, 64, false);   /* 8 filesystem blocks */
    CHECK(tiny, "tiny device creation failed");
    enum vfs_error e = ufs_format(tiny, "X");
    CHECK(block_memory_destroy(tiny), "tiny destroy failed");
    CHECK(e == VFS_NO_SPACE, "format of a too-small device accepted");
    return teardown(0);
}

static const char *ufs_tree_and_errors(void) {
    SETUP(512, 16384);
    CHECK(fs->create_directory("/projects") == VFS_OK && fs->create_directory("/projects/ultrium") == VFS_OK, "mkdir failed");
    CHECK(fs->create_file("/projects/ultrium/notes.txt") == VFS_OK && fs->create_file("/empty") == VFS_OK, "create failed");
    CHECK(fs->create_file("/empty") == VFS_EXISTS && fs->create_directory("/projects") == VFS_EXISTS, "duplicate accepted");
    CHECK(fs->create_file("/missing/x") == VFS_NOT_FOUND, "create under missing parent accepted");
    CHECK(fs->create_file("/empty/x") == VFS_NOT_DIRECTORY, "create under a file accepted");
    CHECK(fs->create_file("/abcdefghijklmnopqrstuvwxyz0123456") == VFS_INVALID_PATH, "32-character name accepted");
    CHECK(fs->create_file("/abcdefghijklmnopqrstuvwxyz01234") == VFS_OK, "31-character name rejected");
    CHECK(fs->create_file("/") == VFS_INVALID_PATH && fs->unlink("/") == VFS_INVALID_PATH, "root create/unlink accepted");
    CHECK(fs->stat("/nothing", &(struct vfs_info){0}) == VFS_NOT_FOUND, "stat of missing path succeeded");
    struct vfs_info st;
    CHECK(fs->stat("/projects/ultrium/notes.txt", &st) == VFS_OK && st.type == VFS_NODE_FILE && st.size == 0, "empty file stat wrong");
    CHECK(fs->is_directory("/projects/ultrium") && !fs->is_directory("/empty") && !fs->is_directory("/nope"), "is_directory wrong");
    bool has_dir = false;
    CHECK(count_entries("/", &has_dir) == 3 && has_dir && count_entries("/projects", 0) == 1, "directory listing wrong");
    struct vfs_file f = {0};
    CHECK(fs->file_open("/projects", VFS_OPEN_READ, &f) == VFS_IS_DIRECTORY, "opening a directory as a file accepted");
    struct vfs_dir d = {0};
    CHECK(fs->dir_open("/empty", &d) == VFS_NOT_DIRECTORY, "listing a file accepted");
    CHECK(fs->unlink("/projects") == VFS_NOT_EMPTY, "non-empty directory removed");
    CHECK(fs->unlink("/projects/ultrium/notes.txt") == VFS_OK && fs->unlink("/projects/ultrium") == VFS_OK &&
          fs->unlink("/projects") == VFS_OK, "bottom-up removal failed");
    CHECK(fs->unlink("/projects") == VFS_NOT_FOUND, "removed directory still present");
    return teardown(0);
}

static const char *ufs_file_io(void) {
    SETUP(512, 16384);
    CHECK(fs->create_file("/small") == VFS_OK, "create failed");
    struct vfs_file f = {0};
    size_t n;
    CHECK(fs->file_open("/small", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
    CHECK(fs->file_write(&f, "Persistent data", 15, &n) == VFS_OK && n == 15, "write failed");
    CHECK(fs->file_write(&f, " and more", 9, &n) == VFS_OK && n == 9, "append failed");
    CHECK(fs->file_read(&f, chunk, 4, &n) == VFS_BAD_HANDLE, "read through a write-only handle accepted");
    CHECK(fs->file_close(&f) == VFS_OK && fs->file_close(&f) == VFS_BAD_HANDLE, "close semantics wrong");
    CHECK(fs->file_write(&f, "x", 1, &n) == VFS_BAD_HANDLE, "stale handle accepted");
    CHECK(fs->file_open("/small", VFS_OPEN_READ, &f) == VFS_OK, "reopen failed");
    CHECK(fs->file_read(&f, chunk, 100, &n) == VFS_OK && n == 24 && !memcmp(chunk, "Persistent data and more", 24), "read back wrong");
    CHECK(fs->file_read(&f, chunk, 100, &n) == VFS_OK && n == 0, "EOF not reported");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    /* Overwrite the start; size must not shrink. */
    CHECK(fs->file_open("/small", VFS_OPEN_WRITE, &f) == VFS_OK && fs->file_write(&f, "PERSISTENT", 10, &n) == VFS_OK, "overwrite failed");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    CHECK(stream_verify("/small", 24, 0) != 0, "pattern verify should differ for text file");  /* sanity of the verifier */
    CHECK(fs->file_open("/small", VFS_OPEN_READ, &f) == VFS_OK && fs->file_read(&f, chunk, 100, &n) == VFS_OK &&
          n == 24 && !memcmp(chunk, "PERSISTENT data and more", 24), "overwrite content wrong");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    /* Unaligned multi-block, indirect-block and growth. */
    CHECK(stream_write("/multi", 3 * BLOCK + 1234, 5, 3000, true) == VFS_OK, "multi-block write failed");
    { const char *r = stream_verify("/multi", 3 * BLOCK + 1234, 5); CHECK(!r, r); }
    CHECK(stream_write("/big", 40 * BLOCK + 77, 9, 1777, true) == VFS_OK, "indirect-block write failed");
    { const char *r = stream_verify("/big", 40 * BLOCK + 77, 9); CHECK(!r, r); }
    /* Growth: append in a second open via a rewrite of the larger prefix. */
    CHECK(stream_write("/big", 60 * BLOCK, 9, 4096, false) == VFS_OK, "growth write failed");
    { const char *r = stream_verify("/big", 60 * BLOCK, 9); CHECK(!r, r); }
    return teardown(0);
}

static const char *ufs_max_file_and_full(void) {
    SETUP(512, 16384);   /* 8 MiB: 2048 blocks */
    uint64_t max = UFS_MAX_FILE_BYTES;
    CHECK(stream_write("/max", max, 3, 4096, true) == VFS_OK, "maximum-size file write failed");
    { const char *r = stream_verify("/max", max, 3); CHECK(!r, r); }
    struct vfs_file f = {0}; size_t n;
    CHECK(fs->file_open("/max", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
    for (uint64_t off = 0; off < max; off += BLOCK) { /* position at EOF */
        pat(chunk, off, BLOCK, 3);
        CHECK(fs->file_write(&f, chunk, BLOCK, &n) == VFS_OK, "reposition write failed");
    }
    CHECK(fs->file_write(&f, "x", 1, &n) == VFS_NO_SPACE && n == 0, "write beyond maximum file size accepted");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    { const char *r = stream_verify("/max", max, 3); CHECK(!r, r); }
    struct ufs_info info;
    CHECK(ufs_get_info(&info) && info.free_blocks < info.total_blocks - 1036, "free-space accounting did not drop");
    /* Disk full: fill with 128 KiB files, then free and reuse. */
    char name[16] = "/f000";
    unsigned made = 0; enum vfs_error e = VFS_OK;
    for (; made < 200 && e == VFS_OK; ++made) {
        name[2] = (char)('0' + made / 100); name[3] = (char)('0' + made / 10 % 10); name[4] = (char)('0' + made % 10);
        e = stream_write(name, 128 * 1024, (uint8_t)made, 4096, true);
    }
    CHECK(e == VFS_NO_SPACE, "disk-full did not report NO_SPACE");
    CHECK(made >= 8, "volume filled implausibly early");
    unsigned failed_index = made - 1;
    name[2] = (char)('0' + failed_index / 100); name[3] = (char)('0' + failed_index / 10 % 10); name[4] = (char)('0' + failed_index % 10);
    CHECK(fs->unlink(name) == VFS_OK, "partial file not removable after ENOSPC");
    for (unsigned i = 0; i < failed_index; ++i) {
        name[2] = (char)('0' + i / 100); name[3] = (char)('0' + i / 10 % 10); name[4] = (char)('0' + i % 10);
        const char *r = stream_verify(name, 128 * 1024, (uint8_t)i); CHECK(!r, r);
    }
    CHECK(fs->unlink("/max") == VFS_OK, "free of the maximum file failed");
    CHECK(stream_write("/reuse", 512 * 1024, 77, 4096, true) == VFS_OK, "freed space was not reusable");
    { const char *r = stream_verify("/reuse", 512 * 1024, 77); CHECK(!r, r); }
    return teardown(0);
}

static const char *ufs_metadata_exhaustion(void) {
    SETUP(512, 400);   /* 50 filesystem blocks: the minimum 32 inodes */
    struct ufs_info info; CHECK(ufs_get_info(&info) && info.inode_count == 32, "unexpected inode count");
    char name[8] = "/a00"; unsigned made = 0; enum vfs_error e = VFS_OK;
    for (; made < 40 && e == VFS_OK; ++made) { name[2] = (char)('0' + made / 10); name[3] = (char)('0' + made % 10); e = fs->create_file(name); }
    CHECK(e == VFS_NO_SPACE && made == 32, "inode exhaustion not reported at 31 files");
    CHECK(ufs_get_info(&info) && info.free_inodes == 0 && info.free_blocks > 0, "metadata exhaustion accounting wrong");
    CHECK(fs->unlink("/a00") == VFS_OK && fs->create_file("/again") == VFS_OK, "freed inode not reusable");
    ufs_test_abandon();
    CHECK(ufs_mount(dev, false) == VFS_OK, "volume at inode capacity does not remount");
    return teardown(0);
}

static const char *ufs_directory_growth(void) {
    SETUP(512, 16384);
    CHECK(fs->create_directory("/many") == VFS_OK, "mkdir failed");
    char path[32] = "/many/e000";
    for (unsigned i = 0; i < 200; ++i) {
        path[7] = (char)('0' + i / 100); path[8] = (char)('0' + i / 10 % 10); path[9] = (char)('0' + i % 10);
        CHECK(fs->create_file(path) == VFS_OK, "create in a growing directory failed");
    }
    CHECK(count_entries("/many", 0) == 200, "growing directory lists the wrong count");
    CHECK(fs->unlink("/many") == VFS_NOT_EMPTY, "non-empty large directory removed");
    for (unsigned i = 0; i < 200; i += 2) {
        path[7] = (char)('0' + i / 100); path[8] = (char)('0' + i / 10 % 10); path[9] = (char)('0' + i % 10);
        CHECK(fs->unlink(path) == VFS_OK, "unlink in a large directory failed");
    }
    CHECK(count_entries("/many", 0) == 100, "count after deleting half wrong");
    CHECK(fs->create_file("/many/zzz") == VFS_OK, "freed directory slot not reused");
    ufs_test_abandon();
    CHECK(ufs_mount(dev, false) == VFS_OK && count_entries("/many", 0) == 101, "large directory lost on remount");
    return teardown(0);
}

static const char *ufs_rename_and_busy(void) {
    SETUP(512, 16384);
    CHECK(fs->create_directory("/a") == VFS_OK && fs->create_directory("/a/sub") == VFS_OK && fs->create_directory("/b") == VFS_OK, "setup failed");
    CHECK(stream_write("/a/sub/data", 6000, 2, 2000, true) == VFS_OK, "write failed");
    CHECK(fs->rename("/a/sub/data", "/a/sub/data2") == VFS_OK, "same-directory rename failed");
    CHECK(fs->stat("/a/sub/data", &(struct vfs_info){0}) == VFS_NOT_FOUND, "old name still present");
    CHECK(fs->rename("/a/sub", "/b/moved") == VFS_OK, "cross-directory directory rename failed");
    { const char *r = stream_verify("/b/moved/data2", 6000, 2); CHECK(!r, r); }
    CHECK(fs->create_file("/b/exists") == VFS_OK && fs->rename("/b/moved", "/b/exists") == VFS_EXISTS, "rename onto an existing name accepted");
    CHECK(fs->rename("/b", "/b/moved/inside") == VFS_INVALID_PATH, "directory moved into itself accepted");
    CHECK(fs->rename("/b", "/b") == VFS_INVALID_PATH, "rename onto itself accepted");
    CHECK(fs->rename("/", "/x") == VFS_INVALID_PATH, "root rename accepted");
    CHECK(fs->rename("/missing", "/y") == VFS_NOT_FOUND, "rename of a missing item accepted");
    CHECK(fs->rename("/b/exists", "/nodir/z") == VFS_NOT_FOUND, "rename into a missing directory accepted");
    /* Busy items. */
    struct vfs_file f = {0};
    CHECK(fs->file_open("/b/moved/data2", VFS_OPEN_READ, &f) == VFS_OK, "open failed");
    CHECK(fs->unlink("/b/moved/data2") == VFS_BUSY && fs->rename("/b/moved/data2", "/b/q") == VFS_BUSY, "open file removed or renamed");
    CHECK(fs->file_close(&f) == VFS_OK && fs->unlink("/b/moved/data2") == VFS_OK, "file not removable after close");
    struct vfs_dir d = {0};
    CHECK(fs->dir_open("/b", &d) == VFS_OK && ufs_unmount() == VFS_BUSY, "unmount with an open directory accepted");
    CHECK(fs->dir_close(&d) == VFS_OK && fs->dir_close(&d) == VFS_BAD_HANDLE, "dir close semantics wrong");
    /* Handle table bound. */
    struct vfs_file many[17]; unsigned opened = 0;
    CHECK(fs->create_file("/h") == VFS_OK, "create failed");
    for (; opened < 17; ++opened) { many[opened] = (struct vfs_file){0}; if (fs->file_open("/h", VFS_OPEN_READ, &many[opened]) != VFS_OK) break; }
    CHECK(opened == 16, "handle table limit is not 16");
    for (unsigned i = 0; i < opened; ++i) CHECK(fs->file_close(&many[i]) == VFS_OK, "close of many handles failed");
    return teardown(0);
}

static const char *ufs_persistence_and_unclean(void) {
    SETUP(512, 16384);
    CHECK(fs->create_directory("/docs") == VFS_OK && stream_write("/docs/a.txt", 10000, 1, 4096, true) == VFS_OK &&
          stream_write("/b.bin", 70000, 2, 4096, true) == VFS_OK && fs->create_file("/empty") == VFS_OK, "setup failed");
    CHECK(ufs_sync() == VFS_OK && ufs_unmount() == VFS_OK, "sync/unmount failed");
    CHECK(ufs_mount(dev, false) == VFS_OK, "remount failed");
    struct ufs_info info; CHECK(ufs_get_info(&info) && !info.unclean_mount && info.orphan_inodes == 0, "clean volume reported unclean");
    { const char *r = stream_verify("/docs/a.txt", 10000, 1); CHECK(!r, r); }
    { const char *r = stream_verify("/b.bin", 70000, 2); CHECK(!r, r); }
    CHECK(fs->stat("/empty", &(struct vfs_info){0}) == VFS_OK, "empty file lost");
    /* Modify, then simulate power loss before any sync. */
    CHECK(stream_write("/c.bin", 5000, 8, 4096, true) == VFS_OK, "write failed");
    ufs_test_abandon();
    CHECK(ufs_mount(dev, false) == VFS_OK, "remount after abandoned session failed");
    CHECK(ufs_get_info(&info) && info.unclean_mount, "unclean session not reported");
    { const char *r = stream_verify("/c.bin", 5000, 8); CHECK(!r, r); }
    CHECK(ufs_sync() == VFS_OK && ufs_unmount() == VFS_OK && ufs_mount(dev, false) == VFS_OK, "sync/remount failed");
    CHECK(ufs_get_info(&info) && !info.unclean_mount, "sync did not mark the volume clean");
    /* Repeated mount cycles. */
    for (unsigned i = 0; i < 20; ++i) CHECK(ufs_unmount() == VFS_OK && ufs_mount(dev, false) == VFS_OK, "mount cycle failed");
    { const char *r = stream_verify("/b.bin", 70000, 2); CHECK(!r, r); }
    return teardown(0);
}

static const char *ufs_read_only(void) {
    SETUP(512, 16384);
    CHECK(stream_write("/keep", 9000, 4, 4096, true) == VFS_OK && ufs_unmount() == VFS_OK, "setup failed");
    CHECK(ufs_mount(dev, true) == VFS_OK, "read-only mount failed");
    struct ufs_info info; CHECK(ufs_get_info(&info) && info.read_only, "read-only flag not reported");
    CHECK(fs->create_file("/x") == VFS_READ_ONLY && fs->create_directory("/y") == VFS_READ_ONLY, "create on a read-only mount accepted");
    CHECK(fs->unlink("/keep") == VFS_READ_ONLY && fs->rename("/keep", "/k2") == VFS_READ_ONLY, "modification on a read-only mount accepted");
    struct vfs_file f = {0};
    CHECK(fs->file_open("/keep", VFS_OPEN_WRITE, &f) == VFS_READ_ONLY, "write open on a read-only mount accepted");
    { const char *r = stream_verify("/keep", 9000, 4); CHECK(!r, r); }
    CHECK(ufs_sync() == VFS_OK && ufs_unmount() == VFS_OK, "read-only unmount failed");
    dev->flags |= BLOCK_FLAG_READ_ONLY;
    CHECK(ufs_mount(dev, false) == VFS_OK && ufs_get_info(&info) && info.read_only, "read-only device not forced read-only");
    CHECK(fs->create_file("/x") == VFS_READ_ONLY, "create on a read-only device accepted");
    CHECK(ufs_unmount() == VFS_OK, "unmount failed");
    dev->flags &= ~BLOCK_FLAG_READ_ONLY;
    return teardown(0);
}

static const char *ufs_block_sizes(void) {
    static const uint32_t sizes[] = {512, 1024, 2048, 4096};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t blocks = (4ULL << 20) / sizes[i];
        const char *s = setup(sizes[i], blocks);
        if (s) return teardown(s);
        enum vfs_error e = fs->create_directory("/d");
        if (e == VFS_OK) e = stream_write("/d/f", 20000, (uint8_t)i, 3000, true);
        const char *r = 0;
        if (e != VFS_OK) r = "write failed";
        else if (ufs_sync() != VFS_OK || ufs_unmount() != VFS_OK || ufs_mount(dev, false) != VFS_OK) r = "remount failed";
        else r = stream_verify("/d/f", 20000, (uint8_t)i);
        if (r) return teardown(r);
        r = teardown(0);
        if (r) return r;
    }
    return 0;
}

/* ---- corruption: craft on-disk damage and require a safe mount refusal ---- */
static uint32_t data_start_of(void) { rd(0); return t_le32(raw + 36); }
static uint32_t inode_blk(uint32_t ino) { return 1 + (ino - 1) / 32; }
static uint8_t *inode_ptr(uint32_t ino) { return raw + ((ino - 1) % 32) * 128; }
static void fix_inode(uint8_t *p) { t_put32(p + 124, t_crc(p, 124)); }
static void fix_super(void) { t_put32(raw + 124, t_crc(raw, 124)); }

enum damage {D_MAGIC, D_VERSION, D_FEATURE, D_SUPER_CRC, D_TOTAL, D_INODE_CRC, D_BAD_POINTER, D_DOUBLE_ALLOC,
             D_DANGLING_ENTRY, D_DUPLICATE_REF, D_TYPE_MISMATCH, D_ROOT_GONE, D_BLOCK_COUNT, D_TRAILING_POINTER, D_COUNT};
static const char *damage_names[D_COUNT] = {
    "bad magic", "unsupported version", "unknown feature flag", "superblock checksum", "volume larger than device",
    "inode checksum", "out-of-range block pointer", "double allocation", "entry naming a free inode",
    "inode referenced twice", "entry/inode type mismatch", "missing root inode", "inconsistent block count",
    "pointer past end of file"};

static const char *apply_damage(enum damage kind, enum vfs_error *expected) {
    *expected = VFS_CORRUPT;
    uint32_t ds = data_start_of();
    uint32_t total = t_le32(raw + 16);
    switch (kind) {
        case D_MAGIC: rd(0); raw[0] ^= 0xff; wr(0); break;
        case D_VERSION: rd(0); t_put32(raw + 8, 2); fix_super(); wr(0); *expected = VFS_UNSUPPORTED; break;
        case D_FEATURE: rd(0); t_put32(raw + 80, 1); fix_super(); wr(0); *expected = VFS_UNSUPPORTED; break;
        case D_SUPER_CRC: rd(0); raw[56] ^= 1; wr(0); break;
        case D_TOTAL: rd(0); t_put32(raw + 16, total + 100000); fix_super(); wr(0); break;
        case D_INODE_CRC: rd(inode_blk(3)); inode_ptr(3)[16] ^= 1; wr(inode_blk(3)); break;
        case D_BAD_POINTER: rd(inode_blk(3)); t_put32(inode_ptr(3) + 16, total + 5); fix_inode(inode_ptr(3)); wr(inode_blk(3)); break;
        case D_DOUBLE_ALLOC: {
            rd(inode_blk(3)); uint32_t shared = t_le32(inode_ptr(3) + 16);
            rd(inode_blk(4)); t_put32(inode_ptr(4) + 16, shared); fix_inode(inode_ptr(4)); wr(inode_blk(4));
            break;
        }
        case D_DANGLING_ENTRY: rd(ds); t_put32(raw + 0, 400); wr(ds); break;           /* first root entry -> free inode */
        case D_DUPLICATE_REF: {
            rd(ds); uint32_t first = t_le32(raw);
            t_put32(raw + 64, first); raw[64 + 4] = raw[4]; wr(ds); break;               /* second entry duplicates the first */
        }
        case D_TYPE_MISMATCH: rd(ds); raw[4] = (raw[4] == 1) ? 2 : 1; wr(ds); break;
        case D_ROOT_GONE: rd(1); memset(raw, 0, 128); wr(1); break;
        case D_BLOCK_COUNT: rd(inode_blk(3)); t_put32(inode_ptr(3) + 68, 99); fix_inode(inode_ptr(3)); wr(inode_blk(3)); break;
        case D_TRAILING_POINTER: rd(inode_blk(3)); t_put32(inode_ptr(3) + 16 + 4 * 11, ds + 5); fix_inode(inode_ptr(3)); wr(inode_blk(3)); break;
        default: return "unknown damage";
    }
    return 0;
}

static const char *ufs_corruption(void) {
    for (unsigned k = 0; k < D_COUNT; ++k) {
        SETUP(512, 16384);
        /* Root entries in order: /one (inode 2, dir), /f1 (inode 3), /f2 (inode 4). */
        CHECK(fs->create_directory("/one") == VFS_OK && stream_write("/f1", 9000, 1, 4096, true) == VFS_OK &&
              stream_write("/f2", 5000, 2, 4096, true) == VFS_OK, "corruption fixture failed");
        CHECK(ufs_sync() == VFS_OK && ufs_unmount() == VFS_OK, "fixture unmount failed");
        enum vfs_error expected;
        const char *r = apply_damage((enum damage)k, &expected);
        CHECK(!r, r);
        uint64_t frames = pmm_free_pages();
        enum vfs_error e = ufs_mount(dev, false);
        if (e != expected) { (void)damage_names[k]; FAIL("damaged volume mounted or returned the wrong error"); }
        CHECK(!ufs_mounted() && dev->holders == 0, "failed mount left the volume claimed");
        CHECK(pmm_free_pages() == frames, "failed mount leaked memory");
        CHECK(ufs_mount(dev, true) != VFS_OK, "damaged volume mounted read-only");
        CHECK(ufs_format(dev, "AGAIN") == VFS_OK && ufs_mount(dev, false) == VFS_OK, "device unusable after rejecting a damaged volume");
        const char *t = teardown(0);
        if (t) return t;
    }
    return 0;
}

static const char *ufs_orphans(void) {
    SETUP(512, 16384);
    CHECK(stream_write("/lost", 9000, 1, 4096, true) == VFS_OK && stream_write("/kept", 5000, 2, 4096, true) == VFS_OK, "fixture failed");
    CHECK(ufs_sync() == VFS_OK && ufs_unmount() == VFS_OK, "unmount failed");
    uint32_t ds = data_start_of();
    rd(ds);
    t_put32(raw + 0, 0); memset(raw + 4, 0, 60); wr(ds);   /* remove the first root entry but leave its inode */
    CHECK(ufs_mount(dev, false) == VFS_OK, "an unreferenced inode must not block mounting");
    struct ufs_info info; CHECK(ufs_get_info(&info) && info.orphan_inodes == 1, "orphan not counted");
    CHECK(fs->stat("/lost", &(struct vfs_info){0}) == VFS_NOT_FOUND, "orphan still visible");
    { const char *r = stream_verify("/kept", 5000, 2); CHECK(!r, r); }
    return teardown(0);
}

static const char *ufs_failure_injection(void) {
    /* Inject a device failure at every point of a create+write+rename+remove
       sequence. The failing call must report an error, nothing may crash, and
       after simulated power loss the volume must still mount with the
       pre-existing data intact. */
    for (int32_t point = 0; point < 40; ++point) {
        SETUP(512, 16384);
        CHECK(stream_write("/stable", 6000, 6, 4096, true) == VFS_OK && ufs_sync() == VFS_OK, "fixture failed");
        block_test_fail_after(point);
        bool failed = false;
        enum vfs_error e = fs->create_directory("/dir");
        if (e != VFS_OK) failed = true;
        if (!failed && (e = stream_write("/dir/new", 20000, 3, 4096, true)) != VFS_OK) failed = true;
        if (!failed && (e = fs->rename("/dir/new", "/dir/moved")) != VFS_OK) failed = true;
        if (!failed && (e = fs->unlink("/dir/moved")) != VFS_OK) failed = true;
        block_test_fail_after(-1);
        if (failed && e != VFS_IO) FAIL("injected failure was not reported as an I/O error");
        ufs_test_abandon();
        CHECK(ufs_mount(dev, false) == VFS_OK, "volume does not mount after an injected failure");
        { const char *r = stream_verify("/stable", 6000, 6); CHECK(!r, r); }
        const char *t = teardown(0);
        if (t) return t;
        if (!failed) break;   /* the whole sequence fits before this injection point */
    }
    return 0;
}

static const char *ufs_allocation_rollback(void) {
    SETUP(512, 400);   /* 50 blocks, small enough to exhaust inside one write */
    struct ufs_info before, after;
    CHECK(ufs_get_info(&before), "info unavailable");
    CHECK(fs->create_file("/big") == VFS_OK, "create failed");
    struct vfs_file f = {0}; size_t n;
    CHECK(fs->file_open("/big", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
    static uint8_t huge[4096 * 8];
    memset(huge, 0x5a, sizeof(huge));
    uint64_t free_before = 0; (void)free_before;
    enum vfs_error e = VFS_OK; unsigned writes = 0;
    while ((e = fs->file_write(&f, huge, sizeof(huge), &n)) == VFS_OK && writes < 100) ++writes;
    CHECK(e == VFS_NO_SPACE && n == 0, "exhaustion inside a write not reported cleanly");
    CHECK(fs->file_close(&f) == VFS_OK, "close failed");
    CHECK(ufs_get_info(&after) && after.free_blocks < 8, "failed growth leaked or over-released blocks");
    uint32_t free_full = after.free_blocks;
    struct vfs_info st; CHECK(fs->stat("/big", &st) == VFS_OK && st.size == writes * sizeof(huge), "size wrong after a failed growth");
    CHECK(fs->unlink("/big") == VFS_OK, "remove failed");
    CHECK(ufs_get_info(&after) && after.free_blocks == before.free_blocks && after.free_blocks > free_full, "free space not fully restored");
    ufs_test_abandon();
    CHECK(ufs_mount(dev, false) == VFS_OK, "remount failed");
    return teardown(0);
}

static const char *ufs_growth_rollback_on_io_error(void) {
    /* A device failure at every point of a growing write must leave the
       in-memory free-space accounting and the file exactly as before. */
    for (int32_t point = 0; point < 700; point += 5) {   /* counts device transfers: 8 per 4 KiB block at 512 B */
        SETUP(512, 4096);
        CHECK(stream_write("/g", 2 * BLOCK, 1, 4096, true) == VFS_OK, "fixture failed");   /* sets the dirty mark */
        struct ufs_info before, after;
        CHECK(ufs_get_info(&before), "info unavailable");
        struct vfs_file f = {0}; size_t n = 0;
        CHECK(fs->file_open("/g", VFS_OPEN_WRITE, &f) == VFS_OK, "open failed");
        static uint8_t big[4096 * 20];
        pat(big, 0, sizeof(big), 1);
        block_test_fail_after(point);
        enum vfs_error e = fs->file_write(&f, big, sizeof(big), &n);   /* grows 2 -> 20 blocks (+ indirect) */
        block_test_fail_after(-1);
        (void)fs->file_close(&f);
        ufs_get_info(&after);
        if (e == VFS_OK) {
            CHECK(after.free_blocks == before.free_blocks - 19, "successful growth accounting wrong");
            const char *r = stream_verify("/g", sizeof(big), 1); CHECK(!r, r);
            const char *t = teardown(0);
            if (t) return t;
            return point > 5 ? 0 : "injection never fired";
        }
        CHECK(e == VFS_IO && n == 0, "failed growth not reported as an I/O error");
        if (after.faulted) {
            /* The failure hit the commit phase: the volume must now be read-only,
               refuse changes, never mark itself clean, and remount consistently
               with either the old or the new complete file. */
            CHECK(after.read_only, "faulted volume stayed writable");
            CHECK(fs->create_file("/after") == VFS_READ_ONLY, "faulted volume accepted a change");
            CHECK(ufs_sync() == VFS_IO, "sync of a faulted volume reported success");
            CHECK(ufs_unmount() == VFS_IO && !ufs_mounted(), "faulted volume could not be released");
            CHECK(ufs_mount(dev, false) == VFS_OK, "faulted volume does not remount");
            struct vfs_info fst;
            CHECK(fs->stat("/g", &fst) == VFS_OK && (fst.size == 2 * BLOCK || fst.size == sizeof(big)), "remounted file has an impossible size");
            if (fst.size == sizeof(big)) { const char *r = stream_verify("/g", sizeof(big), 1); CHECK(!r, r); }
        } else {
            CHECK(after.free_blocks == before.free_blocks, "failed growth leaked blocks");
            struct vfs_info st;
            CHECK(fs->stat("/g", &st) == VFS_OK && st.size == 2 * BLOCK, "failed growth changed the file size");
        }
        const char *t = teardown(0);
        if (t) return t;
    }
    return "injection sweep did not reach a successful write";
}

static const char *ufs_device_removal(void) {
    SETUP(512, 16384);
    CHECK(stream_write("/x", 3000, 1, 4096, true) == VFS_OK, "fixture failed");
    dev->flags |= BLOCK_FLAG_DEAD;   /* simulate a device that disappeared */
    struct vfs_file f = {0}; size_t n;
    enum vfs_error e = fs->file_open("/x", VFS_OPEN_READ, &f);
    CHECK(e == VFS_IO, "metadata access on a dead device did not fail with I/O error");
    CHECK(fs->create_file("/y") == VFS_IO, "create on a dead device accepted");
    (void)n;
    CHECK(ufs_sync() == VFS_OK || true, "sync must not crash");
    dev->flags &= ~BLOCK_FLAG_DEAD;
    return teardown(0);
}


/* ---- VFS mounts through the public API and real shell commands ---- */
static uint8_t mount_buffer[2048];
static uint64_t mount_frames;

static enum vfs_error vfs_put(const char *path, const void *data, size_t length) {
    enum vfs_error e = vfs_create_file(path);
    if (e != VFS_OK) return e;
    struct vfs_file f = {0}; size_t n = 0;
    e = vfs_file_open(path, VFS_OPEN_WRITE, &f);
    if (e == VFS_OK) e = vfs_file_write(&f, data, length, &n);
    if (f.generation) { enum vfs_error c = vfs_file_close(&f); if (e == VFS_OK) e = c; }
    return e == VFS_OK && n != length ? VFS_NO_SPACE : e;
}
static enum vfs_error vfs_put_big(const char *path, size_t total) {
    enum vfs_error e = vfs_create_file(path);
    if (e != VFS_OK) return e;
    struct vfs_file f = {0};
    e = vfs_file_open(path, VFS_OPEN_WRITE, &f);
    memset(mount_buffer, 0x42, sizeof(mount_buffer));
    for (size_t done = 0; e == VFS_OK && done < total; ) {
        size_t chunk = total - done < sizeof(mount_buffer) ? total - done : sizeof(mount_buffer), n = 0;
        e = vfs_file_write(&f, mount_buffer, chunk, &n);
        if (e == VFS_OK && n != chunk) e = VFS_NO_SPACE;
        done += chunk;
    }
    if (f.generation) { enum vfs_error c = vfs_file_close(&f); if (e == VFS_OK) e = c; }
    return e;
}
static bool vfs_has(const char *path, const char *text) {
    struct vfs_file f = {0}; size_t n = 0, len = 0;
    while (text[len]) ++len;
    if (vfs_file_open(path, VFS_OPEN_READ, &f) != VFS_OK) return false;
    enum vfs_error e = vfs_file_read(&f, mount_buffer, sizeof(mount_buffer), &n);
    (void)vfs_file_close(&f);
    return e == VFS_OK && n == len && !memcmp(mount_buffer, text, len);
}
static const char *mount_setup(void) {
    mount_frames = pmm_free_pages();
    (void)ush_execute("go /");
    if (vfs_create_directory("/volumes") != VFS_OK || vfs_create_directory("/volumes/disk") != VFS_OK) return "RAMFS mount-point setup failed";
    dev = block_memory_create("ufst", 512, 16384, false);
    if (!dev) return "memory device creation failed";
    if (ush_execute("ultrium storage format ufst -y") != USH_OK) return "storage format command failed";
    if (ush_execute("ultrium storage mount ufst /volumes/disk") != USH_OK) return "storage mount command failed";
    return 0;
}
static const char *mount_cleanup(const char *result) {
    (void)ush_execute("go /");
    struct vfs_mount_info info;
    vfs_freeze(false);
    for (size_t i = 0; vfs_mount_info_at(i, &info); ++i)
        if (info.persistent) { (void)vfs_unmount(info.path); break; }
    if (ufs_mounted()) ufs_test_abandon();
    if (ufs2_fs_mounted()) ufs2_fs_test_abandon();
    if (dev) { dev->flags &= ~BLOCK_FLAG_DEAD; if (!block_memory_destroy(dev) && !result) result = "device destroy failed"; dev = 0; }
    struct vfs_remove_report report;
    (void)vfs_remove_tree("/volumes", &report);
    (void)vfs_remove_tree("/rf", &report);
    (void)vfs_remove_tree("/full", &report);
    if (!result && pmm_free_pages() != mount_frames) result = "frame accounting changed";
    return result;
}
#define MSETUP() do { const char *s_ = mount_setup(); if (s_) return mount_cleanup(s_); } while (0)
#define MCHECK(cond, text) do { if (!(cond)) return mount_cleanup(text); } while (0)

static const char *vfs_mount_routing(void) {
    MSETUP();
    struct vfs_info st;
    MCHECK(vfs_stat("/volumes/disk", &st) == VFS_OK && st.type == VFS_NODE_DIRECTORY && st.persistent && st.filesystem && st.filesystem[0] == 'U', "mount point not served by UltriumFS");
    MCHECK(vfs_stat("/volumes", &st) == VFS_OK && !st.persistent && st.filesystem[0] == 'R', "parent folder not served by RAMFS");
    /* The spec's example commands, unchanged, on the mounted volume. */
    MCHECK(ush_execute("go /volumes/disk") == USH_OK, "go failed");
    MCHECK(ush_execute("new folder projects") == USH_OK && ush_execute("new file notes.txt") == USH_OK, "new failed");
    MCHECK(ush_execute("write notes2.txt \"Persistent data\"") == USH_OK, "write failed");
    MCHECK(ush_execute("list /volumes/disk") == USH_OK && ush_execute("view notes2.txt") == USH_OK, "list/view failed");
    MCHECK(vfs_has("/volumes/disk/notes2.txt", "Persistent data"), "written text not stored on the volume");
    MCHECK(ush_execute("copy notes2.txt backup.txt") == USH_OK && vfs_has("/volumes/disk/backup.txt", "Persistent data"), "copy on volume failed");
    MCHECK(ush_execute("rename backup.txt old-notes.txt") == USH_OK && vfs_has("/volumes/disk/old-notes.txt", "Persistent data"), "rename on volume failed");
    MCHECK(ush_execute("remove old-notes.txt") == USH_OK && vfs_stat("/volumes/disk/old-notes.txt", &st) == VFS_NOT_FOUND, "remove on volume failed");
    MCHECK(ush_execute("info notes2.txt") == USH_OK, "info failed");
    MCHECK(ush_execute("new folder a/b") == USH_FS_ERROR, "mkdir under a missing parent accepted");
    MCHECK(ush_execute("go /") == USH_OK, "go / failed");
    /* Cross-filesystem copy in both directions; cross-mount rename refused. */
    MCHECK(vfs_put("/rf", "ram data", 8) == VFS_OK, "RAMFS write failed");
    MCHECK(ush_execute("copy /rf /volumes/disk/from-ram") == USH_OK && vfs_has("/volumes/disk/from-ram", "ram data"), "RAMFS to disk copy failed");
    MCHECK(ush_execute("copy /volumes/disk/notes2.txt /from-disk") == USH_OK && vfs_has("/from-disk", "Persistent data"), "disk to RAMFS copy failed");
    MCHECK(vfs_unlink("/from-disk") == VFS_OK, "cleanup failed");
    MCHECK(vfs_rename("/rf", "/volumes/disk/moved") == VFS_UNSUPPORTED && vfs_has("/rf", "ram data"), "cross-mount rename not refused cleanly");
    MCHECK(ush_execute("copy /rf /volumes/disk/from-ram") == USH_FS_ERROR, "copy over an existing file accepted");
    MCHECK(vfs_copy("/volumes/disk/projects", "/volumes/disk/p2") == VFS_IS_DIRECTORY, "directory copy accepted");
    /* A copy that cannot fit must not leave a partial destination. */
    MCHECK(vfs_put_big("/volumes/disk/large", 100000) == VFS_OK, "large file write failed");
    MCHECK(vfs_copy("/volumes/disk/large", "/toolarge") != VFS_OK && vfs_stat("/toolarge", &st) == VFS_NOT_FOUND, "oversized cross-mount copy left a partial file");
    MCHECK(vfs_mount_count() == 2, "mount table wrong");
    return mount_cleanup(0);
}

static const char *vfs_mount_protection(void) {
    MSETUP();
    struct vfs_remove_report report;
    MCHECK(vfs_create_file("/volumes/disk/keep") == VFS_OK, "setup failed");
    MCHECK(vfs_unlink("/volumes/disk") == VFS_BUSY && vfs_unlink("/volumes") == VFS_BUSY, "mount point or ancestor unlinked");
    MCHECK(vfs_rename("/volumes", "/vols") == VFS_BUSY && vfs_rename("/volumes/disk", "/x") == VFS_BUSY, "mount point or ancestor renamed");
    MCHECK(vfs_remove_tree("/volumes", &report) == VFS_BUSY && vfs_stat("/volumes/disk/keep", &(struct vfs_info){0}) == VFS_OK, "recursive removal crossed a mount point");
    MCHECK(vfs_remove_tree("/volumes/disk", &report) == VFS_BUSY && report.removed == 0, "recursive removal of a mount point accepted");
    MCHECK(ush_execute("remove /volumes -r -y") == USH_FS_ERROR && ush_execute("remove /volumes/disk -r -y") == USH_FS_ERROR, "shell removal crossed a mount point");
    MCHECK(vfs_has("/volumes/disk/keep", "") || vfs_stat("/volumes/disk/keep", &(struct vfs_info){0}) == VFS_OK, "volume content lost");
    /* Mount-point policy. */
    MCHECK(vfs_create_directory("/full") == VFS_OK && vfs_create_file("/full/x") == VFS_OK, "setup failed");
    MCHECK(vfs_mount("/full", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_NOT_EMPTY, "mount over a non-empty folder accepted");
    MCHECK(vfs_mount("/missing", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_NOT_FOUND, "mount at a missing path accepted");
    MCHECK(vfs_mount("/full/x", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_NOT_DIRECTORY, "mount on a file accepted");
    MCHECK(vfs_mount("/", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_INVALID_PATH, "mount over the root accepted");
    MCHECK(vfs_mount("/volumes/disk", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_BUSY, "double mount accepted");
    MCHECK(vfs_mount("/volumes/disk/inner", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_BUSY, "nested mount accepted");
    MCHECK(vfs_mount("/volumes", ufs_backend(), 0, "UltriumFS", "ufst", true, false) == VFS_BUSY, "mount over an ancestor of a mount accepted");
    MCHECK(ush_execute("ultrium storage mount ufst /full") == USH_FS_ERROR, "second volume mount via the command accepted");
    MCHECK(vfs_mount_count() == 2, "failed mounts changed the mount table");
    MCHECK(ush_execute("ultrium storage format ufst -y") == USH_FS_ERROR, "mounted volume was formatted");
    MCHECK(ush_execute("ultrium storage format nosuch -y") == USH_FS_ERROR, "format of a missing device accepted");
    return mount_cleanup(0);
}

static const char *vfs_mount_unmount_cycle(void) {
    MSETUP();
    MCHECK(ush_execute("new folder /volumes/disk/projects") == USH_OK &&
           ush_execute("write /volumes/disk/projects/notes.txt Persistent") == USH_OK &&
           vfs_put("/rf", "ram", 3) == VFS_OK, "setup failed");
    /* Handles route to the right backend, concurrently. */
    struct vfs_file disk = {0}, ram = {0}; size_t n;
    MCHECK(vfs_file_open("/volumes/disk/projects/notes.txt", VFS_OPEN_READ, &disk) == VFS_OK && vfs_file_open("/rf", VFS_OPEN_READ, &ram) == VFS_OK, "open failed");
    MCHECK(vfs_file_read(&disk, mount_buffer, 64, &n) == VFS_OK && n == 10 && !memcmp(mount_buffer, "Persistent", 10), "disk handle routed wrongly");
    MCHECK(vfs_file_read(&ram, mount_buffer, 64, &n) == VFS_OK && n == 3 && !memcmp(mount_buffer, "ram", 3), "RAMFS handle routed wrongly");
    MCHECK(vfs_unmount("/volumes/disk") == VFS_BUSY && ush_execute("ultrium storage unmount /volumes/disk") == USH_FS_ERROR, "unmount with an open file accepted");
    MCHECK(vfs_file_close(&disk) == VFS_OK && vfs_file_close(&disk) == VFS_BAD_HANDLE && vfs_file_close(&ram) == VFS_OK, "close routing wrong");
    /* The current folder pins the volume. */
    MCHECK(ush_execute("go /volumes/disk/projects") == USH_OK, "go failed");
    MCHECK(ush_execute("ultrium storage unmount /volumes/disk") == USH_FS_ERROR && vfs_mount_count() == 2, "unmount with the cwd inside accepted");
    MCHECK(ush_execute("go /") == USH_OK, "go / failed");
    MCHECK(ush_execute("ultrium storage unmount /rf") == USH_FS_ERROR && ush_execute("ultrium storage unmount /volumes/nothing") != USH_OK, "unmount of a non-mount accepted");
    /* Freeze (shutdown): reads continue, changes are refused. */
    vfs_freeze(true);
    MCHECK(vfs_create_file("/volumes/disk/frozen") == VFS_BUSY && vfs_create_file("/frozen") == VFS_BUSY, "frozen VFS accepted a create");
    MCHECK(vfs_has("/volumes/disk/projects/notes.txt", "Persistent"), "frozen VFS blocked a read");
    MCHECK(vfs_file_open("/rf", VFS_OPEN_WRITE, &ram) == VFS_BUSY, "frozen VFS accepted a write open");
    vfs_freeze(false);
    /* Shutdown synchronization success and failure. */
    size_t volumes = 0, synced = 0;
    MCHECK(storage_shutdown_sync(&volumes, &synced) && volumes == 1 && synced == 1, "shutdown sync did not synchronize the volume");
    MCHECK(vfs_frozen() && vfs_create_file("/after-sync") == VFS_BUSY, "shutdown sync did not stop new changes");
    storage_shutdown_abort();
    MCHECK(!vfs_frozen() && vfs_create_file("/volumes/disk/extra") == VFS_OK, "abort did not resume operation");
    dev->flags |= BLOCK_FLAG_DEAD;
    MCHECK(!storage_shutdown_sync(&volumes, &synced) && synced == 0, "shutdown sync claimed success on a dead device");
    storage_shutdown_abort();
    dev->flags &= ~BLOCK_FLAG_DEAD;
    MCHECK(ush_execute("ultrium storage sync") == USH_OK, "sync command failed");
    /* Unmount, RAMFS unaffected, remount, data persisted on the device. */
    MCHECK(ush_execute("ultrium storage unmount /volumes/disk") == USH_OK && vfs_mount_count() == 1, "unmount failed");
    MCHECK(!ufs_mounted() && !ufs2_fs_mounted() && dev->holders == 0, "unmount did not release the volume");
    MCHECK(vfs_has("/rf", "ram") && vfs_stat("/volumes/disk", &(struct vfs_info){0}) == VFS_OK, "RAMFS state damaged by unmount");
    MCHECK(vfs_stat("/volumes/disk/projects", &(struct vfs_info){0}) == VFS_NOT_FOUND, "volume content visible after unmount");
    MCHECK(ush_execute("ultrium storage mount ufst /volumes/disk -r") == USH_OK, "remount failed");
    MCHECK(vfs_has("/volumes/disk/projects/notes.txt", "Persistent"), "data lost across unmount and remount");
    MCHECK(vfs_create_file("/volumes/disk/nope") == VFS_READ_ONLY, "read-only mount accepted a create");
    MCHECK(ush_execute("ultrium storage volumes") == USH_OK && ush_execute("ultrium storage info /volumes/disk") == USH_OK &&
           ush_execute("ultrium storage info /") == USH_OK, "volumes/info failed");
    MCHECK(ush_execute("ultrium storage unmount /volumes/disk") == USH_OK, "second unmount failed");
    return mount_cleanup(0);
}

const struct test_case ultriumfs_cases[] = {
    {"ultriumfs format and mount", ufs_format_mount},
    {"ultriumfs format safety", ufs_format_safety},
    {"ultriumfs tree and error paths", ufs_tree_and_errors},
    {"ultriumfs file I/O and growth", ufs_file_io},
    {"ultriumfs maximum file, disk full, reuse", ufs_max_file_and_full},
    {"ultriumfs metadata exhaustion", ufs_metadata_exhaustion},
    {"ultriumfs directory growth", ufs_directory_growth},
    {"ultriumfs rename, busy items, handles", ufs_rename_and_busy},
    {"ultriumfs persistence and unclean mount", ufs_persistence_and_unclean},
    {"ultriumfs read-only", ufs_read_only},
    {"ultriumfs block sizes", ufs_block_sizes},
    {"ultriumfs corruption detection", ufs_corruption},
    {"ultriumfs orphan tolerance", ufs_orphans},
    {"ultriumfs injected device failures", ufs_failure_injection},
    {"ultriumfs allocation rollback", ufs_allocation_rollback},
    {"ultriumfs growth rollback on I/O error", ufs_growth_rollback_on_io_error},
    {"ultriumfs device removal", ufs_device_removal},
    {"vfs mount routing and shell commands", vfs_mount_routing},
    {"vfs mount protection and policy", vfs_mount_protection},
    {"vfs unmount, handles, freeze, shutdown sync", vfs_mount_unmount_cycle}
};
const size_t ultriumfs_case_count = sizeof(ultriumfs_cases) / sizeof(ultriumfs_cases[0]);
