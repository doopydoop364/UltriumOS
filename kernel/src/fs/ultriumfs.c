#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ultriumfs.h"
#include "memory/vmm.h"
#include "memory.h"

/* UltriumFS v1 implementation. Write-through, no block cache: every metadata
   and data change reaches the block device immediately (durable only after a
   flush, see ufs_sync). The free-block bitmap is derived at mount time from
   the inode table, so a crash can never leak blocks. There is no journal and
   no crash consistency guarantee; see docs/ultriumfs-format.md.

   Single-CPU and non-reentrant: shared static buffers are used, each with one
   owner at a time (noted at each use). */

#define INODE_SIZE 128U
#define INODES_PER_BLOCK (UFS_BLOCK_SIZE / INODE_SIZE)
#define DIRENT_SIZE 64U
#define DIRENTS_PER_BLOCK (UFS_BLOCK_SIZE / DIRENT_SIZE)
#define NDIRECT 12U
#define PTRS_PER_BLOCK (UFS_BLOCK_SIZE / 4U)
#define MAX_FILE_BLOCKS (NDIRECT + PTRS_PER_BLOCK)
#define MIN_TOTAL_BLOCKS 16U
#define NAME_LIMIT 31U
#define ROOT_INODE 1U
#define MAX_FILE_HANDLES 16U
#define MAX_DIR_HANDLES 8U
#define INODE_FREE 0U
#define INODE_FILE 1U
#define INODE_DIR 2U
#define SB_CRC_OFFSET 124U
#define STATE_CLEAN 0U
#define STATE_DIRTY 1U
#define INODE_USED 1U
#define INODE_REFERENCED 2U

static const uint8_t magic[8] = {'U', 'L', 'T', 'R', 'I', 'U', 'M', 'F'};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

static uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}

struct inode {
    uint8_t type, flags;
    uint64_t size;
    uint32_t direct[NDIRECT], indirect, block_count;
};

struct growth {
    uint32_t blocks[MAX_FILE_BLOCKS + 1U];
    uint32_t count;
    bool indirect_dirty;
};

struct file_handle { bool active; uint32_t generation, ino, flags; uint64_t offset; };
struct dir_handle { bool active; uint32_t generation, ino, index; };

static struct {
    bool mounted, read_only, dirty_on_disk, unclean, faulted;
    struct block_device *device;
    uint32_t per;                       /* device blocks per filesystem block */
    uint64_t total;
    uint32_t inode_count, inode_start, inode_blocks, data_start;
    char label[17];
    uint8_t *used;                      /* block bitmap, derived at mount */
    uint8_t *inode_state;               /* INODE_USED / INODE_REFERENCED per inode */
    size_t used_pages, inode_pages;
    uint32_t free_blocks, free_inodes, orphans, hint_block, hint_inode;
    struct file_handle files[MAX_FILE_HANDLES];
    struct dir_handle dirs[MAX_DIR_HANDLES];
} vol;

/* Static scratch, one owner at a time. */
static uint8_t buf_inode[UFS_BLOCK_SIZE];   /* inode-table read-modify-write */
static uint8_t buf_data[UFS_BLOCK_SIZE];    /* file data block */
static uint8_t buf_dir[UFS_BLOCK_SIZE];     /* directory block */
static uint8_t buf_ind[UFS_BLOCK_SIZE];     /* indirect block */
static uint8_t buf_super[UFS_BLOCK_SIZE];   /* superblock */
static struct growth growth;

static enum vfs_error from_block(enum block_error e) {
    return e == BLOCK_OK ? VFS_OK : e == BLOCK_READ_ONLY ? VFS_READ_ONLY : VFS_IO;
}
static enum vfs_error fs_read(uint32_t block, void *buffer) {
    if (block >= vol.total) return VFS_CORRUPT;
    return from_block(block_read(vol.device, (uint64_t)block * vol.per, vol.per, buffer, UFS_BLOCK_SIZE));
}
static enum vfs_error fs_write(uint32_t block, const void *buffer) {
    if (block >= vol.total) return VFS_CORRUPT;
    return from_block(block_write(vol.device, (uint64_t)block * vol.per, vol.per, buffer, UFS_BLOCK_SIZE));
}
/* A failed metadata write is uncertain: a 4 KiB block is several device
   transfers and some may have landed. In-memory state can no longer be trusted
   to match the disk, so the volume becomes read-only (and stays marked
   unclean) until it is remounted and re-verified. */
static enum vfs_error fs_write_meta(uint32_t block, const void *buffer) {
    enum vfs_error e = fs_write(block, buffer);
    if (e != VFS_OK && e != VFS_CORRUPT) { vol.read_only = true; vol.faulted = true; }
    return e;
}
static enum vfs_error flush(void) {
    enum block_error e = block_flush(vol.device);
    return (e == BLOCK_OK || e == BLOCK_UNSUPPORTED) ? VFS_OK : from_block(e);
}

/* ---- bitmaps ---- */
static bool block_is_used(uint32_t b) { return vol.used[b >> 3] & (1U << (b & 7)); }
static void block_set(uint32_t b, bool on) {
    if (on) vol.used[b >> 3] |= (uint8_t)(1U << (b & 7)); else vol.used[b >> 3] &= (uint8_t)~(1U << (b & 7));
}
static bool alloc_block(uint32_t *out) {
    if (!vol.free_blocks) return false;
    uint32_t span = (uint32_t)vol.total - vol.data_start, start = vol.hint_block;
    if (start < vol.data_start || start >= vol.total) start = vol.data_start;
    for (uint32_t i = 0; i < span; ++i) {
        uint32_t b = vol.data_start + (start - vol.data_start + i) % span;
        if (!block_is_used(b)) {
            block_set(b, true); --vol.free_blocks; vol.hint_block = b + 1; *out = b;
            return true;
        }
    }
    return false;
}
static bool release_block(uint32_t b) {
    if (b < vol.data_start || b >= vol.total || !block_is_used(b)) return false;  /* double free / invalid */
    block_set(b, false); ++vol.free_blocks;
    return true;
}
static bool alloc_inode(uint32_t *out) {
    if (!vol.free_inodes) return false;
    for (uint32_t i = 0; i < vol.inode_count; ++i) {
        uint32_t n = 1 + (vol.hint_inode + i) % vol.inode_count;
        if (!(vol.inode_state[n - 1] & INODE_USED)) {
            vol.inode_state[n - 1] = INODE_USED; --vol.free_inodes; vol.hint_inode = n; *out = n;
            return true;
        }
    }
    return false;
}
static void release_inode(uint32_t n) {
    if (n >= 1 && n <= vol.inode_count && (vol.inode_state[n - 1] & INODE_USED)) {
        vol.inode_state[n - 1] = 0; ++vol.free_inodes;
    }
}

/* ---- inode encoding ---- */
static void inode_encode(const struct inode *in, uint8_t *p) {
    memset(p, 0, INODE_SIZE);
    p[0] = in->type; p[1] = in->flags;
    put64(p + 8, in->size);
    for (unsigned i = 0; i < NDIRECT; ++i) put32(p + 16 + i * 4, in->direct[i]);
    put32(p + 64, in->indirect); put32(p + 68, in->block_count);
    put32(p + SB_CRC_OFFSET, crc32(p, SB_CRC_OFFSET));
}
static bool all_zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return false; return true; }
/* Returns VFS_OK with type set, or VFS_CORRUPT. A free slot decodes to type 0. */
static enum vfs_error inode_decode(const uint8_t *p, struct inode *in) {
    memset(in, 0, sizeof(*in));
    if (all_zero(p, INODE_SIZE)) return VFS_OK;
    if (le32(p + SB_CRC_OFFSET) != crc32(p, SB_CRC_OFFSET)) return VFS_CORRUPT;
    if ((p[0] != INODE_FILE && p[0] != INODE_DIR) || p[1] || p[2] || p[3] || p[4] || p[5] || p[6] || p[7] ||
        !all_zero(p + 72, 52)) return VFS_CORRUPT;
    in->type = p[0]; in->flags = p[1]; in->size = le64(p + 8);
    for (unsigned i = 0; i < NDIRECT; ++i) in->direct[i] = le32(p + 16 + i * 4);
    in->indirect = le32(p + 64); in->block_count = le32(p + 68);
    return VFS_OK;
}
static uint32_t blocks_for(uint64_t size) { return (uint32_t)((size + UFS_BLOCK_SIZE - 1) / UFS_BLOCK_SIZE); }

static enum vfs_error inode_load(uint32_t ino, struct inode *in) {
    if (ino < 1 || ino > vol.inode_count) return VFS_CORRUPT;
    enum vfs_error e = fs_read(vol.inode_start + (ino - 1) / INODES_PER_BLOCK, buf_inode);
    if (e != VFS_OK) return e;
    e = inode_decode(buf_inode + ((ino - 1) % INODES_PER_BLOCK) * INODE_SIZE, in);
    if (e != VFS_OK) return e;
    return in->type == INODE_FREE ? VFS_CORRUPT : VFS_OK;
}
static enum vfs_error inode_put(uint32_t ino, const struct inode *in) {  /* in == 0 clears */
    uint32_t block = vol.inode_start + (ino - 1) / INODES_PER_BLOCK;
    enum vfs_error e = fs_read(block, buf_inode);
    if (e != VFS_OK) return e;
    uint8_t *slot = buf_inode + ((ino - 1) % INODES_PER_BLOCK) * INODE_SIZE;
    if (in) inode_encode(in, slot); else memset(slot, 0, INODE_SIZE);
    return fs_write_meta(block, buf_inode);
}

/* ---- superblock ---- */
static void super_encode(uint8_t *p, uint32_t state) {
    memset(p, 0, UFS_BLOCK_SIZE);
    memcpy(p, magic, 8);
    put32(p + 8, UFS_VERSION); put32(p + 12, UFS_BLOCK_SIZE);
    put64(p + 16, vol.total);
    put32(p + 24, vol.inode_count); put32(p + 28, vol.inode_start); put32(p + 32, vol.inode_blocks);
    put32(p + 36, vol.data_start); put32(p + 40, ROOT_INODE); put32(p + 44, state);
    put32(p + 72, INODE_SIZE); put32(p + 76, DIRENT_SIZE); put32(p + 80, 0);
    memcpy(p + 56, vol.label, 16);
    put32(p + SB_CRC_OFFSET, crc32(p, SB_CRC_OFFSET));
}
static enum vfs_error mark_state(uint32_t state) {
    super_encode(buf_super, state);
    enum vfs_error e = fs_write(0, buf_super);
    if (e == VFS_OK) e = flush();
    return e;
}
/* Called before every mutation: the on-disk state says "modified since sync". */
static enum vfs_error ensure_dirty(void) {
    if (vol.read_only) return VFS_READ_ONLY;
    if (vol.dirty_on_disk) return VFS_OK;
    enum vfs_error e = mark_state(STATE_DIRTY);
    if (e == VFS_OK) vol.dirty_on_disk = true;
    return e;
}

/* ---- file block mapping ---- */
/* Loads the indirect block into buf_ind when nblocks needs one. */
static enum vfs_error load_indirect(const struct inode *in, uint32_t nblocks) {
    if (nblocks <= NDIRECT) return VFS_OK;
    if (in->indirect < vol.data_start || in->indirect >= vol.total) return VFS_CORRUPT;
    return fs_read(in->indirect, buf_ind);
}
static uint32_t block_at(const struct inode *in, uint32_t index) {  /* buf_ind must be loaded */
    return index < NDIRECT ? in->direct[index] : le32(buf_ind + (index - NDIRECT) * 4);
}
static bool data_block_ok(uint32_t b) { return b >= vol.data_start && b < vol.total; }

static void growth_rollback(void) {
    for (uint32_t i = 0; i < growth.count; ++i) (void)release_block(growth.blocks[i]);
    growth.count = 0; growth.indirect_dirty = false;
}
/* Extends `in` from old_blocks to new_blocks, allocating data blocks and (when
   needed) the indirect block. Nothing is written to disk. On failure the
   allocations are released and `in` must be discarded by the caller. buf_ind
   holds the modified indirect block on success. */
static enum vfs_error grow_inode(struct inode *in, uint32_t old_blocks, uint32_t new_blocks) {
    growth.count = 0; growth.indirect_dirty = false;
    if (new_blocks > MAX_FILE_BLOCKS) return VFS_NO_SPACE;
    uint32_t needed = new_blocks - old_blocks;
    bool new_indirect = new_blocks > NDIRECT && in->indirect == 0;
    if (vol.free_blocks < needed + (new_indirect ? 1U : 0U)) return VFS_NO_SPACE;
    for (uint32_t i = 0; i < needed + (new_indirect ? 1U : 0U); ++i) {
        if (!alloc_block(&growth.blocks[growth.count])) { growth_rollback(); return VFS_NO_SPACE; }
        ++growth.count;
    }
    if (new_blocks > NDIRECT) {
        if (new_indirect) { memset(buf_ind, 0, UFS_BLOCK_SIZE); in->indirect = growth.blocks[growth.count - 1]; }
        else {
            enum vfs_error e = load_indirect(in, old_blocks > NDIRECT ? old_blocks : new_blocks);
            if (e != VFS_OK) { growth_rollback(); return e; }
        }
    }
    for (uint32_t i = old_blocks; i < new_blocks; ++i) {
        uint32_t b = growth.blocks[i - old_blocks];
        if (i < NDIRECT) in->direct[i] = b; else { put32(buf_ind + (i - NDIRECT) * 4, b); growth.indirect_dirty = true; }
    }
    if (new_indirect) growth.indirect_dirty = true;
    in->block_count += needed + (new_indirect ? 1U : 0U);
    return VFS_OK;
}
static enum vfs_error growth_commit_indirect(const struct inode *in) {
    return growth.indirect_dirty ? fs_write_meta(in->indirect, buf_ind) : VFS_OK;
}

/* ---- directories ---- */
static bool name_valid(const char *name, size_t length) {
    if (!length || length > NAME_LIMIT) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == '/' || c == 127) return false;
    }
    return true;
}
static bool entry_name_equal(const uint8_t *entry, const char *name, size_t length) {
    return entry[5] == length && !memcmp(entry + 8, name, length);
}

/* Visit every entry of a directory. Callback returns true to stop. The block
   being visited is in buf_dir; entries are numbered from 0. */
typedef bool (*entry_visitor)(uint8_t *entry, uint32_t block, uint32_t index, void *context);
static enum vfs_error each_entry(const struct inode *dir, entry_visitor visit, void *context, bool *stopped) {
    uint32_t nblocks = blocks_for(dir->size);
    if (dir->type != INODE_DIR || !nblocks || nblocks > MAX_FILE_BLOCKS || (dir->size % UFS_BLOCK_SIZE)) return VFS_CORRUPT;
    enum vfs_error e = load_indirect(dir, nblocks);
    if (e != VFS_OK) return e;
    for (uint32_t bi = 0; bi < nblocks; ++bi) {
        uint32_t block = block_at(dir, bi);
        if (!data_block_ok(block)) return VFS_CORRUPT;
        e = fs_read(block, buf_dir);
        if (e != VFS_OK) return e;
        for (uint32_t k = 0; k < DIRENTS_PER_BLOCK; ++k)
            if (visit(buf_dir + k * DIRENT_SIZE, block, bi * DIRENTS_PER_BLOCK + k, context)) { if (stopped) *stopped = true; return VFS_OK; }
    }
    return VFS_OK;
}

struct find_context { const char *name; size_t length; uint32_t ino; uint8_t type; };
static bool find_visit(uint8_t *entry, uint32_t block, uint32_t index, void *c) {
    (void)block; (void)index;
    struct find_context *f = c;
    uint32_t ino = le32(entry);
    if (!ino || !entry_name_equal(entry, f->name, f->length)) return false;
    f->ino = ino; f->type = entry[4];
    return true;
}
static enum vfs_error dir_find(const struct inode *dir, const char *name, size_t length, uint32_t *ino, uint8_t *type) {
    struct find_context f = {name, length, 0, 0};
    bool found = false;
    enum vfs_error e = each_entry(dir, find_visit, &f, &found);
    if (e != VFS_OK) return e;
    if (!found) return VFS_NOT_FOUND;
    if (ino) *ino = f.ino;
    if (type) *type = f.type;
    return VFS_OK;
}

struct slot_context { uint32_t block, index; bool found; };
static bool slot_visit(uint8_t *entry, uint32_t block, uint32_t index, void *c) {
    struct slot_context *s = c;
    if (le32(entry)) return false;
    s->block = block; s->index = index; s->found = true;
    return true;
}
static bool empty_visit(uint8_t *entry, uint32_t block, uint32_t index, void *c) {
    (void)block; (void)index; (void)c;
    return le32(entry) != 0;
}

/* Write one entry into a known slot (block, index in directory numbering). */
static enum vfs_error entry_write(uint32_t block, uint32_t index, uint32_t ino, uint8_t type,
                                  const char *name, size_t length) {
    enum vfs_error e = fs_read(block, buf_dir);
    if (e != VFS_OK) return e;
    uint8_t *entry = buf_dir + (index % DIRENTS_PER_BLOCK) * DIRENT_SIZE;
    memset(entry, 0, DIRENT_SIZE);
    if (ino) {
        put32(entry, ino); entry[4] = type; entry[5] = (uint8_t)length;
        memcpy(entry + 8, name, length);
    }
    return fs_write_meta(block, buf_dir);
}

/* Find a free slot in `dir`, extending the directory by one block if full.
   Extension writes the new zero block, the indirect block and the directory
   inode (`dir` is updated in memory and on disk). The slot itself is not
   written. */
static enum vfs_error dir_reserve(uint32_t dir_ino, struct inode *dir, uint32_t *block, uint32_t *index) {
    struct slot_context s = {0, 0, false};
    bool stopped = false;
    enum vfs_error e = each_entry(dir, slot_visit, &s, &stopped);
    if (e != VFS_OK) return e;
    if (s.found) { *block = s.block; *index = s.index; return VFS_OK; }
    uint32_t old_blocks = blocks_for(dir->size);
    struct inode grown = *dir;
    e = grow_inode(&grown, old_blocks, old_blocks + 1);
    if (e != VFS_OK) return e;
    memset(buf_dir, 0, UFS_BLOCK_SIZE);
    e = fs_write(growth.blocks[0], buf_dir);
    if (e == VFS_OK) e = growth_commit_indirect(&grown);
    grown.size += UFS_BLOCK_SIZE;
    if (e == VFS_OK) e = inode_put(dir_ino, &grown);
    if (e != VFS_OK) { growth_rollback(); return e; }
    *block = growth.blocks[0]; *index = old_blocks * DIRENTS_PER_BLOCK;
    growth.count = 0;
    *dir = grown;
    return VFS_OK;
}

/* ---- path resolution ---- */
static enum vfs_error component(const char **cursor, const char **name, size_t *length) {
    const char *p = *cursor;
    while (*p == '/') ++p;
    if (!*p) { *name = 0; *length = 0; *cursor = p; return VFS_OK; }
    const char *start = p;
    while (*p && *p != '/') ++p;
    *name = start; *length = (size_t)(p - start); *cursor = p;
    return name_valid(start, *length) ? VFS_OK : VFS_INVALID_PATH;
}
static enum vfs_error walk(const char *path, size_t limit, uint32_t *ino_out, struct inode *in_out) {
    /* Resolves the first `limit` characters of an absolute path. */
    if (!vol.mounted || !path || path[0] != '/') return VFS_INVALID_PATH;
    uint32_t ino = ROOT_INODE;
    struct inode in;
    enum vfs_error e = inode_load(ino, &in);
    if (e != VFS_OK) return e;
    const char *cursor = path, *end = path + limit;
    for (;;) {
        const char *name; size_t length;
        e = component(&cursor, &name, &length);
        if (e != VFS_OK) return e;
        if (!name || name >= end) break;
        if (name + length > end) return VFS_INVALID_PATH;
        if (in.type != INODE_DIR) return VFS_NOT_DIRECTORY;
        uint32_t child; uint8_t type;
        e = dir_find(&in, name, length, &child, &type);
        if (e != VFS_OK) return e;
        ino = child;
        e = inode_load(ino, &in);
        if (e != VFS_OK) return e;
        if (in.type != type) return VFS_CORRUPT;
    }
    if (ino_out) *ino_out = ino;
    if (in_out) *in_out = in;
    return VFS_OK;
}
static size_t text_length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static enum vfs_error lookup(const char *path, uint32_t *ino, struct inode *in) {
    return walk(path, text_length(path), ino, in);
}
/* Splits "/a/b/c" into the directory "/a/b" (resolved) and the leaf "c". */
static enum vfs_error split(const char *path, uint32_t *parent_ino, struct inode *parent,
                            const char **leaf, size_t *leaf_length) {
    size_t n = text_length(path);
    if (!vol.mounted || n < 2 || path[0] != '/') return VFS_INVALID_PATH;
    size_t slash = n;
    while (slash > 0 && path[slash - 1] != '/') --slash;
    size_t length = n - slash;
    if (!name_valid(path + slash, length)) return VFS_INVALID_PATH;
    enum vfs_error e = walk(path, slash > 1 ? slash - 1 : 1, parent_ino, parent);
    if (e != VFS_OK) return e;
    if (parent->type != INODE_DIR) return VFS_NOT_DIRECTORY;
    *leaf = path + slash; *leaf_length = length;
    return VFS_OK;
}

static bool inode_open(uint32_t ino) {
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) if (vol.files[i].active && vol.files[i].ino == ino) return true;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) if (vol.dirs[i].active && vol.dirs[i].ino == ino) return true;
    return false;
}
static unsigned open_handles(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) n += vol.files[i].active;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) n += vol.dirs[i].active;
    return n;
}

/* ---- create / remove ---- */
static enum vfs_error create_node(const char *path, uint8_t type) {
    if (vol.read_only) return VFS_READ_ONLY;
    uint32_t parent_ino; struct inode parent; const char *leaf; size_t leaf_length;
    enum vfs_error e = split(path, &parent_ino, &parent, &leaf, &leaf_length);
    if (e != VFS_OK) return e;
    e = dir_find(&parent, leaf, leaf_length, 0, 0);
    if (e == VFS_OK) return VFS_EXISTS;
    if (e != VFS_NOT_FOUND) return e;
    e = ensure_dirty();
    if (e != VFS_OK) return e;
    uint32_t block, index;
    e = dir_reserve(parent_ino, &parent, &block, &index);
    if (e != VFS_OK) return e;
    uint32_t ino;
    if (!alloc_inode(&ino)) return VFS_NO_SPACE;
    struct inode child = {0};
    child.type = type;
    uint32_t data_block = 0;
    if (type == INODE_DIR) {
        if (!alloc_block(&data_block)) { release_inode(ino); return VFS_NO_SPACE; }
        child.size = UFS_BLOCK_SIZE; child.direct[0] = data_block; child.block_count = 1;
        memset(buf_data, 0, UFS_BLOCK_SIZE);
        e = fs_write(data_block, buf_data);
    }
    /* Child first, then the link: a failure leaves at worst an unreferenced inode. */
    if (e == VFS_OK) e = inode_put(ino, &child);
    if (e == VFS_OK) e = entry_write(block, index, ino, type, leaf, leaf_length);
    if (e != VFS_OK) {
        if (data_block) (void)release_block(data_block);
        release_inode(ino);
        (void)inode_put(ino, 0);
        return e;
    }
    return VFS_OK;
}

/* Frees every block of an inode in the in-memory bitmap using the pointer
   list captured in `list` (count entries). */
static void release_blocks(const uint32_t *list, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) (void)release_block(list[i]);
}
/* Collects all blocks of `in` (data + indirect) into growth.blocks. */
static enum vfs_error collect_blocks(const struct inode *in) {
    uint32_t nblocks = blocks_for(in->size);
    if (nblocks > MAX_FILE_BLOCKS) return VFS_CORRUPT;
    enum vfs_error e = load_indirect(in, nblocks);
    if (e != VFS_OK) return e;
    growth.count = 0;
    for (uint32_t i = 0; i < nblocks; ++i) {
        uint32_t b = block_at(in, i);
        if (!data_block_ok(b)) { growth.count = 0; return VFS_CORRUPT; }
        growth.blocks[growth.count++] = b;
    }
    if (nblocks > NDIRECT) growth.blocks[growth.count++] = in->indirect;
    return VFS_OK;
}

static enum vfs_error ufs_unlink(const char *path) {
    if (vol.read_only) return VFS_READ_ONLY;
    uint32_t parent_ino, ino; struct inode parent, in; const char *leaf; size_t leaf_length;
    enum vfs_error e = split(path, &parent_ino, &parent, &leaf, &leaf_length);
    if (e != VFS_OK) return e;
    uint8_t type;
    e = dir_find(&parent, leaf, leaf_length, &ino, &type);
    if (e != VFS_OK) return e;
    e = inode_load(ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != type) return VFS_CORRUPT;
    if (inode_open(ino)) return VFS_BUSY;
    if (in.type == INODE_DIR) {
        bool nonempty = false;
        e = each_entry(&in, empty_visit, 0, &nonempty);
        if (e != VFS_OK) return e;
        if (nonempty) return VFS_NOT_EMPTY;
    }
    e = ensure_dirty();
    if (e != VFS_OK) return e;
    e = collect_blocks(&in);
    if (e != VFS_OK) return e;
    /* Locate the entry again to clear it (parent blocks were last in buf_dir). */
    struct find_context f = {leaf, leaf_length, 0, 0};
    uint32_t entry_block = 0, entry_index = 0; bool located = false;
    {
        uint32_t nblocks = blocks_for(parent.size);
        e = load_indirect(&parent, nblocks);
        if (e != VFS_OK) return e;
        for (uint32_t bi = 0; bi < nblocks && !located; ++bi) {
            uint32_t block = block_at(&parent, bi);
            if (!data_block_ok(block)) return VFS_CORRUPT;
            e = fs_read(block, buf_dir);
            if (e != VFS_OK) return e;
            for (uint32_t k = 0; k < DIRENTS_PER_BLOCK; ++k) {
                uint8_t *entry = buf_dir + k * DIRENT_SIZE;
                if (le32(entry) == ino && entry_name_equal(entry, f.name, f.length)) {
                    entry_block = block; entry_index = bi * DIRENTS_PER_BLOCK + k; located = true; break;
                }
            }
        }
    }
    if (!located) return VFS_CORRUPT;
    /* Entry first, then the inode. A failure leaves an unreferenced inode. */
    e = entry_write(entry_block, entry_index, 0, 0, 0, 0);
    if (e != VFS_OK) return e;
    e = inode_put(ino, 0);
    if (e != VFS_OK) return e;
    release_blocks(growth.blocks, growth.count);
    growth.count = 0;
    release_inode(ino);
    return VFS_OK;
}

static bool path_prefix(const char *prefix, const char *path) {
    size_t n = 0;
    for (; prefix[n]; ++n) if (path[n] != prefix[n]) return false;   /* stops at path's NUL */
    return n && (path[n] == '/' || path[n] == 0);
}
static enum vfs_error ufs_rename(const char *source, const char *destination) {
    if (vol.read_only) return VFS_READ_ONLY;
    if (text_length(source) < 2) return VFS_INVALID_PATH;       /* the root cannot move */
    if (path_prefix(source, destination)) return VFS_INVALID_PATH; /* into itself or a descendant */
    uint32_t sp_ino, dp_ino, ino; struct inode sp, dp, node;
    const char *sleaf, *dleaf; size_t slen, dlen;
    enum vfs_error e = split(source, &sp_ino, &sp, &sleaf, &slen);
    if (e != VFS_OK) return e;
    uint8_t type;
    e = dir_find(&sp, sleaf, slen, &ino, &type);
    if (e != VFS_OK) return e;
    if (inode_open(ino)) return VFS_BUSY;
    e = split(destination, &dp_ino, &dp, &dleaf, &dlen);
    if (e != VFS_OK) return e;
    e = dir_find(&dp, dleaf, dlen, 0, 0);
    if (e == VFS_OK) return VFS_EXISTS;
    if (e != VFS_NOT_FOUND) return e;
    e = inode_load(ino, &node);
    if (e != VFS_OK) return e;
    e = ensure_dirty();
    if (e != VFS_OK) return e;
    /* Reserve the destination slot first (may extend the directory), then
       remove the old entry, then write the new one. */
    uint32_t nb, ni;
    e = dir_reserve(dp_ino, &dp, &nb, &ni);
    if (e != VFS_OK) return e;
    if (sp_ino == dp_ino) sp = dp;
    uint32_t ob = 0, oi = 0; bool located = false;
    uint32_t nblocks = blocks_for(sp.size);
    e = load_indirect(&sp, nblocks);
    if (e != VFS_OK) return e;
    for (uint32_t bi = 0; bi < nblocks && !located; ++bi) {
        uint32_t block = block_at(&sp, bi);
        if (!data_block_ok(block)) return VFS_CORRUPT;
        e = fs_read(block, buf_dir);
        if (e != VFS_OK) return e;
        for (uint32_t k = 0; k < DIRENTS_PER_BLOCK; ++k) {
            uint8_t *entry = buf_dir + k * DIRENT_SIZE;
            if (le32(entry) == ino && entry_name_equal(entry, sleaf, slen)) { ob = block; oi = bi * DIRENTS_PER_BLOCK + k; located = true; break; }
        }
    }
    if (!located) return VFS_CORRUPT;
    e = entry_write(ob, oi, 0, 0, 0, 0);
    if (e != VFS_OK) return e;
    e = entry_write(nb, ni, ino, type, dleaf, dlen);
    if (e != VFS_OK) {
        (void)entry_write(ob, oi, ino, type, sleaf, slen);  /* best-effort restore */
        return e;
    }
    return VFS_OK;
}

/* ---- file I/O ---- */
static struct file_handle *file_get(struct vfs_file *f) {
    if (!f || f->slot >= MAX_FILE_HANDLES) return 0;
    struct file_handle *h = &vol.files[f->slot];
    return h->active && h->generation == f->generation ? h : 0;
}
static enum vfs_error ufs_file_open(const char *path, uint32_t flags, struct vfs_file *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    if (!flags || (flags & ~(VFS_OPEN_READ | VFS_OPEN_WRITE))) return VFS_BAD_HANDLE;
    if ((flags & VFS_OPEN_WRITE) && vol.read_only) return VFS_READ_ONLY;
    uint32_t ino; struct inode in;
    enum vfs_error e = lookup(path, &ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != INODE_FILE) return VFS_IS_DIRECTORY;
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) {
        struct file_handle *h = &vol.files[i];
        if (h->active || h->generation == UINT32_MAX) continue;
        if (!h->generation) h->generation = 1;
        h->active = true; h->ino = ino; h->flags = flags; h->offset = 0;
        out->slot = i; out->generation = h->generation;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
static enum vfs_error ufs_file_close(struct vfs_file *f) {
    struct file_handle *h = file_get(f);
    if (!h) return VFS_BAD_HANDLE;
    h->active = false; ++h->generation;
    f->slot = f->generation = 0;
    return VFS_OK;
}
static enum vfs_error ufs_file_read(struct vfs_file *f, void *buffer, size_t capacity, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    struct file_handle *h = file_get(f);
    if (!h || !buffer || !bytes_read || !(h->flags & VFS_OPEN_READ)) return VFS_BAD_HANDLE;
    struct inode in;
    enum vfs_error e = inode_load(h->ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != INODE_FILE || in.size > UFS_MAX_FILE_BYTES) return VFS_CORRUPT;
    if (h->offset >= in.size || !capacity) return VFS_OK;
    uint64_t remaining = in.size - h->offset;
    size_t want = capacity < remaining ? capacity : (size_t)remaining, done = 0;
    uint32_t nblocks = blocks_for(in.size);
    e = load_indirect(&in, nblocks);
    if (e != VFS_OK) return e;
    while (done < want) {
        uint64_t position = h->offset + done;
        uint32_t index = (uint32_t)(position / UFS_BLOCK_SIZE), within = (uint32_t)(position % UFS_BLOCK_SIZE);
        uint32_t block = block_at(&in, index);
        if (!data_block_ok(block)) return VFS_CORRUPT;
        e = fs_read(block, buf_data);
        if (e != VFS_OK) { h->offset += done; *bytes_read = done; return done ? VFS_OK : e; }
        size_t chunk = UFS_BLOCK_SIZE - within;
        if (chunk > want - done) chunk = want - done;
        memcpy((uint8_t *)buffer + done, buf_data + within, chunk);
        done += chunk;
    }
    h->offset += done; *bytes_read = done;
    return VFS_OK;
}
static enum vfs_error ufs_file_write(struct vfs_file *f, const void *buffer, size_t length, size_t *bytes_written) {
    if (bytes_written) *bytes_written = 0;
    struct file_handle *h = file_get(f);
    if (!h || !bytes_written || (!buffer && length) || !(h->flags & VFS_OPEN_WRITE)) return VFS_BAD_HANDLE;
    if (vol.read_only) return VFS_READ_ONLY;
    if (!length) return VFS_OK;
    struct inode in;
    enum vfs_error e = inode_load(h->ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != INODE_FILE || in.size > UFS_MAX_FILE_BYTES) return VFS_CORRUPT;
    if (h->offset > in.size || length > UFS_MAX_FILE_BYTES || h->offset > UFS_MAX_FILE_BYTES - length) return VFS_NO_SPACE;
    uint64_t end = h->offset + length;
    uint32_t old_blocks = blocks_for(in.size), new_blocks = blocks_for(end > in.size ? end : in.size);
    e = ensure_dirty();
    if (e != VFS_OK) return e;
    struct inode updated = in;
    bool grown = new_blocks > old_blocks;
    if (grown) {
        e = grow_inode(&updated, old_blocks, new_blocks);
        if (e != VFS_OK) return e;
    } else {
        e = load_indirect(&updated, new_blocks);
        if (e != VFS_OK) return e;
    }
    uint32_t first = (uint32_t)(h->offset / UFS_BLOCK_SIZE), last = (uint32_t)((end - 1) / UFS_BLOCK_SIZE);
    size_t done = 0;
    for (uint32_t index = first; index <= last && e == VFS_OK; ++index) {
        uint32_t block = block_at(&updated, index);
        if (!data_block_ok(block)) { e = VFS_CORRUPT; break; }
        uint64_t block_start = (uint64_t)index * UFS_BLOCK_SIZE;
        uint32_t within = h->offset + done > block_start ? (uint32_t)(h->offset + done - block_start) : 0;
        size_t chunk = UFS_BLOCK_SIZE - within;
        if (chunk > length - done) chunk = length - done;
        if (index >= old_blocks) memset(buf_data, 0, UFS_BLOCK_SIZE);   /* new block: zero-fill */
        else if (chunk != UFS_BLOCK_SIZE) e = fs_read(block, buf_data); /* partial overwrite */
        if (e != VFS_OK) break;
        memcpy(buf_data + within, (const uint8_t *)buffer + done, chunk);
        e = fs_write(block, buf_data);
        done += chunk;
    }
    if (e == VFS_OK && grown) e = growth_commit_indirect(&updated);
    if (e == VFS_OK && (end > in.size || grown)) {
        updated.size = end > in.size ? end : in.size;
        e = inode_put(h->ino, &updated);
    }
    if (e != VFS_OK) {
        if (grown) growth_rollback();   /* inode on disk is unchanged; overwritten bytes may be partial */
        return e;
    }
    growth.count = 0;
    h->offset = end; *bytes_written = length;
    return VFS_OK;
}

/* ---- directory handles ---- */
static struct dir_handle *dir_get(struct vfs_dir *d) {
    if (!d || d->slot >= MAX_DIR_HANDLES) return 0;
    struct dir_handle *h = &vol.dirs[d->slot];
    return h->active && h->generation == d->generation ? h : 0;
}
static enum vfs_error ufs_dir_open(const char *path, struct vfs_dir *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    uint32_t ino; struct inode in;
    enum vfs_error e = lookup(path, &ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != INODE_DIR) return VFS_NOT_DIRECTORY;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) {
        struct dir_handle *h = &vol.dirs[i];
        if (h->active || h->generation == UINT32_MAX) continue;
        if (!h->generation) h->generation = 1;
        h->active = true; h->ino = ino; h->index = 0;
        out->slot = i; out->generation = h->generation;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
static enum vfs_error ufs_dir_next(struct vfs_dir *d, char *name, size_t capacity, enum vfs_node_type *type) {
    struct dir_handle *h = dir_get(d);
    if (!h || !name || !capacity || !type) return VFS_BAD_HANDLE;
    struct inode in;
    enum vfs_error e = inode_load(h->ino, &in);
    if (e != VFS_OK) return e;
    uint32_t nblocks = blocks_for(in.size);
    if (in.type != INODE_DIR || !nblocks || nblocks > MAX_FILE_BLOCKS) return VFS_CORRUPT;
    e = load_indirect(&in, nblocks);
    if (e != VFS_OK) return e;
    for (uint32_t index = h->index; index < nblocks * DIRENTS_PER_BLOCK; ++index) {
        uint32_t bi = index / DIRENTS_PER_BLOCK;
        uint32_t block = block_at(&in, bi);
        if (!data_block_ok(block)) return VFS_CORRUPT;
        e = fs_read(block, buf_dir);
        if (e != VFS_OK) return e;
        const uint8_t *entry = buf_dir + (index % DIRENTS_PER_BLOCK) * DIRENT_SIZE;
        if (!le32(entry)) continue;
        size_t length = entry[5];
        if (capacity < length + 1) return VFS_BUFFER_TOO_SMALL;
        memcpy(name, entry + 8, length); name[length] = 0;
        *type = entry[4] == INODE_DIR ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
        h->index = index + 1;
        return VFS_OK;
    }
    h->index = nblocks * DIRENTS_PER_BLOCK;
    return VFS_END;
}
static enum vfs_error ufs_dir_close(struct vfs_dir *d) {
    struct dir_handle *h = dir_get(d);
    if (!h) return VFS_BAD_HANDLE;
    h->active = false; ++h->generation;
    d->slot = d->generation = 0;
    return VFS_OK;
}

static enum vfs_error ufs_stat(const char *path, struct vfs_info *out) {
    if (!out) return VFS_BAD_HANDLE;
    struct inode in;
    enum vfs_error e = lookup(path, 0, &in);
    if (e != VFS_OK) return e;
    out->type = in.type == INODE_DIR ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->size = in.type == INODE_DIR ? 0 : (size_t)in.size;
    return VFS_OK;
}
static enum vfs_error ufs_create_file(const char *path) { return create_node(path, INODE_FILE); }
static enum vfs_error ufs_create_directory(const char *path) { return create_node(path, INODE_DIR); }
static bool ufs_is_directory(const char *path) {
    struct inode in;
    return lookup(path, 0, &in) == VFS_OK && in.type == INODE_DIR;
}

static const struct vfs_backend_ops operations = {
    .copy = 0,  /* the VFS provides a chunked generic copy */
    .rename = ufs_rename, .stat = ufs_stat,
    .create_file = ufs_create_file, .create_directory = ufs_create_directory,
    .unlink = ufs_unlink, .is_directory = ufs_is_directory,
    .file_open = ufs_file_open, .file_close = ufs_file_close,
    .file_read = ufs_file_read, .file_write = ufs_file_write,
    .dir_open = ufs_dir_open, .dir_next = ufs_dir_next, .dir_close = ufs_dir_close
};
const struct vfs_backend_ops *ufs_backend(void) { return &operations; }

/* ---- format ---- */
static bool power_of_two_divides_block(uint32_t size) { return size >= 512 && size <= UFS_BLOCK_SIZE && !(UFS_BLOCK_SIZE % size); }

enum vfs_error ufs_format(struct block_device *device, const char *label) {
    if (!device || !device->registered) return VFS_IO;
    if (vol.mounted && (vol.device == device || vol.device->parent == device || device->parent == vol.device)) return VFS_BUSY;
    if (device->holders) return VFS_BUSY;
    if (!(device->flags & BLOCK_FLAG_SCRATCH)) return VFS_DENIED;
    if (device->flags & BLOCK_FLAG_READ_ONLY) return VFS_READ_ONLY;
    if (!power_of_two_divides_block(device->block_size)) return VFS_UNSUPPORTED;
    uint64_t blocks = device->block_count / (UFS_BLOCK_SIZE / device->block_size);
    if (blocks < MIN_TOTAL_BLOCKS) return VFS_NO_SPACE;
    if (blocks > UFS_MAX_TOTAL_BLOCKS) blocks = UFS_MAX_TOTAL_BLOCKS;
    if (!block_claim(device)) return VFS_BUSY;
    /* Use the volume scratch state for the duration of the format. */
    struct block_device *saved_device = vol.device; uint32_t saved_per = vol.per; uint64_t saved_total = vol.total;
    uint32_t saved_ic = vol.inode_count, saved_is = vol.inode_start, saved_ib = vol.inode_blocks, saved_ds = vol.data_start;
    char saved_label[17]; memcpy(saved_label, vol.label, 17);
    vol.device = device; vol.per = UFS_BLOCK_SIZE / device->block_size; vol.total = blocks;
    uint32_t inodes = (uint32_t)(blocks / 4);
    if (inodes < 32) inodes = 32;
    if (inodes > UFS_MAX_INODES) inodes = UFS_MAX_INODES;
    inodes = (inodes + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK * INODES_PER_BLOCK;
    vol.inode_count = inodes; vol.inode_start = 1; vol.inode_blocks = inodes / INODES_PER_BLOCK;
    vol.data_start = 1 + vol.inode_blocks;
    memset(vol.label, 0, 17);
    for (unsigned i = 0; label && label[i] && i < 16; ++i) vol.label[i] = label[i];
    if (!label || !label[0]) memcpy(vol.label, "ULTRIUM", 7);
    enum vfs_error e = VFS_OK;
    if (vol.data_start + 2 > vol.total) e = VFS_NO_SPACE;
    /* Invalidate any previous superblock first so an interrupted format can
       never leave an old filesystem half-overwritten but still mountable. */
    memset(buf_super, 0, UFS_BLOCK_SIZE);
    if (e == VFS_OK) e = fs_write(0, buf_super);
    memset(buf_inode, 0, UFS_BLOCK_SIZE);
    for (uint32_t b = 0; e == VFS_OK && b < vol.inode_blocks; ++b) e = fs_write(vol.inode_start + b, buf_inode);
    struct inode root = {0};
    root.type = INODE_DIR; root.size = UFS_BLOCK_SIZE; root.direct[0] = vol.data_start; root.block_count = 1;
    if (e == VFS_OK) {
        memset(buf_data, 0, UFS_BLOCK_SIZE);
        e = fs_write(vol.data_start, buf_data);
    }
    if (e == VFS_OK) {
        memset(buf_inode, 0, UFS_BLOCK_SIZE);
        inode_encode(&root, buf_inode);   /* inode 1 is slot 0 of the first table block */
        e = fs_write(vol.inode_start, buf_inode);
    }
    if (e == VFS_OK) e = flush();
    if (e == VFS_OK) {
        super_encode(buf_super, STATE_CLEAN);
        e = fs_write(0, buf_super);
        if (e == VFS_OK) e = flush();
    }
    vol.device = saved_device; vol.per = saved_per; vol.total = saved_total;
    vol.inode_count = saved_ic; vol.inode_start = saved_is; vol.inode_blocks = saved_ib; vol.data_start = saved_ds;
    memcpy(vol.label, saved_label, 17);
    block_release(device);
    return e;
}

/* ---- mount ---- */
static void free_arrays(void) {
    if (vol.used) vmm_free_pages(vol.used);
    if (vol.inode_state) vmm_free_pages(vol.inode_state);
    vol.used = vol.inode_state = 0;
}
static bool claim_block_bit(uint32_t b) {   /* returns false on double allocation */
    if (!data_block_ok(b) || block_is_used(b)) return false;
    block_set(b, true);
    return true;
}

struct verify_context { uint32_t dir_ino; enum vfs_error error; };
static bool verify_visit(uint8_t *entry, uint32_t block, uint32_t index, void *c) {
    (void)block; (void)index;
    struct verify_context *v = c;
    uint32_t ino = le32(entry);
    if (!ino) {
        if (!all_zero(entry, DIRENT_SIZE)) { v->error = VFS_CORRUPT; return true; }
        return false;
    }
    uint8_t type = entry[4], length = entry[5];
    if (ino <= ROOT_INODE || ino > vol.inode_count || !(vol.inode_state[ino - 1] & INODE_USED) ||
        (vol.inode_state[ino - 1] & INODE_REFERENCED) || !name_valid((const char *)entry + 8, length) ||
        entry[6] || entry[7] || !all_zero(entry + 8 + length, DIRENT_SIZE - 8 - length)) { v->error = VFS_CORRUPT; return true; }
    /* Entry type must match the inode: re-read the inode table block (buf_inode is free here). */
    struct inode child;
    if (inode_load(ino, &child) != VFS_OK || (child.type != type) || (type != INODE_FILE && type != INODE_DIR)) {
        v->error = VFS_CORRUPT; return true;
    }
    vol.inode_state[ino - 1] |= INODE_REFERENCED;
    return false;
}

static enum vfs_error verify_volume(void) {
    /* Pass 1: inodes and block ownership. */
    for (uint32_t b = 0; b < vol.data_start; ++b) block_set(b, true);
    vol.free_blocks = (uint32_t)vol.total - vol.data_start;
    uint32_t used_blocks = 0;
    for (uint32_t t = 0; t < vol.inode_blocks; ++t) {
        enum vfs_error e = fs_read(vol.inode_start + t, buf_inode);
        if (e != VFS_OK) return e;
        static uint8_t table[UFS_BLOCK_SIZE];   /* local copy: buf_inode is reused below */
        memcpy(table, buf_inode, UFS_BLOCK_SIZE);
        for (uint32_t k = 0; k < INODES_PER_BLOCK; ++k) {
            uint32_t ino = t * INODES_PER_BLOCK + k + 1;
            struct inode in;
            if (inode_decode(table + k * INODE_SIZE, &in) != VFS_OK) return VFS_CORRUPT;
            if (in.type == INODE_FREE) continue;
            vol.inode_state[ino - 1] = INODE_USED; --vol.free_inodes;
            uint32_t nblocks = blocks_for(in.size);
            if (in.size > UFS_MAX_FILE_BYTES || nblocks > MAX_FILE_BLOCKS) return VFS_CORRUPT;
            if (in.type == INODE_DIR && (!nblocks || in.size % UFS_BLOCK_SIZE)) return VFS_CORRUPT;
            if (in.block_count != nblocks + (nblocks > NDIRECT ? 1U : 0U)) return VFS_CORRUPT;
            for (uint32_t i = 0; i < NDIRECT; ++i) {
                if (i < nblocks) { if (!claim_block_bit(in.direct[i])) return VFS_CORRUPT; ++used_blocks; }
                else if (in.direct[i]) return VFS_CORRUPT;
            }
            if (nblocks > NDIRECT) {
                if (!claim_block_bit(in.indirect)) return VFS_CORRUPT;
                ++used_blocks;
                e = fs_read(in.indirect, buf_ind);
                if (e != VFS_OK) return e;
                for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                    uint32_t b = le32(buf_ind + i * 4);
                    if (NDIRECT + i < nblocks) { if (!claim_block_bit(b)) return VFS_CORRUPT; ++used_blocks; }
                    else if (b) return VFS_CORRUPT;
                }
            } else if (in.indirect) return VFS_CORRUPT;
        }
    }
    vol.free_blocks -= used_blocks;
    if (!(vol.inode_state[ROOT_INODE - 1] & INODE_USED)) return VFS_CORRUPT;
    struct inode root;
    enum vfs_error e = inode_load(ROOT_INODE, &root);
    if (e != VFS_OK || root.type != INODE_DIR) return VFS_CORRUPT;
    /* Pass 2: every directory entry must name a distinct, allocated, correctly typed inode. */
    for (uint32_t ino = 1; ino <= vol.inode_count; ++ino) {
        if (!(vol.inode_state[ino - 1] & INODE_USED)) continue;
        struct inode dir;
        e = inode_load(ino, &dir);
        if (e != VFS_OK) return e;
        if (dir.type != INODE_DIR) continue;
        struct verify_context v = {ino, VFS_OK};
        bool stopped = false;
        e = each_entry(&dir, verify_visit, &v, &stopped);
        if (e != VFS_OK) return e;
        if (v.error != VFS_OK) return v.error;
    }
    /* Unreferenced non-root inodes are orphans (a crash can leave them); they
       are counted, keep their blocks, and are reported. */
    vol.orphans = 0;
    for (uint32_t ino = 2; ino <= vol.inode_count; ++ino)
        if ((vol.inode_state[ino - 1] & INODE_USED) && !(vol.inode_state[ino - 1] & INODE_REFERENCED)) ++vol.orphans;
    return VFS_OK;
}

enum vfs_error ufs_mount(struct block_device *device, bool read_only) {
    if (vol.mounted) return VFS_BUSY;
    if (!device || !device->registered) return VFS_IO;
    if (!power_of_two_divides_block(device->block_size)) return VFS_UNSUPPORTED;
    if (device->flags & BLOCK_FLAG_READ_ONLY) read_only = true;
    if (!block_claim(device)) return VFS_BUSY;
    memset(&vol, 0, sizeof(vol));
    vol.device = device; vol.per = UFS_BLOCK_SIZE / device->block_size; vol.read_only = read_only;
    enum vfs_error e = from_block(block_read(device, 0, vol.per, buf_super, UFS_BLOCK_SIZE));
    if (e == VFS_OK) {
        if (memcmp(buf_super, magic, 8)) e = VFS_CORRUPT;
        else if (le32(buf_super + SB_CRC_OFFSET) != crc32(buf_super, SB_CRC_OFFSET)) e = VFS_CORRUPT;
        else if (le32(buf_super + 8) != UFS_VERSION || le32(buf_super + 80) != 0) e = VFS_UNSUPPORTED;
        else if (le32(buf_super + 12) != UFS_BLOCK_SIZE || le32(buf_super + 72) != INODE_SIZE ||
                 le32(buf_super + 76) != DIRENT_SIZE || le32(buf_super + 40) != ROOT_INODE) e = VFS_CORRUPT;
    }
    if (e == VFS_OK) {
        vol.total = le64(buf_super + 16);
        vol.inode_count = le32(buf_super + 24); vol.inode_start = le32(buf_super + 28);
        vol.inode_blocks = le32(buf_super + 32); vol.data_start = le32(buf_super + 36);
        uint32_t state = le32(buf_super + 44);
        memcpy(vol.label, buf_super + 56, 16); vol.label[16] = 0;
        for (unsigned i = 0; i < 16; ++i) if (vol.label[i] && (vol.label[i] < 32 || vol.label[i] > 126)) e = VFS_CORRUPT;
        if (vol.total < MIN_TOTAL_BLOCKS || vol.total > UFS_MAX_TOTAL_BLOCKS ||
            vol.total > device->block_count / vol.per ||
            !vol.inode_count || vol.inode_count > UFS_MAX_INODES || vol.inode_count % INODES_PER_BLOCK ||
            vol.inode_start != 1 || vol.inode_blocks != vol.inode_count / INODES_PER_BLOCK ||
            vol.data_start != 1 + vol.inode_blocks || vol.data_start + 2 > vol.total || state > STATE_DIRTY) e = VFS_CORRUPT;
        vol.unclean = state == STATE_DIRTY; vol.dirty_on_disk = state == STATE_DIRTY;
    }
    if (e == VFS_OK) {
        vol.used_pages = (size_t)(((vol.total + 7) / 8 + 4095) / 4096);
        vol.inode_pages = (size_t)((vol.inode_count + 4095) / 4096);
        vol.used = vmm_alloc_pages(vol.used_pages);
        vol.inode_state = vmm_alloc_pages(vol.inode_pages);
        if (!vol.used || !vol.inode_state) e = VFS_NO_SPACE;
    }
    if (e == VFS_OK) {
        vol.free_inodes = vol.inode_count; vol.hint_block = vol.data_start;
        vol.mounted = true;   /* walk() and each_entry() require it during verification */
        e = verify_volume();
    }
    if (e != VFS_OK) {
        vol.mounted = false;
        free_arrays();
        block_release(device);
        memset(&vol, 0, sizeof(vol));
        return e;
    }
    return VFS_OK;
}

enum vfs_error ufs_sync(void) {
    if (!vol.mounted) return VFS_OK;
    if (vol.faulted) { (void)flush(); return VFS_IO; }   /* never mark a faulted volume clean */
    if (vol.read_only) return VFS_OK;
    enum vfs_error e = flush();
    if (e != VFS_OK) return e;
    if (vol.dirty_on_disk) {
        e = mark_state(STATE_CLEAN);
        if (e != VFS_OK) return e;
        vol.dirty_on_disk = false;
    }
    return VFS_OK;
}

enum vfs_error ufs_unmount(void) {
    if (!vol.mounted) return VFS_NOT_FOUND;
    if (open_handles()) return VFS_BUSY;
    enum vfs_error e = ufs_sync();
    /* A failed sync keeps the volume mounted so pending changes are never
       silently discarded. A faulted volume is the exception: it cannot be made
       consistent, so it may be released (left marked unclean) to let the
       system shut down; the error is still reported. */
    if (e != VFS_OK && !vol.faulted) return e;
    free_arrays();
    block_release(vol.device);
    memset(&vol, 0, sizeof(vol));
    return e;
}
void ufs_test_abandon(void) {
    if (!vol.mounted) return;
    free_arrays();
    block_release(vol.device);
    memset(&vol, 0, sizeof(vol));
}
bool ufs_mounted(void) { return vol.mounted; }
bool ufs_get_info(struct ufs_info *info) {
    if (!info || !vol.mounted) return false;
    memcpy(info->label, vol.label, 17);
    info->total_blocks = vol.total - vol.data_start; info->free_blocks = vol.free_blocks;
    info->inode_count = vol.inode_count; info->free_inodes = vol.free_inodes;
    info->orphan_inodes = vol.orphans; info->read_only = vol.read_only;
    info->unclean_mount = vol.unclean; info->faulted = vol.faulted; info->device = vol.device;
    return true;
}
