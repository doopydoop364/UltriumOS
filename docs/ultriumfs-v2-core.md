# UltriumFS 2 filesystem core (implementation notes)

Status: implemented and QEMU-tested on memory-backed devices (`ultrium test ufs2fs`, 21 cases). Not yet reachable from the shell: `ultrium storage format|mount` still create and mount v1 volumes. The next pull request adds the mount policy, the v1 read-only path, the checker and the shell commands. Specification: [ultriumfs-v2-format.md](ultriumfs-v2-format.md); journal layer: [ultriumfs-v2-journal.md](ultriumfs-v2-journal.md).

Code: `kernel/src/fs/ufs2_meta.[ch]` (inode, extent leaf, directory block and bitmap block codecs with contextual CRC-32C), `ufs2_verify.c` (the verifier shared by mount and the future checker), `ufs2_fs.[ch]` (operations, format, mount).

## What exists

- **Format and mount.** `ufs2_fs_format` writes the bitmap, a zeroed inode table, the root directory and then the superblock pair last (interrupted formats are never mountable). `ufs2_fs_mount` opens the core (journal replay on writable devices), runs the verifier, and applies the mount policy of section 10 of the specification: structural findings refuse the mount (nothing is written, resources released), repairable findings mount **read-only** with `repair_recommended`, orphans are counted and tolerated, a device without flush or a read-only device mounts read-only with the reason. The volume is marked *mounted* only after verification passes.
- **Operations as single transactions:** create file, mkdir, unlink, rmdir, rename (same or different directory, files and directories, with the parent link updated atomically and a destination-ancestry check), directory growth, file growth chunks. Bitmap, inode and directory changes always travel in the same transaction. Memory state (bitmap, inode flags, counters) changes immediately and is rolled back from an undo list if a transaction is abandoned.
- **Ordered data.** A write call appends in chunks of at most 64 new blocks and 3 new extents: the chunk's data is written and **flushed**, then one transaction updates size, extents, leaf blocks and bitmap. Overwrites of existing bytes are in place, **not journaled and not atomic** (documented in the specification); the volume is flushed before the call returns.
- **Extents.** Append-only maps: 11 inline extents, promotion to depth 1 with 253-extent leaf blocks (up to 11 leaves, **2783 extents**), merging with the last extent when physically adjacent, allocation that first tries to extend in place. Files of any size up to free space and the extent limit; a 5 MiB file (above v1's 4 MiB limit) is tested through the backend operations.
- **Verification** (one bounded pass): superblock/journal conditions, every inode checksum and reserved field, extent order/range/overflow, **duplicate block ownership across the whole volume**, leaf blocks, block counts, every directory block footer and entry (generation-checked), duplicate references, parent links, parent-chain cycles (64 steps), duplicate names (bounded quadratic scan, up to 2048 entries per directory; larger directories set `large_directory_unchecked`), orphans, and the on-disk bitmap against the ownership derived from the inodes (leaks, unmarked owned blocks, padding bits, checksums).
- **Inode generations.** A freed inode keeps its generation with a valid checksum, reallocation increments it, and directory entries carry the generation, so a stale entry is detected as dangling.

## Verification of the implementation

21 kernel cases: format/mount accounting, tree and error paths, file I/O and growth, a 5 MiB file, fragmentation (a file spanning a leaf block, with every block returned on delete), a two-leaf extent map, disk full and reuse, inode exhaustion, directory growth, rename/busy/handles, persistence and unclean sessions (including re-marking after a sync), read-only and flush policy, four device block sizes, inode generations, a crafted **maximal 2783-extent file** (verifies cleanly, reads back through all 11 leaves, refuses an append with no leaked blocks), 20 kinds of crafted **structural damage** (each must be refused for the *specific* reason and leave resources released), repairable damage (mounts read-only), orphans, a stale-generation entry, stale state on a read-only open, and an **I/O-error sweep** over create, mkdir, a growing write, rename and unlink at every device transfer, requiring a clean remount each time.

### Negative controls (temporary mutations, restored after each run)

| Deliberate defect | Result |
| --- | --- |
| verifier ignores duplicate block ownership | structural-damage case fails |
| extent leaf blocks not freed on delete | fragmentation and two-leaf cases fail |
| extents merged without checking physical adjacency | 3 cases fail (data corruption) |
| freed inode loses its generation | generations case fails |
| rename does not update the moved inode's parent | rename and I/O-error cases fail |
| directory block checksum not sealed | 19 of 20 cases fail |

A bug found while writing the tests: `ino_store` stamped a transaction sequence on freed inodes, which the format forbids (all fields zero but the generation); the decoder rejected them and every unlink produced an inconsistent volume. Fixed before this PR.

## Not covered here

Power-loss crash recovery of file operations (the next-but-one PR builds the recovery matrix on top of this layer; the journal layer itself is covered in its own PR); the checker/repair commands; the VFS mount and shell integration; concurrency (single CPU).
