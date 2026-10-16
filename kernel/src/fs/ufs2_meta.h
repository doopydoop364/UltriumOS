#ifndef ULTRIUM_UFS2_META_H
#define ULTRIUM_UFS2_META_H
#include <stdbool.h>
#include <stdint.h>
#include "ufs2.h"

/* UltriumFS 2 metadata codecs (docs/ultriumfs-v2-format.md sections 5-7).
   Every structure is encoded byte by byte and carries a contextual CRC-32C. */
#define UFS2_ROOT_SLOTS 11U
#define UFS2_LEAF_EXTENTS 253U
#define UFS2_MAX_EXTENTS (UFS2_ROOT_SLOTS * UFS2_LEAF_EXTENTS)
#define UFS2_MAX_EXTENT_LEN (1U << 20)
#define UFS2_DIRENTS 63U
#define UFS2_NAME_MAX 31U
#define UFS2_INODES_PER_BLOCK 16U
#define UFS2_ROOT_INODE 1U

enum ufs2_inode_type { UFS2_FREE = 0, UFS2_FILE = 1, UFS2_DIR = 2 };

struct ufs2_extent { uint32_t logical, physical, length, flags; };
/* Root slots are extents at depth 0 and index entries at depth 1:
   {first_logical, leaf_block, leaf_extents, 0} stored in the same four fields. */
struct ufs2_inode {
    uint8_t type, depth;
    uint32_t generation;
    uint64_t size;
    uint32_t parent, flags, extent_count, block_count;
    uint64_t txn_seq;
    uint16_t root_entries;
    struct ufs2_extent slot[UFS2_ROOT_SLOTS];
};

/* An all-zero slot is a pristine free inode (generation 0). */
bool ufs2_slot_pristine(const uint8_t *raw);
/* VFS_OK, or VFS_CORRUPT (checksum, reserved bytes, unknown type/depth/flags). */
enum vfs_error ufs2_inode_decode(const uint8_t uuid[16], uint32_t ino, const uint8_t *raw, struct ufs2_inode *out);
void ufs2_inode_encode(const uint8_t uuid[16], uint32_t ino, const struct ufs2_inode *in, uint8_t *raw);

/* Extent-leaf block. `entries` receives the extent count. */
enum vfs_error ufs2_leaf_decode(const uint8_t uuid[16], uint32_t block, const uint8_t *raw, uint32_t owner,
                                uint32_t generation, uint32_t *first_logical, uint32_t *entries,
                                struct ufs2_extent *out /* UFS2_LEAF_EXTENTS */);
void ufs2_leaf_encode(const uint8_t uuid[16], uint32_t block, uint8_t *raw, uint32_t owner, uint32_t generation,
                      uint32_t first_logical, uint32_t entries, const struct ufs2_extent *extents);

/* Directory block: 63 entries + footer. */
enum vfs_error ufs2_dirblock_check(const uint8_t uuid[16], uint32_t block, const uint8_t *raw, uint32_t owner,
                                   uint32_t generation, uint32_t logical);
void ufs2_dirblock_init(uint8_t *raw);   /* zero entries; footer written by seal */
void ufs2_dirblock_seal(const uint8_t uuid[16], uint32_t block, uint8_t *raw, uint32_t owner,
                        uint32_t generation, uint32_t logical);
/* Entry accessors (index 0..62). */
uint32_t ufs2_dirent_inode(const uint8_t *raw, unsigned index);
uint32_t ufs2_dirent_generation(const uint8_t *raw, unsigned index);
uint8_t ufs2_dirent_type(const uint8_t *raw, unsigned index);
unsigned ufs2_dirent_name_length(const uint8_t *raw, unsigned index);
const uint8_t *ufs2_dirent_name(const uint8_t *raw, unsigned index);
bool ufs2_dirent_valid(const uint8_t *raw, unsigned index);          /* structure and name rules */
void ufs2_dirent_set(uint8_t *raw, unsigned index, uint32_t inode, uint32_t generation, uint8_t type,
                     const char *name, unsigned length);
void ufs2_dirent_clear(uint8_t *raw, unsigned index);
bool ufs2_name_valid(const char *name, unsigned length);

/* Bitmap block k covers filesystem blocks [k * 32704, (k + 1) * 32704). */
enum vfs_error ufs2_bitmap_check(const uint8_t uuid[16], uint32_t block, const uint8_t *raw);
void ufs2_bitmap_seal(const uint8_t uuid[16], uint32_t block, uint8_t *raw);
#endif
