#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "ufs2_meta.h"
#include "memory.h"

#define MAGIC_LEAF 0x4c545845U    /* "EXTL" little-endian */
#define MAGIC_DIRB 0x42524944U    /* "DIRB" */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static bool zero(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return false; return true; }

/* ---- inode ---- */
bool ufs2_slot_pristine(const uint8_t *raw) { return zero(raw, UFS2_INODE_SIZE); }

enum vfs_error ufs2_inode_decode(const uint8_t uuid[16], uint32_t ino, const uint8_t *raw, struct ufs2_inode *in) {
    memset(in, 0, sizeof(*in));
    if (ufs2_slot_pristine(raw)) return VFS_OK;
    if (le32(raw + 252) != ufs2_crc32(uuid, ino, raw, 252)) return VFS_CORRUPT;
    if (raw[0] > UFS2_DIR || raw[1] || raw[3] || raw[2] > 1 || le32(raw + 20) || !zero(raw + 42, 22) || !zero(raw + 240, 12))
        return VFS_CORRUPT;
    in->type = raw[0]; in->depth = raw[2];
    in->generation = le32(raw + 4); in->size = le64(raw + 8); in->parent = le32(raw + 16);
    in->flags = 0; in->extent_count = le32(raw + 24); in->block_count = le32(raw + 28); in->txn_seq = le64(raw + 32);
    in->root_entries = le16(raw + 40);
    if (in->root_entries > UFS2_ROOT_SLOTS) return VFS_CORRUPT;
    for (unsigned i = 0; i < UFS2_ROOT_SLOTS; ++i) {
        const uint8_t *s = raw + 64 + i * 16;
        in->slot[i] = (struct ufs2_extent){le32(s), le32(s + 4), le32(s + 8), le32(s + 12)};
        if (i >= in->root_entries && (in->slot[i].logical || in->slot[i].physical || in->slot[i].length || in->slot[i].flags)) return VFS_CORRUPT;
    }
    if (in->type == UFS2_FREE) {   /* a freed inode keeps only its generation */
        if (in->size || in->parent || in->extent_count || in->block_count || in->txn_seq || in->root_entries || in->depth) return VFS_CORRUPT;
    }
    return VFS_OK;
}
void ufs2_inode_encode(const uint8_t uuid[16], uint32_t ino, const struct ufs2_inode *in, uint8_t *raw) {
    memset(raw, 0, UFS2_INODE_SIZE);
    raw[0] = in->type; raw[2] = in->depth;
    put32(raw + 4, in->generation); put64(raw + 8, in->size); put32(raw + 16, in->parent);
    put32(raw + 24, in->extent_count); put32(raw + 28, in->block_count); put64(raw + 32, in->txn_seq);
    put16(raw + 40, in->root_entries);
    for (unsigned i = 0; i < UFS2_ROOT_SLOTS; ++i) {
        uint8_t *s = raw + 64 + i * 16;
        put32(s, in->slot[i].logical); put32(s + 4, in->slot[i].physical); put32(s + 8, in->slot[i].length); put32(s + 12, in->slot[i].flags);
    }
    put32(raw + 252, ufs2_crc32(uuid, ino, raw, 252));
}

/* ---- extent leaf ---- */
enum vfs_error ufs2_leaf_decode(const uint8_t uuid[16], uint32_t block, const uint8_t *raw, uint32_t owner,
                                uint32_t generation, uint32_t *first_logical, uint32_t *entries, struct ufs2_extent *out) {
    if (le32(raw) != MAGIC_LEAF) return VFS_CORRUPT;
    if (le32(raw + 4092) != ufs2_crc64ctx(uuid, block, raw, 4092)) return VFS_CORRUPT;
    uint32_t n = le16(raw + 12);
    if (le32(raw + 4) != owner || le32(raw + 8) != generation || le16(raw + 14) != 0 || n > UFS2_LEAF_EXTENTS ||
        !zero(raw + 20, 12) || !zero(raw + 4080, 12)) return VFS_CORRUPT;
    *first_logical = le32(raw + 16); *entries = n;
    for (uint32_t i = 0; i < UFS2_LEAF_EXTENTS; ++i) {
        const uint8_t *e = raw + 32 + i * 16;
        struct ufs2_extent x = {le32(e), le32(e + 4), le32(e + 8), le32(e + 12)};
        if (i < n) out[i] = x;
        else if (x.logical || x.physical || x.length || x.flags) return VFS_CORRUPT;
    }
    return VFS_OK;
}
void ufs2_leaf_encode(const uint8_t uuid[16], uint32_t block, uint8_t *raw, uint32_t owner, uint32_t generation,
                      uint32_t first_logical, uint32_t entries, const struct ufs2_extent *x) {
    memset(raw, 0, UFS2_BLOCK);
    put32(raw, MAGIC_LEAF); put32(raw + 4, owner); put32(raw + 8, generation); put16(raw + 12, (uint16_t)entries);
    put32(raw + 16, first_logical);
    for (uint32_t i = 0; i < entries; ++i) {
        uint8_t *e = raw + 32 + i * 16;
        put32(e, x[i].logical); put32(e + 4, x[i].physical); put32(e + 8, x[i].length); put32(e + 12, x[i].flags);
    }
    put32(raw + 4092, ufs2_crc64ctx(uuid, block, raw, 4092));
}

/* ---- directory block ---- */
bool ufs2_name_valid(const char *name, unsigned length) {
    if (!length || length > UFS2_NAME_MAX) return false;
    for (unsigned i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == '/' || c == 127) return false;
    }
    return true;
}
enum vfs_error ufs2_dirblock_check(const uint8_t uuid[16], uint32_t block, const uint8_t *raw, uint32_t owner,
                                   uint32_t generation, uint32_t logical) {
    if (le32(raw + 4092) != ufs2_crc64ctx(uuid, block, raw, 4092)) return VFS_CORRUPT;
    if (le32(raw + 4032) != MAGIC_DIRB || le32(raw + 4036) != owner || le32(raw + 4040) != generation ||
        le32(raw + 4044) != logical || !zero(raw + 4048, 44)) return VFS_CORRUPT;
    return VFS_OK;
}
void ufs2_dirblock_init(uint8_t *raw) { memset(raw, 0, UFS2_BLOCK); }
void ufs2_dirblock_seal(const uint8_t uuid[16], uint32_t block, uint8_t *raw, uint32_t owner, uint32_t generation, uint32_t logical) {
    memset(raw + 4032, 0, 64);
    put32(raw + 4032, MAGIC_DIRB); put32(raw + 4036, owner); put32(raw + 4040, generation); put32(raw + 4044, logical);
    put32(raw + 4092, ufs2_crc64ctx(uuid, block, raw, 4092));
}
uint32_t ufs2_dirent_inode(const uint8_t *raw, unsigned i) { return le32(raw + i * UFS2_DIRENT_SIZE); }
uint32_t ufs2_dirent_generation(const uint8_t *raw, unsigned i) { return le32(raw + i * UFS2_DIRENT_SIZE + 4); }
uint8_t ufs2_dirent_type(const uint8_t *raw, unsigned i) { return raw[i * UFS2_DIRENT_SIZE + 8]; }
unsigned ufs2_dirent_name_length(const uint8_t *raw, unsigned i) { return raw[i * UFS2_DIRENT_SIZE + 9]; }
const uint8_t *ufs2_dirent_name(const uint8_t *raw, unsigned i) { return raw + i * UFS2_DIRENT_SIZE + 12; }
bool ufs2_dirent_valid(const uint8_t *raw, unsigned i) {
    const uint8_t *e = raw + i * UFS2_DIRENT_SIZE;
    if (!le32(e)) return zero(e, UFS2_DIRENT_SIZE);
    unsigned n = e[9];
    if ((e[8] != UFS2_FILE && e[8] != UFS2_DIR) || le16(e + 10) || !ufs2_name_valid((const char *)e + 12, n)) return false;
    return zero(e + 12 + n, 52 - n);
}
void ufs2_dirent_set(uint8_t *raw, unsigned i, uint32_t inode, uint32_t generation, uint8_t type, const char *name, unsigned length) {
    uint8_t *e = raw + i * UFS2_DIRENT_SIZE;
    memset(e, 0, UFS2_DIRENT_SIZE);
    put32(e, inode); put32(e + 4, generation); e[8] = type; e[9] = (uint8_t)length;
    memcpy(e + 12, name, length);
}
void ufs2_dirent_clear(uint8_t *raw, unsigned i) { memset(raw + i * UFS2_DIRENT_SIZE, 0, UFS2_DIRENT_SIZE); }

/* ---- bitmap block ---- */
enum vfs_error ufs2_bitmap_check(const uint8_t uuid[16], uint32_t block, const uint8_t *raw) {
    if (le32(raw + 4092) != ufs2_crc64ctx(uuid, block, raw, 4092)) return VFS_CORRUPT;
    return zero(raw + 4088, 4) ? VFS_OK : VFS_CORRUPT;
}
void ufs2_bitmap_seal(const uint8_t uuid[16], uint32_t block, uint8_t *raw) {
    memset(raw + 4088, 0, 4);
    put32(raw + 4092, ufs2_crc64ctx(uuid, block, raw, 4092));
}
