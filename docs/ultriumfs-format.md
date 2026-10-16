# UltriumFS on-disk format, version 1

Status: implemented (`kernel/src/fs/ultriumfs.c`) and QEMU-tested on memory-backed and virtio devices. **No crash consistency is claimed** (see Limits).

All integers are **little-endian** and are encoded and decoded byte by byte; no C structure is ever written to disk. The corruption tests build damaged images with an independent encoder written from this document.

## Geometry

- Filesystem block: **4096 bytes**. The device logical block size must divide 4096 (512, 1024, 2048 or 4096); a filesystem block is `4096 / block_size` device blocks.
- Volume size: 16 to 2^20 blocks (**4 GiB maximum**). Larger devices use only the first 2^20 blocks.
- Layout (filesystem blocks): `0` superblock, `1 .. 1+I` inode table (`I = inode_count / 32`), then data blocks. There is **no on-disk free-space bitmap**.
- Inodes: `inode_count = clamp(total/4, 32, 65536)` rounded up to a multiple of 32; inode numbers start at 1; **inode 1 is the root directory**.

## Superblock (block 0)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `ULTRIUMF` |
| 8 | 4 | version (= 1) |
| 12 | 4 | block size (= 4096) |
| 16 | 8 | total blocks |
| 24 | 4 | inode count |
| 28 | 4 | inode table start (= 1) |
| 32 | 4 | inode table blocks |
| 36 | 4 | data start |
| 40 | 4 | root inode (= 1) |
| 44 | 4 | state: 0 clean, 1 modified since last sync |
| 56 | 16 | label (printable ASCII, NUL padded) |
| 72 | 4 | inode size (= 128) |
| 76 | 4 | directory entry size (= 64) |
| 80 | 4 | incompatible feature flags (must be 0) |
| 124 | 4 | CRC-32 of bytes 0..123 |

The rest is zero. CRC-32 is the IEEE polynomial (reflected 0xEDB88320, initial and final XOR 0xFFFFFFFF). Mount refuses: wrong magic or CRC or inconsistent geometry (`corrupt`); any version other than 1 or any nonzero feature flags (`unsupported`); a volume larger than the device.

## Inode (128 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | type: 0 free, 1 file, 2 directory |
| 1..7 | | reserved, must be 0 |
| 8 | 8 | size in bytes (directories: allocated bytes, a multiple of 4096) |
| 16 | 48 | 12 direct block numbers |
| 64 | 4 | indirect block number |
| 68 | 4 | block count (data blocks plus the indirect block) |
| 72..123 | | reserved, must be 0 |
| 124 | 4 | CRC-32 of bytes 0..123 |

A free inode is all zero. A file of size `s` uses `ceil(s/4096)` data blocks: the first 12 through `direct`, the rest through the indirect block (1024 entries). Pointers beyond the used count must be zero. **Maximum file size: (12 + 1024) × 4096 = 4,243,456 bytes.** Directories always have at least one block.

## Directory entry (64 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | inode number, 0 = free slot (then the whole entry is zero) |
| 4 | 1 | type (1 file, 2 directory), must match the inode |
| 5 | 1 | name length, 1..31 |
| 6..7 | | zero |
| 8 | 56 | name bytes, rest zero |

Names are 1 to **31** bytes (the VFS limit; the entry has room for 55), any byte except `/`, control characters and 0x7f. There are no `.`/`..` entries; the VFS normalizes those lexically. Directories grow by whole blocks (64 entries each) and are never shrunk.

## Mount verification

Mount reads the whole inode table once (bounded by the inode count) and builds the free-block bitmap **from the inodes**, which is why it cannot leak blocks. It rejects, with no repair attempted:

- inode checksum errors, reserved bytes set, size above the maximum, block-count mismatch
- block pointers outside the data area, **blocks claimed twice** (including metadata overlap), nonzero pointers past the file's end
- a missing or non-directory root
- directory entries naming free, out-of-range or root inodes, **inodes referenced twice**, type mismatches, bad names or nonzero padding

Unreferenced non-root inodes (**orphans**, which a crash can leave) are tolerated, counted and reported; their blocks stay allocated. Cycles cannot be walked from the root because every inode is referenced at most once and the root is never referenced. Not detected at mount: duplicate names inside one directory (lookups return the first match); file data is not checksummed.

A mount that fails releases all memory and the device claim and leaves the device unchanged; the same device can then be reformatted.

## Allocation and limits

| Item | Value |
| --- | --- |
| Allocation | next-fit over the in-memory bitmap, single blocks, no extents, fragmentation allowed |
| Write path | files grow in whole blocks; new blocks are zero-filled; growth allocates every needed block first and releases them all if anything fails |
| Out of space | `no space`, volume unchanged; metadata exhaustion (inodes) is reported the same way |
| Max file | 4,243,456 bytes; maximum volume 4 GiB; maximum 65,536 inodes |
| Files per directory | limited by the directory's own file-size limit (about 66,000 entries) |
| Open handles | 16 files and 8 directory iterators per volume; one mounted volume at a time |
| Memory at mount | `total/8` bytes of bitmap plus one byte per inode (at most 128 KiB + 64 KiB) |

## Integrity, ordering and failures

There is no cache: every change is written to the device immediately (write-through) and becomes durable after a device flush (`ultrium storage sync`, unmount or shutdown). Before the first change after a sync, the superblock state is set to *modified* and flushed; sync flushes, sets it to *clean* and flushes again. Mounting a volume in the *modified* state works but is reported as an **unclean mount**.

Ordering is chosen so interruption leaves orphans rather than damage where possible: create writes the child inode, then the entry; unlink clears the entry, then the inode; rename reserves the new slot, clears the old entry, then writes the new one; growth writes data, then the indirect block, then the inode last.

A failed metadata write is treated as uncertain (a 4 KiB write is several device transfers and some may have landed): the volume turns **read-only and faulted**, refuses changes and never marks itself clean. It must be unmounted and remounted, which re-verifies it. Overwriting existing file bytes is not atomic and a failure can leave them partly written.

## Limits and non-goals

- **No journal and no crash consistency.** A power loss can lose recent writes, tear overwritten data, leave orphans, or (for a cross-directory rename or a torn metadata block) leave a volume that mount rejects. No repair tool exists. Power-loss safety has not been tested and must not be assumed.
- No permissions, timestamps, links, extended attributes, sparse files, truncate, or POSIX semantics.
- A journal or copy-on-write metadata, a checksum on data, double-indirect blocks and an fsck are candidate later work. A journal would add a log region and replay on mount; the version field and incompatible-feature flags exist so such a change can be introduced without silently mis-mounting.
- Formatting requires a writable, unmounted, **scratch-flagged** device (see `docs/block-devices.md`); it first invalidates the old superblock, then writes tables, and writes the new superblock last.

## Tests

`ultrium test ultriumfs` (17 cases, also part of `ultrium test all`): format/mount accounting, format safety, tree and error paths, file I/O and growth through the indirect block, maximum file size, disk-full and space reuse, metadata exhaustion, directory growth, rename/busy/handle limits, persistence and unclean mounts, read-only, four block sizes, 14 kinds of crafted corruption, orphan tolerance, injected device failures at every point of a create/write/rename/remove sequence, growth rollback and fault handling, and device failure. Not covered: real power loss, concurrent access (the kernel is single-CPU), and failures between the individual sector writes of one block other than through the injection hook.
