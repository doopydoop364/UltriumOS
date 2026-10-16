#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ufs2_fs.h"
#include "ufs2_meta.h"
#include "memory/vmm.h"
#include "memory.h"

/* UltriumFS 2 filesystem operations. Every metadata change is one journal
   transaction (docs/ultriumfs-v2-format.md section 8); new file data is written
   and flushed before the transaction that references it (section 9). The
   in-memory bitmap and inode state change immediately and are rolled back from
   an undo list if the transaction is abandoned. */

#define MAX_FILE_HANDLES 16U
#define MAX_DIR_HANDLES 8U
#define UNDO_MAX 3200U
#define CHUNK_BLOCKS 64U
#define CHUNK_EXTENTS 3U

struct file_handle { bool active; uint32_t generation, ino, inode_generation, flags; uint64_t offset; };
struct dir_handle { bool active; uint32_t generation, ino, inode_generation, index; };
struct undo_range { uint32_t start, length; bool set; };

static struct {
    bool mounted;
    struct ufs2_core core;
    uint8_t *bm, *istate;                    /* flat block bitmap (bit b = block b); inode used flags */
    size_t bm_pages, istate_pages;
    uint32_t free_blocks, free_inodes, orphans, hint_block, hint_inode;
    bool repair_recommended;
    struct undo_range undo[UNDO_MAX];
    uint32_t undo_count;
    struct file_handle files[MAX_FILE_HANDLES];
    struct dir_handle dirs[MAX_DIR_HANDLES];
} v;

static uint8_t ib[UFS2_BLOCK], db[UFS2_BLOCK], lb[UFS2_BLOCK], xb[UFS2_BLOCK];

#define CORE (&v.core)
#define GEO (v.core.sb.geo)

static bool bm_test(uint32_t b) { return v.bm[b >> 3] & (1U << (b & 7)); }

/* ---- transactions with in-memory rollback ---- */
static void undo_revert(void) {
    while (v.undo_count) {
        struct undo_range *u = &v.undo[--v.undo_count];
        for (uint32_t b = u->start; b < u->start + u->length; ++b) {
            if (u->set) v.bm[b >> 3] &= (uint8_t)~(1U << (b & 7)); else v.bm[b >> 3] |= (uint8_t)(1U << (b & 7));
        }
        if (u->set) v.free_blocks += u->length; else v.free_blocks -= u->length;
    }
}
static enum vfs_error fs_begin(void) {
    if (!v.mounted) return VFS_BAD_HANDLE;
    if (v.core.read_only) return VFS_READ_ONLY;
    if (v.core.sb.state == UFS2_STATE_CLEAN) {          /* first change after a sync: mark the volume mounted again */
        enum vfs_error e = ufs2_core_mark_mounted(CORE);
        if (e != VFS_OK) return e;
    }
    enum vfs_error e = ufs2_txn_begin(CORE);
    if (e == VFS_OK) v.undo_count = 0;
    return e;
}
static void fs_abort(void) { ufs2_txn_abort(CORE); undo_revert(); }
static enum vfs_error fs_commit(void) {
    /* Seal every staged bitmap block (checksum last, after all bit changes). */
    for (uint32_t i = 0; i < v.core.stage_count; ++i) {
        uint32_t b = v.core.stage_block[i];
        if (b >= GEO.bitmap_start && b < GEO.bitmap_start + GEO.bitmap_blocks)
            ufs2_bitmap_seal(v.core.sb.uuid, b, v.core.stage + (size_t)i * UFS2_BLOCK);
    }
    enum vfs_error e = ufs2_txn_commit(CORE);
    if (e != VFS_OK) { undo_revert(); return e; }
    v.undo_count = 0;
    return VFS_OK;
}

/* ---- bitmap ---- */
/* Flip [start, start+length) to `set`, in memory and in the staged bitmap blocks. */
static enum vfs_error bits_apply(uint32_t start, uint32_t length, bool set) {
    if (!length || (uint64_t)start + length > GEO.total) return VFS_CORRUPT;
    if (v.undo_count >= UNDO_MAX) return VFS_NO_SPACE;
    uint32_t k0 = start / UFS2_BITMAP_BITS, k1 = (start + length - 1) / UFS2_BITMAP_BITS;
    for (uint32_t k = k0; k <= k1; ++k) {      /* stage first: nothing changes if the transaction is too large */
        uint8_t *image;
        enum vfs_error e = ufs2_txn_block(CORE, GEO.bitmap_start + k, &image);
        if (e != VFS_OK) return e;
    }
    for (uint32_t b = start; b < start + length; ++b)
        if (bm_test(b) == set) return VFS_CORRUPT;      /* double allocation or double free */
    for (uint32_t k = k0; k <= k1; ++k) {
        uint8_t *image;
        (void)ufs2_txn_block(CORE, GEO.bitmap_start + k, &image);
        uint32_t lo = start > k * UFS2_BITMAP_BITS ? start : k * UFS2_BITMAP_BITS;
        uint32_t hi_end = (k + 1) * UFS2_BITMAP_BITS, hi = start + length < hi_end ? start + length : hi_end;
        for (uint32_t b = lo; b < hi; ++b) {
            uint32_t off = b - k * UFS2_BITMAP_BITS;
            if (set) { image[off >> 3] |= (uint8_t)(1U << (off & 7)); v.bm[b >> 3] |= (uint8_t)(1U << (b & 7)); }
            else { image[off >> 3] &= (uint8_t)~(1U << (off & 7)); v.bm[b >> 3] &= (uint8_t)~(1U << (b & 7)); }
        }
    }
    if (set) v.free_blocks -= length; else v.free_blocks += length;
    v.undo[v.undo_count++] = (struct undo_range){start, length, set};
    return VFS_OK;
}
/* First free run at or after `hint` (wrapping once), up to `want` blocks. */
static bool find_run(uint32_t want, uint32_t hint, uint32_t *start, uint32_t *length) {
    uint32_t lo = GEO.data_start, hi = GEO.total - 1;      /* data area [lo, hi) */
    if (hint < lo || hint >= hi) hint = lo;
    uint32_t span = hi - lo;
    for (uint32_t i = 0; i < span;) {
        uint32_t b = lo + (hint - lo + i) % span;
        if ((b & 7) == 0 && i + 8 <= span && b + 8 <= hi && v.bm[b >> 3] == 0xff) { i += 8; continue; }
        if (bm_test(b)) { ++i; continue; }
        uint32_t n = 1;
        while (n < want && b + n < hi && !bm_test(b + n)) ++n;
        *start = b; *length = n;
        return true;
    }
    return false;
}
/* Allocate up to `count` blocks in at most CHUNK_EXTENTS runs. Returns blocks obtained. */
static enum vfs_error alloc_blocks(uint32_t count, uint32_t hint, struct ufs2_extent *runs, uint32_t *run_count, uint32_t *obtained) {
    *run_count = 0; *obtained = 0;
    while (*obtained < count && *run_count < CHUNK_EXTENTS) {
        uint32_t s, n;
        if (!find_run(count - *obtained, hint, &s, &n)) break;
        enum vfs_error e = bits_apply(s, n, true);
        if (e != VFS_OK) return e;
        runs[(*run_count)++] = (struct ufs2_extent){0, s, n, 0};
        *obtained += n; hint = s + n;
    }
    return VFS_OK;
}

/* ---- inodes ---- */
static enum vfs_error ino_load(uint32_t ino, struct ufs2_inode *in) {
    if (ino < 1 || ino > GEO.inode_count) return VFS_CORRUPT;
    enum vfs_error e = ufs2_meta_read(CORE, GEO.inode_start + (ino - 1) / UFS2_INODES_PER_BLOCK, ib);
    if (e != VFS_OK) return e;
    return ufs2_inode_decode(v.core.sb.uuid, ino, ib + ((ino - 1) % UFS2_INODES_PER_BLOCK) * UFS2_INODE_SIZE, in);
}
static enum vfs_error ino_store(uint32_t ino, struct ufs2_inode *in) {
    uint8_t *image;
    enum vfs_error e = ufs2_txn_block(CORE, GEO.inode_start + (ino - 1) / UFS2_INODES_PER_BLOCK, &image);
    if (e != VFS_OK) return e;
    if (in->type != UFS2_FREE) in->txn_seq = v.core.next_seq;   /* a freed inode keeps only its generation */
    ufs2_inode_encode(v.core.sb.uuid, ino, in, image + ((ino - 1) % UFS2_INODES_PER_BLOCK) * UFS2_INODE_SIZE);
    return VFS_OK;
}
static uint32_t blocks_of(const struct ufs2_inode *in) { return (uint32_t)(in->type == UFS2_DIR ? in->size / UFS2_BLOCK : (in->size + UFS2_BLOCK - 1) / UFS2_BLOCK); }

/* ---- extent map ---- */
/* Physical block of file block `logical`, and how many blocks follow contiguously. */
static enum vfs_error map_lookup(const struct ufs2_inode *in, uint32_t ino, uint32_t logical, uint32_t *physical, uint32_t *run) {
    static struct ufs2_extent ext[UFS2_LEAF_EXTENTS];
    const struct ufs2_extent *list = in->slot; uint32_t n = in->root_entries;
    if (in->depth == 1) {
        uint32_t i = 0;
        while (i + 1 < in->root_entries && in->slot[i + 1].logical <= logical) ++i;
        if (!in->root_entries || in->slot[i].logical > logical) return VFS_NOT_FOUND;
        uint32_t leaf = in->slot[i].physical, first, entries;
        enum vfs_error e = ufs2_meta_read(CORE, leaf, lb);
        if (e != VFS_OK) return e;
        if (ufs2_leaf_decode(v.core.sb.uuid, leaf, lb, ino, in->generation, &first, &entries, ext) != VFS_OK) return VFS_CORRUPT;
        list = ext; n = entries;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (logical >= list[i].logical && logical - list[i].logical < list[i].length) {
            *physical = list[i].physical + (logical - list[i].logical);
            *run = list[i].length - (logical - list[i].logical);
            return VFS_OK;
        }
    return VFS_NOT_FOUND;
}
static void leaf_write(uint32_t leaf, uint8_t *image, uint32_t ino, uint32_t gen, uint32_t first, uint32_t n, const struct ufs2_extent *x) {
    ufs2_leaf_encode(v.core.sb.uuid, leaf, image, ino, gen, first, n, x);
}
/* Append [logical, logical+length) -> physical to the file's extent map (merging
   with the last extent when contiguous). Updates `in` and stages leaf blocks. */
static enum vfs_error map_append(struct ufs2_inode *in, uint32_t ino, uint32_t logical, uint32_t physical, uint32_t length) {
    static struct ufs2_extent ext[UFS2_LEAF_EXTENTS];
    uint32_t first, entries;
    if (in->depth == 0) {
        if (in->root_entries) {
            struct ufs2_extent *last = &in->slot[in->root_entries - 1];
            if (last->logical + last->length == logical && last->physical + last->length == physical && last->length + length <= UFS2_MAX_EXTENT_LEN) { last->length += length; return VFS_OK; }
        }
        if (in->extent_count >= UFS2_MAX_EXTENTS) return VFS_NO_SPACE;
        if (in->root_entries < UFS2_ROOT_SLOTS) { in->slot[in->root_entries++] = (struct ufs2_extent){logical, physical, length, 0}; ++in->extent_count; return VFS_OK; }
        /* Promote to depth 1: one leaf holds the inline extents plus the new one. */
        struct ufs2_extent run[CHUNK_EXTENTS]; uint32_t runs, got;
        enum vfs_error e = alloc_blocks(1, physical + length, run, &runs, &got);
        if (e != VFS_OK) return e;
        if (got != 1) return VFS_NO_SPACE;
        uint32_t leaf = run[0].physical;
        for (uint32_t i = 0; i < UFS2_ROOT_SLOTS; ++i) ext[i] = in->slot[i];
        ext[UFS2_ROOT_SLOTS] = (struct ufs2_extent){logical, physical, length, 0};
        uint8_t *image;
        e = ufs2_txn_block(CORE, leaf, &image);
        if (e != VFS_OK) return e;
        leaf_write(leaf, image, ino, in->generation, ext[0].logical, UFS2_ROOT_SLOTS + 1, ext);
        memset(in->slot, 0, sizeof(in->slot));
        in->slot[0] = (struct ufs2_extent){ext[0].logical, leaf, UFS2_ROOT_SLOTS + 1, 0};
        in->depth = 1; in->root_entries = 1; ++in->extent_count; ++in->block_count;
        return VFS_OK;
    }
    uint32_t idx = in->root_entries - 1, leaf = in->slot[idx].physical;
    uint8_t *image;
    enum vfs_error e = ufs2_txn_block(CORE, leaf, &image);
    if (e != VFS_OK) return e;
    if (ufs2_leaf_decode(v.core.sb.uuid, leaf, image, ino, in->generation, &first, &entries, ext) != VFS_OK) return VFS_CORRUPT;
    if (entries && ext[entries - 1].logical + ext[entries - 1].length == logical && ext[entries - 1].physical + ext[entries - 1].length == physical &&
        ext[entries - 1].length + length <= UFS2_MAX_EXTENT_LEN) {
        ext[entries - 1].length += length;
        leaf_write(leaf, image, ino, in->generation, first, entries, ext);
        return VFS_OK;
    }
    if (in->extent_count >= UFS2_MAX_EXTENTS) return VFS_NO_SPACE;
    if (entries < UFS2_LEAF_EXTENTS) {
        ext[entries] = (struct ufs2_extent){logical, physical, length, 0};
        leaf_write(leaf, image, ino, in->generation, first, entries + 1, ext);
        in->slot[idx].length = entries + 1; ++in->extent_count;
        return VFS_OK;
    }
    if (in->root_entries >= UFS2_ROOT_SLOTS) return VFS_NO_SPACE;                  /* extent limit */
    struct ufs2_extent run[CHUNK_EXTENTS]; uint32_t runs, got;
    e = alloc_blocks(1, physical + length, run, &runs, &got);
    if (e != VFS_OK) return e;
    if (got != 1) return VFS_NO_SPACE;
    uint32_t nleaf = run[0].physical;
    e = ufs2_txn_block(CORE, nleaf, &image);
    if (e != VFS_OK) return e;
    struct ufs2_extent only = {logical, physical, length, 0};
    leaf_write(nleaf, image, ino, in->generation, logical, 1, &only);
    in->slot[in->root_entries++] = (struct ufs2_extent){logical, nleaf, 1, 0};
    ++in->extent_count; ++in->block_count;
    return VFS_OK;
}
/* Release every block of an inode (data and leaves) in the bitmap. */
static enum vfs_error free_inode_blocks(const struct ufs2_inode *in, uint32_t ino) {
    static struct ufs2_extent ext[UFS2_LEAF_EXTENTS];
    enum vfs_error e;
    if (in->depth == 0) {
        for (uint32_t i = 0; i < in->root_entries; ++i) { e = bits_apply(in->slot[i].physical, in->slot[i].length, false); if (e != VFS_OK) return e; }
        return VFS_OK;
    }
    for (uint32_t i = 0; i < in->root_entries; ++i) {
        uint32_t leaf = in->slot[i].physical, first, entries;
        e = ufs2_meta_read(CORE, leaf, lb);
        if (e != VFS_OK) return e;
        if (ufs2_leaf_decode(v.core.sb.uuid, leaf, lb, ino, in->generation, &first, &entries, ext) != VFS_OK) return VFS_CORRUPT;
        for (uint32_t k = 0; k < entries; ++k) { e = bits_apply(ext[k].physical, ext[k].length, false); if (e != VFS_OK) return e; }
        e = bits_apply(leaf, 1, false);
        if (e != VFS_OK) return e;
    }
    return VFS_OK;
}

/* ---- directories ---- */
struct dloc { uint32_t logical, physical; unsigned index; };
static enum vfs_error dir_block(const struct ufs2_inode *dir, uint32_t dino, uint32_t logical, uint32_t *physical) {
    uint32_t run;
    enum vfs_error e = map_lookup(dir, dino, logical, physical, &run);
    if (e == VFS_NOT_FOUND) return VFS_CORRUPT;
    if (e != VFS_OK) return e;
    e = ufs2_meta_read(CORE, *physical, db);
    if (e != VFS_OK) return e;
    return ufs2_dirblock_check(v.core.sb.uuid, *physical, db, dino, dir->generation, logical) == VFS_OK ? VFS_OK : VFS_CORRUPT;
}
/* Find a name. On success fills child/gen/type and the entry location. */
static enum vfs_error dir_lookup(const struct ufs2_inode *dir, uint32_t dino, const char *name, unsigned length,
                                 uint32_t *child, uint32_t *gen, uint8_t *type, struct dloc *loc) {
    uint32_t n = blocks_of(dir);
    for (uint32_t l = 0; l < n; ++l) {
        uint32_t phys;
        enum vfs_error e = dir_block(dir, dino, l, &phys);
        if (e != VFS_OK) return e;
        for (unsigned i = 0; i < UFS2_DIRENTS; ++i) {
            if (!ufs2_dirent_inode(db, i) || ufs2_dirent_name_length(db, i) != length || memcmp(ufs2_dirent_name(db, i), name, length)) continue;
            if (child) *child = ufs2_dirent_inode(db, i);
            if (gen) *gen = ufs2_dirent_generation(db, i);
            if (type) *type = ufs2_dirent_type(db, i);
            if (loc) *loc = (struct dloc){l, phys, i};
            return VFS_OK;
        }
    }
    return VFS_NOT_FOUND;
}
static bool dir_is_empty(const struct ufs2_inode *dir, uint32_t dino, enum vfs_error *error) {
    uint32_t n = blocks_of(dir);
    for (uint32_t l = 0; l < n; ++l) {
        uint32_t phys;
        *error = dir_block(dir, dino, l, &phys);
        if (*error != VFS_OK) return false;
        for (unsigned i = 0; i < UFS2_DIRENTS; ++i) if (ufs2_dirent_inode(db, i)) return false;
    }
    *error = VFS_OK;
    return true;
}
/* Find a free slot, extending the directory by one block when full (inside the
   open transaction). `dir` is updated in memory; the caller stores it. */
static enum vfs_error dir_reserve(struct ufs2_inode *dir, uint32_t dino, struct dloc *loc, bool *extended) {
    *extended = false;
    uint32_t n = blocks_of(dir);
    for (uint32_t l = 0; l < n; ++l) {
        uint32_t phys;
        enum vfs_error e = dir_block(dir, dino, l, &phys);
        if (e != VFS_OK) return e;
        for (unsigned i = 0; i < UFS2_DIRENTS; ++i) if (!ufs2_dirent_inode(db, i)) { *loc = (struct dloc){l, phys, i}; return VFS_OK; }
    }
    uint32_t last_phys = 0, run;
    if (n && map_lookup(dir, dino, n - 1, &last_phys, &run) != VFS_OK) return VFS_CORRUPT;
    struct ufs2_extent runs[CHUNK_EXTENTS]; uint32_t nr, got;
    enum vfs_error e = alloc_blocks(1, last_phys + 1, runs, &nr, &got);
    if (e != VFS_OK) return e;
    if (got != 1) return VFS_NO_SPACE;
    uint32_t phys = runs[0].physical;
    e = map_append(dir, dino, n, phys, 1);
    if (e != VFS_OK) return e;
    uint8_t *image;
    e = ufs2_txn_block(CORE, phys, &image);
    if (e != VFS_OK) return e;
    ufs2_dirblock_init(image);
    ufs2_dirblock_seal(v.core.sb.uuid, phys, image, dino, dir->generation, n);
    dir->size += UFS2_BLOCK; ++dir->block_count;
    *loc = (struct dloc){n, phys, 0}; *extended = true;
    return VFS_OK;
}
static enum vfs_error dir_entry_set(const struct ufs2_inode *dir, uint32_t dino, const struct dloc *loc, uint32_t child, uint32_t gen,
                                    uint8_t type, const char *name, unsigned length) {
    uint8_t *image;
    enum vfs_error e = ufs2_txn_block(CORE, loc->physical, &image);
    if (e != VFS_OK) return e;
    if (child) ufs2_dirent_set(image, loc->index, child, gen, type, name, length); else ufs2_dirent_clear(image, loc->index);
    ufs2_dirblock_seal(v.core.sb.uuid, loc->physical, image, dino, dir->generation, loc->logical);
    return VFS_OK;
}

/* ---- paths ---- */
static enum vfs_error component(const char **cursor, const char **name, unsigned *length) {
    const char *p = *cursor;
    while (*p == '/') ++p;
    if (!*p) { *name = 0; *length = 0; *cursor = p; return VFS_OK; }
    const char *start = p;
    while (*p && *p != '/') ++p;
    *name = start; *length = (unsigned)(p - start); *cursor = p;
    return ufs2_name_valid(start, *length) ? VFS_OK : VFS_INVALID_PATH;
}
static size_t text_length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static enum vfs_error walk(const char *path, size_t limit, uint32_t *ino_out, struct ufs2_inode *in_out) {
    if (!v.mounted || !path || path[0] != '/') return VFS_INVALID_PATH;
    uint32_t ino = UFS2_ROOT_INODE;
    struct ufs2_inode in;
    enum vfs_error e = ino_load(ino, &in);
    if (e != VFS_OK) return e;
    const char *cursor = path, *end = path + limit;
    for (;;) {
        const char *name; unsigned length;
        e = component(&cursor, &name, &length);
        if (e != VFS_OK) return e;
        if (!name || name >= end) break;
        if (name + length > end) return VFS_INVALID_PATH;
        if (in.type != UFS2_DIR) return VFS_NOT_DIRECTORY;
        uint32_t child, gen; uint8_t type;
        e = dir_lookup(&in, ino, name, length, &child, &gen, &type, 0);
        if (e != VFS_OK) return e;
        ino = child;
        e = ino_load(ino, &in);
        if (e != VFS_OK) return e;
        if (in.type != type || in.generation != gen) return VFS_CORRUPT;
    }
    if (in.type == UFS2_FREE) return VFS_CORRUPT;
    if (ino_out) *ino_out = ino;
    if (in_out) *in_out = in;
    return VFS_OK;
}
static enum vfs_error lookup(const char *path, uint32_t *ino, struct ufs2_inode *in) { return walk(path, text_length(path), ino, in); }
static enum vfs_error split(const char *path, uint32_t *pino, struct ufs2_inode *parent, const char **leaf, unsigned *leaf_length) {
    size_t n = text_length(path);
    if (!v.mounted || n < 2 || path[0] != '/') return VFS_INVALID_PATH;
    size_t slash = n;
    while (slash > 0 && path[slash - 1] != '/') --slash;
    unsigned length = (unsigned)(n - slash);
    if (!ufs2_name_valid(path + slash, length)) return VFS_INVALID_PATH;
    enum vfs_error e = walk(path, slash > 1 ? slash - 1 : 1, pino, parent);
    if (e != VFS_OK) return e;
    if (parent->type != UFS2_DIR) return VFS_NOT_DIRECTORY;
    *leaf = path + slash; *leaf_length = length;
    return VFS_OK;
}

static bool inode_open(uint32_t ino) {
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) if (v.files[i].active && v.files[i].ino == ino) return true;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) if (v.dirs[i].active && v.dirs[i].ino == ino) return true;
    return false;
}
static unsigned open_handles(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) n += v.files[i].active;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) n += v.dirs[i].active;
    return n;
}

/* ---- create / unlink / rename (one transaction each) ---- */
static enum vfs_error alloc_inode(uint32_t *ino, uint32_t *gen) {
    if (!v.free_inodes) return VFS_NO_SPACE;
    for (uint32_t i = 0; i < GEO.inode_count; ++i) {
        uint32_t n = 1 + (v.hint_inode + i) % GEO.inode_count;
        if (v.istate[n - 1]) continue;
        struct ufs2_inode in;
        enum vfs_error e = ino_load(n, &in);
        if (e != VFS_OK) return e;
        if (in.type != UFS2_FREE) return VFS_CORRUPT;
        if (in.generation == UINT32_MAX) continue;            /* retired: its generation space is exhausted */
        *ino = n; *gen = in.generation + 1;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
static enum vfs_error create_node(const char *path, uint8_t type) {
    if (!v.mounted) return VFS_BAD_HANDLE;
    if (v.core.read_only) return VFS_READ_ONLY;
    uint32_t pino; struct ufs2_inode parent; const char *leaf; unsigned leaf_length;
    enum vfs_error e = split(path, &pino, &parent, &leaf, &leaf_length);
    if (e != VFS_OK) return e;
    e = dir_lookup(&parent, pino, leaf, leaf_length, 0, 0, 0, 0);
    if (e == VFS_OK) return VFS_EXISTS;
    if (e != VFS_NOT_FOUND) return e;
    if (!v.free_inodes || (type == UFS2_DIR && !v.free_blocks)) return VFS_NO_SPACE;
    e = fs_begin();
    if (e != VFS_OK) return e;
    uint32_t ino, gen;
    e = alloc_inode(&ino, &gen);
    struct dloc loc; bool extended;
    struct ufs2_inode child = {0};
    if (e == VFS_OK) {
        child.type = type; child.generation = gen; child.parent = pino;
        if (type == UFS2_DIR) {
            struct ufs2_extent runs[CHUNK_EXTENTS]; uint32_t nr, got;
            e = alloc_blocks(1, GEO.data_start, runs, &nr, &got);
            if (e == VFS_OK && got != 1) e = VFS_NO_SPACE;
            if (e == VFS_OK) {
                child.depth = 0; child.root_entries = 1; child.extent_count = 1; child.block_count = 1; child.size = UFS2_BLOCK;
                child.slot[0] = (struct ufs2_extent){0, runs[0].physical, 1, 0};
                uint8_t *image;
                e = ufs2_txn_block(CORE, runs[0].physical, &image);
                if (e == VFS_OK) { ufs2_dirblock_init(image); ufs2_dirblock_seal(v.core.sb.uuid, runs[0].physical, image, ino, gen, 0); }
            }
        }
    }
    if (e == VFS_OK) e = ino_store(ino, &child);
    if (e == VFS_OK) e = dir_reserve(&parent, pino, &loc, &extended);
    if (e == VFS_OK) e = dir_entry_set(&parent, pino, &loc, ino, gen, type, leaf, leaf_length);
    if (e == VFS_OK && extended) e = ino_store(pino, &parent);
    if (e != VFS_OK) { fs_abort(); return e; }
    e = fs_commit();
    if (e != VFS_OK) return e;
    v.istate[ino - 1] = 1; --v.free_inodes; v.hint_inode = ino;
    return VFS_OK;
}

static enum vfs_error ufs2_unlink(const char *path) {
    if (!v.mounted) return VFS_BAD_HANDLE;
    if (v.core.read_only) return VFS_READ_ONLY;
    uint32_t pino, ino, gen; struct ufs2_inode parent, in; const char *leaf; unsigned leaf_length; uint8_t type; struct dloc loc;
    enum vfs_error e = split(path, &pino, &parent, &leaf, &leaf_length);
    if (e != VFS_OK) return e;
    e = dir_lookup(&parent, pino, leaf, leaf_length, &ino, &gen, &type, &loc);
    if (e != VFS_OK) return e;
    e = ino_load(ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != type || in.generation != gen) return VFS_CORRUPT;
    if (inode_open(ino)) return VFS_BUSY;
    if (in.type == UFS2_DIR) {
        bool empty = dir_is_empty(&in, ino, &e);
        if (e != VFS_OK) return e;
        if (!empty) return VFS_NOT_EMPTY;
    }
    e = fs_begin();
    if (e != VFS_OK) return e;
    e = dir_entry_set(&parent, pino, &loc, 0, 0, 0, 0, 0);
    if (e == VFS_OK) e = free_inode_blocks(&in, ino);
    if (e == VFS_OK) {
        struct ufs2_inode freed = {0};
        freed.generation = in.generation;        /* a freed inode keeps its generation */
        e = ino_store(ino, &freed);
    }
    if (e != VFS_OK) { fs_abort(); return e; }
    e = fs_commit();
    if (e != VFS_OK) return e;
    v.istate[ino - 1] = 0; ++v.free_inodes;
    return VFS_OK;
}

static bool path_prefix(const char *prefix, const char *path) {
    size_t n = 0;
    for (; prefix[n]; ++n) if (path[n] != prefix[n]) return false;
    return n && (path[n] == '/' || path[n] == 0);
}
static enum vfs_error ufs2_rename(const char *source, const char *destination) {
    if (!v.mounted) return VFS_BAD_HANDLE;
    if (v.core.read_only) return VFS_READ_ONLY;
    if (text_length(source) < 2) return VFS_INVALID_PATH;
    if (path_prefix(source, destination)) return VFS_INVALID_PATH;
    uint32_t sp, dp, ino, gen; struct ufs2_inode spin, dpin, node; const char *sleaf, *dleaf; unsigned slen, dlen; uint8_t type; struct dloc sloc, dloc_;
    enum vfs_error e = split(source, &sp, &spin, &sleaf, &slen);
    if (e != VFS_OK) return e;
    e = dir_lookup(&spin, sp, sleaf, slen, &ino, &gen, &type, &sloc);
    if (e != VFS_OK) return e;
    if (inode_open(ino)) return VFS_BUSY;
    e = split(destination, &dp, &dpin, &dleaf, &dlen);
    if (e != VFS_OK) return e;
    e = dir_lookup(&dpin, dp, dleaf, dlen, 0, 0, 0, 0);
    if (e == VFS_OK) return VFS_EXISTS;
    if (e != VFS_NOT_FOUND) return e;
    e = ino_load(ino, &node);
    if (e != VFS_OK) return e;
    if (node.type != type || node.generation != gen) return VFS_CORRUPT;
    if (node.type == UFS2_DIR) {   /* never move a directory beneath itself: walk the destination's parent chain */
        uint32_t p = dp; unsigned steps = 0;
        while (p && steps++ < 64) {
            if (p == ino) return VFS_INVALID_PATH;
            struct ufs2_inode pi; e = ino_load(p, &pi);
            if (e != VFS_OK) return e;
            p = pi.parent;
        }
    }
    e = fs_begin();
    if (e != VFS_OK) return e;
    bool extended = false;
    e = dir_reserve(&dpin, dp, &dloc_, &extended);
    if (e == VFS_OK && sp == dp) spin = dpin;                      /* same directory: one in-memory copy */
    if (e == VFS_OK) e = dir_entry_set(&spin, sp, &sloc, 0, 0, 0, 0, 0);
    if (e == VFS_OK) e = dir_entry_set(&dpin, dp, &dloc_, ino, gen, type, dleaf, dlen);
    if (e == VFS_OK && extended) e = ino_store(dp, &dpin);
    if (e == VFS_OK && sp != dp) { node.parent = dp; e = ino_store(ino, &node); }
    if (e != VFS_OK) { fs_abort(); return e; }
    return fs_commit();
}

/* ---- file I/O ---- */
static struct file_handle *file_get(struct vfs_file *f) {
    if (!f || f->slot >= MAX_FILE_HANDLES) return 0;
    struct file_handle *h = &v.files[f->slot];
    return h->active && h->generation == f->generation ? h : 0;
}
static enum vfs_error ufs2_file_open(const char *path, uint32_t flags, struct vfs_file *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    if (!flags || (flags & ~(VFS_OPEN_READ | VFS_OPEN_WRITE))) return VFS_BAD_HANDLE;
    if ((flags & VFS_OPEN_WRITE) && v.core.read_only) return VFS_READ_ONLY;
    uint32_t ino; struct ufs2_inode in;
    enum vfs_error e = lookup(path, &ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != UFS2_FILE) return VFS_IS_DIRECTORY;
    for (unsigned i = 0; i < MAX_FILE_HANDLES; ++i) {
        struct file_handle *h = &v.files[i];
        if (h->active || h->generation == UINT32_MAX) continue;
        if (!h->generation) h->generation = 1;
        h->active = true; h->ino = ino; h->inode_generation = in.generation; h->flags = flags; h->offset = 0;
        out->slot = i; out->generation = h->generation;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
static enum vfs_error ufs2_file_close(struct vfs_file *f) {
    struct file_handle *h = file_get(f);
    if (!h) return VFS_BAD_HANDLE;
    h->active = false; ++h->generation;
    f->slot = f->generation = 0;
    return VFS_OK;
}
static enum vfs_error handle_inode(const struct file_handle *h, struct ufs2_inode *in) {
    enum vfs_error e = ino_load(h->ino, in);
    if (e != VFS_OK) return e;
    return in->type == UFS2_FILE && in->generation == h->inode_generation ? VFS_OK : VFS_CORRUPT;
}
static enum vfs_error ufs2_file_read(struct vfs_file *f, void *buffer, size_t capacity, size_t *bytes_read) {
    if (bytes_read) *bytes_read = 0;
    struct file_handle *h = file_get(f);
    if (!h || !buffer || !bytes_read || !(h->flags & VFS_OPEN_READ)) return VFS_BAD_HANDLE;
    struct ufs2_inode in;
    enum vfs_error e = handle_inode(h, &in);
    if (e != VFS_OK) return e;
    if (h->offset >= in.size || !capacity) return VFS_OK;
    uint64_t remaining = in.size - h->offset;
    size_t want = capacity < remaining ? capacity : (size_t)remaining, done = 0;
    while (done < want) {
        uint64_t position = h->offset + done;
        uint32_t logical = (uint32_t)(position / UFS2_BLOCK), within = (uint32_t)(position % UFS2_BLOCK), phys, run;
        e = map_lookup(&in, h->ino, logical, &phys, &run);
        if (e == VFS_OK) e = ufs2_block_read(CORE, phys, xb);
        if (e != VFS_OK) { h->offset += done; *bytes_read = done; return done ? VFS_OK : (e == VFS_NOT_FOUND ? VFS_CORRUPT : e); }
        size_t chunk = UFS2_BLOCK - within;
        if (chunk > want - done) chunk = want - done;
        memcpy((uint8_t *)buffer + done, xb + within, chunk);
        done += chunk;
    }
    h->offset += done; *bytes_read = done;
    return VFS_OK;
}
static enum vfs_error ufs2_file_write(struct vfs_file *f, const void *buffer, size_t length, size_t *bytes_written) {
    if (bytes_written) *bytes_written = 0;
    struct file_handle *h = file_get(f);
    if (!h || !bytes_written || (!buffer && length) || !(h->flags & VFS_OPEN_WRITE)) return VFS_BAD_HANDLE;
    if (v.core.read_only) return VFS_READ_ONLY;
    size_t done = 0;
    bool overwrote = false;
    enum vfs_error e = VFS_OK;
    while (done < length && e == VFS_OK) {
        struct ufs2_inode in;
        e = handle_inode(h, &in);
        if (e != VFS_OK) break;
        if (h->offset > in.size) { e = VFS_NO_SPACE; break; }
        /* A: bytes inside the current size are overwritten in place (not journaled, not atomic). */
        if (h->offset < in.size) {
            size_t n = length - done < in.size - h->offset ? length - done : (size_t)(in.size - h->offset), part = 0;
            while (part < n && e == VFS_OK) {
                uint64_t position = h->offset + part;
                uint32_t logical = (uint32_t)(position / UFS2_BLOCK), within = (uint32_t)(position % UFS2_BLOCK), phys, run;
                size_t chunk = UFS2_BLOCK - within;
                if (chunk > n - part) chunk = n - part;
                e = map_lookup(&in, h->ino, logical, &phys, &run);
                if (e == VFS_NOT_FOUND) e = VFS_CORRUPT;
                if (e == VFS_OK && chunk != UFS2_BLOCK) e = ufs2_block_read(CORE, phys, xb);
                if (e == VFS_OK) { memcpy(xb + within, (const uint8_t *)buffer + done + part, chunk); e = ufs2_block_write(CORE, phys, xb); }
                part += chunk;
            }
            if (e == VFS_OK) overwrote = true;
            h->offset += part; done += part;
            if (e != VFS_OK) break;
            continue;
        }
        /* B: append at the end of the file: one chunk = one transaction (data first). */
        uint32_t tail_space = in.size % UFS2_BLOCK ? UFS2_BLOCK - (uint32_t)(in.size % UFS2_BLOCK) : 0;
        size_t remaining = length - done;
        uint64_t max_bytes = (uint64_t)tail_space + (uint64_t)CHUNK_BLOCKS * UFS2_BLOCK;
        size_t nbytes = remaining < max_bytes ? remaining : (size_t)max_bytes;
        uint32_t old_blocks = blocks_of(&in);
        uint32_t need = (uint32_t)((in.size + nbytes + UFS2_BLOCK - 1) / UFS2_BLOCK) - old_blocks;
        if ((uint64_t)old_blocks + need > UINT32_MAX / 2) { e = VFS_NO_SPACE; break; }
        e = fs_begin();
        if (e != VFS_OK) break;
        struct ufs2_extent runs[CHUNK_EXTENTS]; uint32_t nr = 0, got = 0;
        if (need) {
            uint32_t hint = GEO.data_start, last_run;
            if (old_blocks && map_lookup(&in, h->ino, old_blocks - 1, &hint, &last_run) == VFS_OK) ++hint;   /* extend in place when free */
            e = alloc_blocks(need, hint, runs, &nr, &got);
            if (e != VFS_OK) { fs_abort(); break; }
            if (got < need) {                                      /* space or extent limit: write what fits */
                size_t fits = (size_t)tail_space + (size_t)got * UFS2_BLOCK;
                if (fits < nbytes) nbytes = fits;
                need = got;
            }
        }
        if (!nbytes) { fs_abort(); e = VFS_NO_SPACE; break; }
        /* Write the data. The tail block (already allocated) is updated in place; new blocks are zero-filled. */
        size_t written = 0;
        block_set_phase("data-write");
        if (tail_space) {
            uint32_t within = UFS2_BLOCK - tail_space, phys, run;
            size_t chunk = tail_space < nbytes ? tail_space : nbytes;
            e = map_lookup(&in, h->ino, old_blocks - 1, &phys, &run);
            if (e == VFS_OK) e = ufs2_block_read(CORE, phys, xb);
            if (e == VFS_OK) { memcpy(xb + within, (const uint8_t *)buffer + done, chunk); e = ufs2_block_write(CORE, phys, xb); }
            written = chunk;
        }
        uint32_t logical = old_blocks;
        for (uint32_t r = 0; r < nr && e == VFS_OK; ++r) {
            for (uint32_t k = 0; k < runs[r].length && e == VFS_OK; ++k) {
                if (logical - old_blocks >= need) break;
                size_t chunk = nbytes - written < UFS2_BLOCK ? nbytes - written : UFS2_BLOCK;
                memset(xb, 0, UFS2_BLOCK);
                if (chunk) memcpy(xb, (const uint8_t *)buffer + done + written, chunk);
                e = ufs2_block_write(CORE, runs[r].physical + k, xb);
                written += chunk; ++logical;
            }
        }
        if (e == VFS_OK) { block_set_phase("data-flush"); e = ufs2_flush(CORE); }      /* ordered data: durable before the metadata commit */
        if (e != VFS_OK) { fs_abort(); break; }
        /* Metadata: extents, size, counts and the bitmap, in one transaction. */
        struct ufs2_inode updated = in;
        uint32_t lg = old_blocks;
        for (uint32_t r = 0; r < nr && e == VFS_OK; ++r) {
            uint32_t len = runs[r].length;
            if (lg - old_blocks + len > need) len = need - (lg - old_blocks);
            if (len) { e = map_append(&updated, h->ino, lg, runs[r].physical, len); updated.block_count += len; lg += len; }
        }
        if (e == VFS_OK) { updated.size = in.size + nbytes; e = ino_store(h->ino, &updated); }
        if (e != VFS_OK) { fs_abort(); break; }
        e = fs_commit();
        if (e != VFS_OK) break;
        h->offset += nbytes; done += nbytes;
    }
    if (overwrote && e == VFS_OK) e = ufs2_flush(CORE);
    *bytes_written = done;
    return e;
}

/* ---- directory handles ---- */
static struct dir_handle *dir_get(struct vfs_dir *d) {
    if (!d || d->slot >= MAX_DIR_HANDLES) return 0;
    struct dir_handle *h = &v.dirs[d->slot];
    return h->active && h->generation == d->generation ? h : 0;
}
static enum vfs_error ufs2_dir_open(const char *path, struct vfs_dir *out) {
    if (!out) return VFS_BAD_HANDLE;
    if (out->slot || out->generation) return VFS_BAD_HANDLE;
    uint32_t ino; struct ufs2_inode in;
    enum vfs_error e = lookup(path, &ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != UFS2_DIR) return VFS_NOT_DIRECTORY;
    for (unsigned i = 0; i < MAX_DIR_HANDLES; ++i) {
        struct dir_handle *h = &v.dirs[i];
        if (h->active || h->generation == UINT32_MAX) continue;
        if (!h->generation) h->generation = 1;
        h->active = true; h->ino = ino; h->inode_generation = in.generation; h->index = 0;
        out->slot = i; out->generation = h->generation;
        return VFS_OK;
    }
    return VFS_NO_SPACE;
}
static enum vfs_error ufs2_dir_next(struct vfs_dir *d, char *name, size_t capacity, enum vfs_node_type *type) {
    struct dir_handle *h = dir_get(d);
    if (!h || !name || !capacity || !type) return VFS_BAD_HANDLE;
    struct ufs2_inode in;
    enum vfs_error e = ino_load(h->ino, &in);
    if (e != VFS_OK) return e;
    if (in.type != UFS2_DIR || in.generation != h->inode_generation) return VFS_CORRUPT;
    uint32_t n = blocks_of(&in);
    for (uint32_t index = h->index; index < n * UFS2_DIRENTS; ++index) {
        uint32_t l = index / UFS2_DIRENTS, phys;
        e = dir_block(&in, h->ino, l, &phys);
        if (e != VFS_OK) return e;
        unsigned i = index % UFS2_DIRENTS;
        if (!ufs2_dirent_inode(db, i)) continue;
        unsigned length = ufs2_dirent_name_length(db, i);
        if (capacity < length + 1) return VFS_BUFFER_TOO_SMALL;
        memcpy(name, ufs2_dirent_name(db, i), length); name[length] = 0;
        *type = ufs2_dirent_type(db, i) == UFS2_DIR ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
        h->index = index + 1;
        return VFS_OK;
    }
    h->index = n * UFS2_DIRENTS;
    return VFS_END;
}
static enum vfs_error ufs2_dir_close(struct vfs_dir *d) {
    struct dir_handle *h = dir_get(d);
    if (!h) return VFS_BAD_HANDLE;
    h->active = false; ++h->generation;
    d->slot = d->generation = 0;
    return VFS_OK;
}
static enum vfs_error ufs2_stat(const char *path, struct vfs_info *out) {
    if (!out) return VFS_BAD_HANDLE;
    struct ufs2_inode in;
    enum vfs_error e = lookup(path, 0, &in);
    if (e != VFS_OK) return e;
    out->type = in.type == UFS2_DIR ? VFS_NODE_DIRECTORY : VFS_NODE_FILE;
    out->size = in.type == UFS2_DIR ? 0 : (size_t)in.size;
    return VFS_OK;
}
static enum vfs_error ufs2_create_file(const char *path) { return create_node(path, UFS2_FILE); }
static enum vfs_error ufs2_create_directory(const char *path) { return create_node(path, UFS2_DIR); }
static bool ufs2_is_directory(const char *path) {
    struct ufs2_inode in;
    return lookup(path, 0, &in) == VFS_OK && in.type == UFS2_DIR;
}
static const struct vfs_backend_ops operations = {
    .copy = 0, .rename = ufs2_rename, .stat = ufs2_stat,
    .create_file = ufs2_create_file, .create_directory = ufs2_create_directory,
    .unlink = ufs2_unlink, .is_directory = ufs2_is_directory,
    .file_open = ufs2_file_open, .file_close = ufs2_file_close,
    .file_read = ufs2_file_read, .file_write = ufs2_file_write,
    .dir_open = ufs2_dir_open, .dir_next = ufs2_dir_next, .dir_close = ufs2_dir_close
};
const struct vfs_backend_ops *ufs2_fs_backend(void) { return &operations; }

/* ---- format ---- */
static enum vfs_error populate(struct ufs2_core *c, void *context) {
    (void)context;
    const struct ufs2_geometry *g = &c->sb.geo;
    static uint8_t blockbuf[UFS2_BLOCK];
    enum vfs_error e = VFS_OK;
    /* Bitmap: metadata regions, the superblock copies, the root directory block and the padding bits. */
    uint32_t root_block = g->data_start;
    for (uint32_t k = 0; e == VFS_OK && k < g->bitmap_blocks; ++k) {
        memset(blockbuf, 0, UFS2_BLOCK);
        for (uint32_t j = 0; j < UFS2_BITMAP_BITS; ++j) {
            uint32_t b = k * UFS2_BITMAP_BITS + j;
            bool used = b < g->data_start || b == root_block || b >= g->total - 1;      /* b >= total-1: SB B and padding */
            if (used) blockbuf[j >> 3] |= (uint8_t)(1U << (j & 7));
        }
        ufs2_bitmap_seal(c->sb.uuid, g->bitmap_start + k, blockbuf);
        e = ufs2_block_write(c, g->bitmap_start + k, blockbuf);
    }
    memset(blockbuf, 0, UFS2_BLOCK);
    for (uint32_t t = 0; e == VFS_OK && t < g->inode_blocks; ++t) {
        if (t == 0) {   /* inode 1: the root directory */
            struct ufs2_inode root = {0};
            root.type = UFS2_DIR; root.generation = 1; root.parent = 0; root.size = UFS2_BLOCK; root.root_entries = 1; root.extent_count = 1; root.block_count = 1;
            root.slot[0] = (struct ufs2_extent){0, root_block, 1, 0};
            root.txn_seq = 0;
            ufs2_inode_encode(c->sb.uuid, UFS2_ROOT_INODE, &root, blockbuf);
        }
        e = ufs2_block_write(c, g->inode_start + t, blockbuf);
        if (t == 0) memset(blockbuf, 0, UFS2_BLOCK);
    }
    if (e == VFS_OK) {
        ufs2_dirblock_init(blockbuf);
        ufs2_dirblock_seal(c->sb.uuid, root_block, blockbuf, UFS2_ROOT_INODE, 1, 0);
        e = ufs2_block_write(c, root_block, blockbuf);
    }
    return e;
}
enum vfs_error ufs2_fs_format(struct block_device *d, const char *label, const uint8_t *uuid) {
    if (v.mounted && v.core.dev == d) return VFS_BUSY;
    return ufs2_core_format_with(d, label, uuid, populate, 0);
}

/* ---- mount ---- */
static void release_memory(void) {
    if (v.bm) vmm_free_pages(v.bm);
    if (v.istate) vmm_free_pages(v.istate);
    v.bm = v.istate = 0;
}
enum vfs_error ufs2_fs_mount(struct block_device *d, bool want_write, struct ufs2_report *report) {
    if (v.mounted) return VFS_BUSY;
    memset(&v, 0, sizeof(v));
    static struct ufs2_report local;
    struct ufs2_report *r = report ? report : &local;
    enum vfs_error e = ufs2_core_open_ex(CORE, d, want_write, false);
    if (e != VFS_OK) { memset(&v, 0, sizeof(v)); return e; }
    e = ufs2_verify(CORE, r);
    if (e == VFS_OK && r->count[UFS2_STRUCTURAL]) e = VFS_CORRUPT;
    if (e != VFS_OK) { ufs2_test_abandon(CORE); memset(&v, 0, sizeof(v)); return e; }
    if (r->count[UFS2_REPAIRABLE] && !v.core.read_only) {       /* never write to a volume that needs repair */
        v.core.read_only = true; v.core.ro_reason = "repair recommended (run 'ultrium storage check')";
    }
    v.repair_recommended = r->count[UFS2_REPAIRABLE] != 0;
    v.bm = vmm_alloc_pages(((size_t)GEO.bitmap_blocks * 4088 + 4095) / 4096);
    v.istate = vmm_alloc_pages(((size_t)GEO.inode_count + 4095) / 4096);
    v.bm_pages = ((size_t)GEO.bitmap_blocks * 4088 + 4095) / 4096; v.istate_pages = ((size_t)GEO.inode_count + 4095) / 4096;
    if (!v.bm || !v.istate) e = VFS_NO_SPACE;
    for (uint32_t k = 0; e == VFS_OK && k < GEO.bitmap_blocks; ++k) {
        e = ufs2_block_read(CORE, GEO.bitmap_start + k, xb);
        if (e == VFS_OK && ufs2_bitmap_check(v.core.sb.uuid, GEO.bitmap_start + k, xb) != VFS_OK) {
            if (!v.core.read_only) e = VFS_CORRUPT;          /* unreachable: the verifier flagged it as repairable */
        }
        if (e == VFS_OK) memcpy(v.bm + (size_t)k * 4088, xb, 4088);
    }
    for (uint32_t t = 0; e == VFS_OK && t < GEO.inode_blocks; ++t) {
        e = ufs2_block_read(CORE, GEO.inode_start + t, xb);
        for (unsigned k = 0; e == VFS_OK && k < UFS2_INODES_PER_BLOCK; ++k) {
            struct ufs2_inode in;
            if (ufs2_inode_decode(v.core.sb.uuid, t * UFS2_INODES_PER_BLOCK + k + 1, xb + k * UFS2_INODE_SIZE, &in) != VFS_OK) { e = VFS_CORRUPT; break; }
            v.istate[t * UFS2_INODES_PER_BLOCK + k] = in.type != UFS2_FREE;
        }
    }
    if (e != VFS_OK) { release_memory(); ufs2_test_abandon(CORE); memset(&v, 0, sizeof(v)); return e; }
    v.free_blocks = 0;
    for (uint32_t b = GEO.data_start; b < GEO.total - 1; ++b) if (!bm_test(b)) ++v.free_blocks;
    v.free_inodes = 0;
    for (uint32_t i = 0; i < GEO.inode_count; ++i) if (!v.istate[i]) ++v.free_inodes;
    v.orphans = r->orphans; v.hint_block = GEO.data_start; v.hint_inode = 1;
    if (!v.core.read_only) {
        e = ufs2_core_mark_mounted(CORE);
        if (e != VFS_OK) { release_memory(); ufs2_test_abandon(CORE); memset(&v, 0, sizeof(v)); return e; }
    }
    v.mounted = true;
    return VFS_OK;
}
enum vfs_error ufs2_fs_sync(void) {
    if (!v.mounted) return VFS_OK;
    if (v.core.faulted) { (void)ufs2_flush(CORE); return VFS_IO; }
    if (v.core.read_only) return VFS_OK;
    enum vfs_error e = ufs2_flush(CORE);
    if (e != VFS_OK) return e;
    if (v.core.sb.state == UFS2_STATE_MOUNTED) {
        v.core.sb.state = UFS2_STATE_CLEAN;
        e = ufs2_sb_update(CORE);
        if (e != VFS_OK) v.core.sb.state = UFS2_STATE_MOUNTED;
    }
    return e;
}
enum vfs_error ufs2_fs_unmount(void) {
    if (!v.mounted) return VFS_NOT_FOUND;
    if (open_handles()) return VFS_BUSY;
    enum vfs_error e = ufs2_fs_sync();
    if (e != VFS_OK && !v.core.faulted) return e;                 /* never discard pending changes silently */
    bool faulted = v.core.faulted;
    release_memory();
    enum vfs_error c = ufs2_core_close(CORE);
    memset(&v, 0, sizeof(v));
    return faulted ? VFS_IO : (e != VFS_OK ? e : c);
}
void ufs2_fs_test_abandon(void) {
    if (!v.mounted) return;
    release_memory();
    ufs2_test_abandon(CORE);
    memset(&v, 0, sizeof(v));
}
bool ufs2_fs_mounted(void) { return v.mounted; }
struct ufs2_core *ufs2_fs_test_core(void) { return v.mounted ? CORE : 0; }
bool ufs2_fs_get_info(struct ufs2_fs_info *info) {
    if (!info || !v.mounted) return false;
    memcpy(info->label, v.core.sb.label, 33);
    info->total_blocks = GEO.total - 1 - GEO.data_start; info->free_blocks = v.free_blocks;
    info->inode_count = GEO.inode_count; info->free_inodes = v.free_inodes; info->orphans = v.orphans;
    info->read_only = v.core.read_only; info->faulted = v.core.faulted; info->unclean = v.core.unclean;
    info->repair_recommended = v.repair_recommended; info->generation = v.core.sb.generation; info->transactions = v.core.txns;
    info->device = v.core.dev; info->ro_reason = v.core.ro_reason;
    return true;
}
