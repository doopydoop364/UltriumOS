#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ufs2_fs.h"
#include "ufs2_meta.h"
#include "crc32c.h"
#include "memory/vmm.h"
#include "memory.h"

/* One bounded verification pass (docs/ultriumfs-v2-format.md section 11). It
   never modifies the volume. All loops are explicit and bounded by the inode
   count, the extent limits, the directory sizes and the volume size. */

#define IS_USED 1U
#define IS_REFERENCED 2U
#define IS_BAD 4U             /* inode failed validation: its extents are not trusted */
#define IS_DIR 8U
#define NAME_SCAN_MAX 2048U

struct vctx {
    struct ufs2_core *c;
    struct ufs2_report *r;
    const uint8_t *uuid;
    const struct ufs2_geometry *g;
    uint8_t *own, *disk, *istate;
    uint32_t *igen, *iparent;
    size_t own_pages, disk_pages, istate_pages, igen_pages, iparent_pages;
};

static uint8_t blk[UFS2_BLOCK], blk2[UFS2_BLOCK];
static struct ufs2_extent leaf_ext[UFS2_LEAF_EXTENTS];
static uint64_t name_hash[NAME_SCAN_MAX];

const char *ufs2_finding_name(uint8_t code) {
    switch (code) {
        case UF_BITMAP_CRC: return "bitmap block checksum";
        case UF_BITMAP_PADDING: return "bitmap padding bits";
        case UF_BITMAP_LEAK: return "block marked used but not owned";
        case UF_BITMAP_UNMARKED: return "block owned but marked free";
        case UF_INODE_CRC: return "inode checksum";
        case UF_INODE_FIELDS: return "inode fields invalid";
        case UF_ROOT_INVALID: return "root directory invalid";
        case UF_EXTENT_RANGE: return "extent out of range";
        case UF_EXTENT_ORDER: return "extent logical order";
        case UF_EXTENT_OVERLAP: return "block owned twice";
        case UF_EXTENT_COUNT: return "extent or block count mismatch";
        case UF_LEAF_INVALID: return "extent leaf invalid";
        case UF_DIR_BLOCK: return "directory block invalid";
        case UF_DIR_ENTRY: return "directory entry invalid or dangling";
        case UF_DIR_DUPLICATE_REF: return "inode referenced twice";
        case UF_DIR_PARENT: return "parent mismatch";
        case UF_DIR_CYCLE: return "directory cycle or excessive depth";
        case UF_DIR_DUPLICATE_NAME: return "duplicate name in a directory";
        case UF_ORPHAN_INODE: return "orphaned inode";
        case UF_STALE_STATE: return "stale mounted state";
        case UF_SB_COPY: return "superblock copy damaged";
        case UF_JOURNAL_PENDING: return "committed journal transaction pending";
    }
    return "unknown";
}
static void add(struct vctx *x, enum ufs2_finding_class cls, enum ufs2_finding_code code, uint32_t inode, uint32_t block) {
    struct ufs2_report *r = x->r;
    ++r->count[cls];
    if (r->stored < UFS2_REPORT_ITEMS) { r->item[r->stored].cls = (uint8_t)cls; r->item[r->stored].code = (uint8_t)code; r->item[r->stored].inode = inode; r->item[r->stored].block = block; ++r->stored; }
}
static bool bit(const uint8_t *map, uint32_t b) { return map[b >> 3] & (1U << (b & 7)); }
static void bit_set(uint8_t *map, uint32_t b) { map[b >> 3] |= (uint8_t)(1U << (b & 7)); }

static uint8_t *alloc_array(size_t bytes, size_t *pages) {
    *pages = (bytes + 4095) / 4096;
    if (!*pages) *pages = 1;
    return vmm_alloc_pages(*pages);
}
static void free_all(struct vctx *x) {
    if (x->own) vmm_free_pages(x->own);
    if (x->disk) vmm_free_pages(x->disk);
    if (x->istate) vmm_free_pages(x->istate);
    if (x->igen) vmm_free_pages(x->igen);
    if (x->iparent) vmm_free_pages(x->iparent);
}

/* ---- extent iteration (explicit state, no recursion) ---- */
struct eiter {
    struct ufs2_core *c; const uint8_t *uuid; const struct ufs2_inode *in; uint32_t ino;
    uint32_t slot, pos, leaf_n; bool leaf_ready, bad;
};
static void iter_init(struct eiter *it, struct ufs2_core *c, const struct ufs2_inode *in, uint32_t ino) {
    it->c = c; it->uuid = c->sb.uuid; it->in = in; it->ino = ino; it->slot = it->pos = it->leaf_n = 0; it->leaf_ready = false; it->bad = false;
}
static bool iter_next(struct eiter *it, struct ufs2_extent *out) {
    const struct ufs2_inode *in = it->in;
    if (in->depth == 0) {
        if (it->pos >= in->root_entries) return false;
        *out = in->slot[it->pos++];
        return true;
    }
    for (;;) {
        if (!it->leaf_ready) {
            if (it->slot >= in->root_entries) return false;
            uint32_t leaf = in->slot[it->slot].physical, first = 0, n = 0;
            if (ufs2_block_read(it->c, leaf, blk2) != VFS_OK ||
                ufs2_leaf_decode(it->uuid, leaf, blk2, it->ino, in->generation, &first, &n, leaf_ext) != VFS_OK) { it->bad = true; return false; }
            it->leaf_n = n; it->pos = 0; it->leaf_ready = true;
        }
        if (it->pos < it->leaf_n) { *out = leaf_ext[it->pos++]; return true; }
        it->leaf_ready = false; ++it->slot;
    }
}

/* Validate one extent and claim its blocks. Returns false on any violation. */
static bool claim_extent(struct vctx *x, uint32_t ino, const struct ufs2_extent *e, uint32_t *expect, uint64_t *data_blocks) {
    const struct ufs2_geometry *g = x->g;
    if (e->flags) { add(x, UFS2_STRUCTURAL, UF_INODE_FIELDS, ino, 0); return false; }
    if (!e->length || e->length > UFS2_MAX_EXTENT_LEN || e->physical < g->data_start || (uint64_t)e->physical + e->length > (uint64_t)g->total - 1) {
        add(x, UFS2_STRUCTURAL, UF_EXTENT_RANGE, ino, e->physical); return false;
    }
    if (e->logical != *expect) { add(x, UFS2_STRUCTURAL, UF_EXTENT_ORDER, ino, e->physical); return false; }
    for (uint32_t b = e->physical; b < e->physical + e->length; ++b) {
        if (bit(x->own, b)) { add(x, UFS2_STRUCTURAL, UF_EXTENT_OVERLAP, ino, b); return false; }   /* stops at the first duplicate */
        bit_set(x->own, b);
    }
    *expect += e->length; *data_blocks += e->length; ++x->r->extents;
    return true;
}

static void scan_inode(struct vctx *x, uint32_t ino, const struct ufs2_inode *in) {
    struct ufs2_report *r = x->r;
    x->istate[ino - 1] = IS_USED | (in->type == UFS2_DIR ? IS_DIR : 0);
    x->igen[ino - 1] = in->generation; x->iparent[ino - 1] = in->parent;
    ++r->inodes_used;
    if (in->type == UFS2_DIR) ++r->directories; else ++r->files;
    uint32_t expect = 0, leaves = 0; uint64_t data = 0, extents = 0;
    bool ok = true;
    if (in->depth == 0) {
        if (in->root_entries != in->extent_count) { add(x, UFS2_STRUCTURAL, UF_EXTENT_COUNT, ino, 0); ok = false; }
        for (uint32_t i = 0; ok && i < in->root_entries; ++i) { ok = claim_extent(x, ino, &in->slot[i], &expect, &data); ++extents; }
    } else {
        if (!in->root_entries) { add(x, UFS2_STRUCTURAL, UF_INODE_FIELDS, ino, 0); ok = false; }
        for (uint32_t i = 0; ok && i < in->root_entries; ++i) {
            const struct ufs2_extent *idx = &in->slot[i];
            uint32_t leaf = idx->physical;
            if (idx->flags || idx->logical != expect || !idx->length || idx->length > UFS2_LEAF_EXTENTS) { add(x, UFS2_STRUCTURAL, UF_LEAF_INVALID, ino, leaf); ok = false; break; }
            if (leaf < x->g->data_start || leaf >= x->g->total - 1 || bit(x->own, leaf)) { add(x, UFS2_STRUCTURAL, bit(x->own, leaf) ? UF_EXTENT_OVERLAP : UF_EXTENT_RANGE, ino, leaf); ok = false; break; }
            bit_set(x->own, leaf); ++leaves;
            uint32_t first = 0, n = 0;
            if (ufs2_block_read(x->c, leaf, blk) != VFS_OK ||
                ufs2_leaf_decode(x->uuid, leaf, blk, ino, in->generation, &first, &n, leaf_ext) != VFS_OK || first != idx->logical || n != idx->length) {
                add(x, UFS2_STRUCTURAL, UF_LEAF_INVALID, ino, leaf); ok = false; break;
            }
            for (uint32_t k = 0; ok && k < n; ++k) { ok = claim_extent(x, ino, &leaf_ext[k], &expect, &data); ++extents; }
        }
        if (ok && extents != in->extent_count) { add(x, UFS2_STRUCTURAL, UF_EXTENT_COUNT, ino, 0); ok = false; }
    }
    if (ok) {
        uint64_t want = in->type == UFS2_DIR ? in->size / UFS2_BLOCK : (in->size + UFS2_BLOCK - 1) / UFS2_BLOCK;
        if ((in->type == UFS2_DIR && (in->size % UFS2_BLOCK || !want)) || data != want || in->block_count != data + leaves) {
            add(x, UFS2_STRUCTURAL, UF_EXTENT_COUNT, ino, 0); ok = false;
        }
    }
    if (!ok) x->istate[ino - 1] |= IS_BAD;
    r->blocks_owned += (uint32_t)(data + leaves);
}

/* ---- directories ---- */
static void scan_directory(struct vctx *x, uint32_t dir) {
    struct ufs2_core *c = x->c;
    struct ufs2_inode in;
    /* Re-read the directory inode (the pass above did not keep it). */
    uint32_t tb = x->g->inode_start + (dir - 1) / UFS2_INODES_PER_BLOCK;
    if (ufs2_block_read(c, tb, blk) != VFS_OK ||
        ufs2_inode_decode(x->uuid, dir, blk + ((dir - 1) % UFS2_INODES_PER_BLOCK) * UFS2_INODE_SIZE, &in) != VFS_OK) return;
    struct eiter it; struct ufs2_extent e;
    iter_init(&it, c, &in, dir);
    uint32_t logical = 0, names = 0;
    bool name_cap_hit = false;
    while (iter_next(&it, &e)) {
        for (uint32_t k = 0; k < e.length; ++k, ++logical) {
            if (ufs2_block_read(c, e.physical + k, blk) != VFS_OK || ufs2_dirblock_check(x->uuid, e.physical + k, blk, dir, in.generation, logical) != VFS_OK) {
                add(x, UFS2_STRUCTURAL, UF_DIR_BLOCK, dir, e.physical + k); continue;
            }
            for (unsigned i = 0; i < UFS2_DIRENTS; ++i) {
                if (!ufs2_dirent_valid(blk, i)) { add(x, UFS2_STRUCTURAL, UF_DIR_ENTRY, dir, e.physical + k); continue; }
                uint32_t child = ufs2_dirent_inode(blk, i);
                if (!child) continue;
                uint32_t gen = ufs2_dirent_generation(blk, i); uint8_t type = ufs2_dirent_type(blk, i);
                if (child <= UFS2_ROOT_INODE || child > x->g->inode_count || !(x->istate[child - 1] & IS_USED) ||
                    x->igen[child - 1] != gen || ((x->istate[child - 1] & IS_DIR) ? UFS2_DIR : UFS2_FILE) != type) {
                    add(x, UFS2_STRUCTURAL, UF_DIR_ENTRY, dir, e.physical + k); continue;
                }
                if (x->istate[child - 1] & IS_REFERENCED) { add(x, UFS2_STRUCTURAL, UF_DIR_DUPLICATE_REF, child, e.physical + k); continue; }
                x->istate[child - 1] |= IS_REFERENCED;
                if (x->iparent[child - 1] != dir) add(x, UFS2_STRUCTURAL, UF_DIR_PARENT, child, e.physical + k);
                /* Duplicate names within this directory (bounded quadratic scan on a 64-bit name hash). */
                uint32_t h1 = crc32c_begin(), h2 = 2166136261U;
                const uint8_t *nm = ufs2_dirent_name(blk, i); unsigned nl = ufs2_dirent_name_length(blk, i);
                h1 = crc32c_end(crc32c_update(h1, nm, nl));
                for (unsigned q = 0; q < nl; ++q) { h2 ^= nm[q]; h2 *= 16777619U; }
                uint64_t h = (uint64_t)h1 << 32 | h2;
                if (names < NAME_SCAN_MAX) {
                    for (uint32_t q = 0; q < names; ++q) if (name_hash[q] == h) { add(x, UFS2_STRUCTURAL, UF_DIR_DUPLICATE_NAME, dir, e.physical + k); break; }
                    name_hash[names++] = h;
                } else name_cap_hit = true;
            }
        }
    }
    if (it.bad) add(x, UFS2_STRUCTURAL, UF_LEAF_INVALID, dir, 0);
    if (name_cap_hit) x->r->large_directory_unchecked = true;
}

enum vfs_error ufs2_verify(struct ufs2_core *c, struct ufs2_report *r) {
    memset(r, 0, sizeof(*r));
    struct vctx x; memset(&x, 0, sizeof(x));
    x.c = c; x.r = r; x.uuid = c->sb.uuid; x.g = &c->sb.geo;
    const struct ufs2_geometry *g = x.g;
    uint32_t flat = g->bitmap_blocks * 4088U;
    x.own = alloc_array(flat, &x.own_pages); x.disk = alloc_array(flat, &x.disk_pages);
    x.istate = alloc_array(g->inode_count, &x.istate_pages);
    x.igen = (uint32_t *)alloc_array((size_t)g->inode_count * 4, &x.igen_pages);
    x.iparent = (uint32_t *)alloc_array((size_t)g->inode_count * 4, &x.iparent_pages);
    if (!x.own || !x.disk || !x.istate || !x.igen || !x.iparent) { free_all(&x); return VFS_NO_SPACE; }

    /* Superblock and journal conditions. */
    if (c->sb_was_degraded) add(&x, UFS2_REPAIRABLE, UF_SB_COPY, 0, 0);
    if (c->read_only && c->sb.state == UFS2_STATE_MOUNTED) add(&x, UFS2_REPAIRABLE, UF_STALE_STATE, 0, 0);

    /* Disk bitmap. */
    for (uint32_t k = 0; k < g->bitmap_blocks; ++k) {
        if (ufs2_block_read(c, g->bitmap_start + k, blk) != VFS_OK) { free_all(&x); return VFS_IO; }
        if (ufs2_bitmap_check(x.uuid, g->bitmap_start + k, blk) != VFS_OK) { add(&x, UFS2_REPAIRABLE, UF_BITMAP_CRC, 0, g->bitmap_start + k); continue; }
        memcpy(x.disk + (size_t)k * 4088U, blk, 4088);
    }
    /* Ownership: metadata regions first, then every inode's extents. */
    for (uint32_t b = 0; b < g->data_start; ++b) bit_set(x.own, b);
    bit_set(x.own, g->total - 1);
    for (uint32_t b = g->total; b < flat * 8U; ++b) bit_set(x.own, b);
    for (uint32_t t = 0; t < g->inode_blocks; ++t) {
        if (ufs2_block_read(c, g->inode_start + t, blk) != VFS_OK) { free_all(&x); return VFS_IO; }
        for (unsigned k = 0; k < UFS2_INODES_PER_BLOCK; ++k) {
            uint32_t ino = t * UFS2_INODES_PER_BLOCK + k + 1;
            struct ufs2_inode in;
            const uint8_t *raw = blk + k * UFS2_INODE_SIZE;
            if (ufs2_inode_decode(x.uuid, ino, raw, &in) != VFS_OK) { add(&x, UFS2_STRUCTURAL, UF_INODE_CRC, ino, g->inode_start + t); x.istate[ino - 1] = IS_USED | IS_BAD; continue; }
            if (in.type == UFS2_FREE) continue;
            if (in.generation == 0 || in.flags) { add(&x, UFS2_STRUCTURAL, UF_INODE_FIELDS, ino, 0); x.istate[ino - 1] = IS_USED | IS_BAD; continue; }
            scan_inode(&x, ino, &in);
            /* scan_inode may have read other blocks into the shared buffer; reload the table block. */
            if (ufs2_block_read(c, g->inode_start + t, blk) != VFS_OK) { free_all(&x); return VFS_IO; }
        }
    }
    /* Root. */
    if (!(x.istate[UFS2_ROOT_INODE - 1] & IS_USED) || !(x.istate[UFS2_ROOT_INODE - 1] & IS_DIR) || (x.istate[UFS2_ROOT_INODE - 1] & IS_BAD) || x.iparent[UFS2_ROOT_INODE - 1] != 0)
        add(&x, UFS2_STRUCTURAL, UF_ROOT_INVALID, UFS2_ROOT_INODE, 0);
    /* Directory references (all directories, reachable or not). */
    for (uint32_t ino = 1; ino <= g->inode_count; ++ino)
        if ((x.istate[ino - 1] & (IS_USED | IS_DIR | IS_BAD)) == (IS_USED | IS_DIR)) scan_directory(&x, ino);
    /* Orphans and parent chains. */
    for (uint32_t ino = 2; ino <= g->inode_count; ++ino)
        if ((x.istate[ino - 1] & IS_USED) && !(x.istate[ino - 1] & IS_REFERENCED) && !(x.istate[ino - 1] & IS_BAD)) { add(&x, UFS2_ORPHAN, UF_ORPHAN_INODE, ino, 0); ++r->orphans; }
    for (uint32_t ino = 2; ino <= g->inode_count; ++ino) {
        if ((x.istate[ino - 1] & (IS_USED | IS_DIR | IS_BAD)) != (IS_USED | IS_DIR)) continue;
        uint32_t p = x.iparent[ino - 1]; unsigned steps = 0; bool reached = false;
        while (p && steps < 64) {
            if (p > g->inode_count || !(x.istate[p - 1] & IS_USED) || !(x.istate[p - 1] & IS_DIR)) { add(&x, UFS2_STRUCTURAL, UF_DIR_PARENT, ino, 0); reached = true; break; }
            if (p == UFS2_ROOT_INODE) { reached = true; break; }
            p = x.iparent[p - 1]; ++steps;
        }
        if (!reached) add(&x, UFS2_STRUCTURAL, UF_DIR_CYCLE, ino, 0);
    }
    /* Bitmap against derived ownership. */
    for (uint32_t b = 0; b < flat * 8U; ++b) {
        bool d = bit(x.disk, b), o = bit(x.own, b);
        if (b >= g->total) { if (!d) { add(&x, UFS2_REPAIRABLE, UF_BITMAP_PADDING, 0, b); break; } continue; }
        if (d && !o) { if (!r->bitmap_leaked++) add(&x, UFS2_REPAIRABLE, UF_BITMAP_LEAK, 0, b); }
        else if (!d && o) { if (!r->bitmap_unmarked++) add(&x, UFS2_REPAIRABLE, UF_BITMAP_UNMARKED, 0, b); }
    }
    free_all(&x);
    return VFS_OK;
}
