# UltriumFS 2.0 on-disk format (format version 2)

Status: **specification only**. Nothing in this document is implemented yet; the implementing pull requests are listed at the end and each states what it actually delivers. The existing v1 filesystem ([ultriumfs-format.md](ultriumfs-format.md)) is unchanged by this document.

UltriumFS 2.0 is an **extent-based filesystem with write-ahead metadata journaling, ordered file-data writes, checksummed metadata and conservative recovery**. It does **not** provide data journaling, data checksums, snapshots, compression, encryption, sparse files or truncation; those are reserved extension points (see "Extension points"), not features.

## 1. Conventions

- All integers are **little-endian** and encoded/decoded byte by byte; no C structure is ever written to disk. Offsets below are byte offsets within the structure.
- **Filesystem block**: 4096 bytes. The device logical block size must divide 4096 (512, 1024, 2048 or 4096). All "block numbers" are filesystem blocks (`u32`).
- "Zero" means every byte of a reserved field must be 0 on write and is rejected otherwise by mount and the checker.
- **Checksum**: CRC-32C (Castagnoli, polynomial 0x1EDC6F41, reflected form 0x82F63B78, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF). The algorithm is part of the format version; changing it requires a new format version.
- **Contextual checksums.** A checksum is computed over a *context prefix* followed by the structure bytes up to (excluding) the checksum field, so a structure copied to the wrong place or taken from another filesystem fails verification:

| Structure | Context prefix (before the covered bytes) |
| --- | --- |
| Superblock copy | block number `u64` |
| Bitmap block, directory block, extent-leaf block | UUID (16 bytes) ‖ block number `u64` |
| Inode | UUID (16 bytes) ‖ inode number `u32` |
| Journal descriptor / commit block | UUID (16 bytes) ‖ journal ring position `u32` |

## 2. Volume geometry

```text
block 0                  superblock copy A
blocks 1 .. J            journal ring (J = journal_blocks)
blocks 1+J ..            block bitmap (BB blocks)
...                      inode table (IB blocks)
...                      data area (data_start .. total_blocks-2)
block total_blocks - 1   superblock copy B
```

- `total_blocks = min(device_blocks / (4096 / device_block_size), 2^20)`. **This is the format rule that lets copy B be found without reading copy A:** its location is derived from the device capacity alone. A volume is therefore at most **4 GiB**; a larger device uses only its first 4 GiB. Minimum volume: **512 blocks (2 MiB)**.
- `inode_count = clamp(total_blocks / 4, 64, 65536)` rounded up to a multiple of 16; `inode_blocks = inode_count / 16`.
- `bitmap_blocks = ceil(total_blocks / 32704)` (see section 6).
- `max_txn_blocks = 12 + bitmap_blocks` (worst-case metadata blocks of one operation; see section 8.5), at most 45.
- `journal_blocks J = clamp(total_blocks / 64, 4 × (max_txn_blocks + 2), 4096)`.
- Metadata is laid out in the order above with no gaps; every block in `[0, data_start)` and block `total_blocks - 1` is marked used in the bitmap.
- Online resizing is **not supported**. A volume whose superblock `total_blocks` differs from the rule above (for example an image later grown) still mounts through copy A, but copy B can then only be found from `total_blocks`, so redundancy recovery is documented as unavailable in that case.

## 3. Superblock

Two copies, A at block 0 and B at block `total_blocks - 1`. Only the first 256 bytes are used; the rest of the block is zero and not covered by the checksum.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `ULTRIUM2` |
| 8 | 4 | format version = 2 |
| 12 | 4 | block size = 4096 |
| 16 | 8 | **generation** (increments on every superblock write) |
| 24 | 8 | total_blocks |
| 32 | 4 | compatible feature flags |
| 36 | 4 | read-only-compatible feature flags |
| 40 | 4 | incompatible feature flags |
| 44 | 4 | state: 0 = cleanly unmounted, 1 = mounted read-write |
| 48 | 16 | filesystem UUID |
| 64 | 32 | volume label (printable ASCII, NUL padded) |
| 96 | 4 | journal_start (= 1) |
| 100 | 4 | journal_blocks |
| 104 | 4 | max_txn_blocks |
| 108 | 4 | bitmap_start |
| 112 | 4 | bitmap_blocks |
| 116 | 4 | inode_start |
| 120 | 4 | inode_count |
| 124 | 4 | inode_blocks |
| 128 | 4 | data_start |
| 132 | 4 | root_inode (= 1) |
| 136 | 4 | inode_size (= 256) |
| 140 | 4 | dirent_size (= 64) |
| 144 | 8 | **journal_tail_seq**: sequence number of the oldest transaction not yet checkpointed (equivalently, the next transaction expected) |
| 152 | 4 | **journal_tail_pos**: ring position (0 .. J-1) where that transaction starts |
| 156 | 96 | reserved, zero |
| 252 | 4 | CRC-32C (context: block number) over bytes 0..251 |

### 3.1 Feature flags and compatibility policy

Defined bits (all other bits are undefined and reserved):

| Set | Bit | Name | Meaning |
| --- | --- | --- | --- |
| incompatible | 0 | `EXTENTS` | files use extent maps (always set in version 2) |
| incompatible | 1 | `JOURNAL` | write-ahead metadata journal (always set in version 2) |
| read-only-compat | – | none defined | |
| compatible | – | none defined | |

Mount behaviour: an **unknown incompatible bit refuses the mount** (even read-only); an **unknown read-only-compatible bit forces a read-only mount**; unknown compatible bits are ignored. A feature that a mounted implementation does not understand is never silently ignored.

### 3.2 Redundancy and update protocol

Copies are written **alternately ("ping-pong")**, never both at once. An update writes the new state to the copy *not* currently selected, with `generation + 1`, then flushes. A write interrupted at any point can therefore damage only the older copy; the newest valid copy is untouched.

**Selection at mount** (`superblock_select`): read A (block 0) and B (block `min(device_blocks/per, 2^20) - 1`). A copy is *valid* if its magic, version, block size and checksum are correct and its geometry passes the sanity checks of section 11. Then:

1. Both valid: they **must agree on every static field** (everything except generation, state, journal_tail_seq, journal_tail_pos and the checksum); disagreement is corruption and refuses the mount. Select the **higher generation**. Equal generations with different contents are corruption.
2. One valid: select it. The mount may proceed read-write; the first superblock update naturally overwrites the damaged copy. The condition is reported, and `check --repair` rewrites the damaged copy from the valid one (provably safe because the valid copy is self-checking and current).
3. Neither valid: the volume is unmountable and unrepairable by this format (no third copy exists). The checker reports "both superblock copies damaged" and changes nothing.

For a copy B that is valid but has the *lower* generation, mount treats it as the stale copy: it is not an error (the normal state after every update is that the two copies differ by one generation).

**Invariant S1.** A superblock update that changes `journal_tail_*` must reach stable storage (flush completed) **before** any journal ring space it frees is reused. This is what makes the two-copy scheme safe with the journal.

## 4. Journal

A ring of `J` blocks starting at `journal_start`; ring position `p` is filesystem block `journal_start + p`. The journal holds **complete images of metadata blocks** (physical block logging), not operation records.

### 4.1 Transaction layout

A transaction with `n` payload blocks (1 ≤ n ≤ `max_txn_blocks`) occupies `n + 2` consecutive ring positions, wrapping modulo `J`:

```text
position t       descriptor block
positions t+1..  n payload blocks (verbatim images of the home blocks)
position t+n+1   commit block
```

**Descriptor block**

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `UJD2` |
| 4 | 4 | n |
| 8 | 8 | sequence number |
| 16 | 8 | reserved, zero |
| 24 | 4 × n | target block numbers (home locations) |
| … | | zero padding |
| 4092 | 4 | CRC-32C (context: UUID, ring position) over bytes 0..4091 |

**Commit block**

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `UJC2` |
| 4 | 4 | n |
| 8 | 8 | sequence number |
| 16 | 4 | payload CRC-32C: over UUID ‖ sequence ‖ the whole descriptor block ‖ all n payload blocks |
| 20 | 4072 | reserved, zero |
| 4092 | 4 | CRC-32C (context: UUID, ring position) over bytes 0..4091 |

Sequence numbers are `u64`, start at 1 at format time and increase by exactly 1 per committed transaction; overflow is refused.

### 4.2 Transaction states

| # | State | What has happened | Visible as committed? |
| --- | --- | --- | --- |
| 1 | Prepared | Metadata block images exist in memory only | no |
| 2 | Journal data written | descriptor and payload blocks written to the ring | no |
| 3 | Journal data flushed | flush completed for those writes | no |
| 4 | Committed | commit block written **and flushed** | **yes**: recovery will apply it |
| 5 | Applied | images written to their home locations | yes |
| 6 | Home flushed | flush completed for the home writes | yes |
| 7 | Checkpointed | superblock with the advanced tail written and flushed | yes; journal space reusable |

A transaction becomes visible only at state 4; the commit block is written strictly after the payload flush. The commit write is a single block (atomic at sector granularity in practice, and protected by its own checksum plus the payload checksum), so a torn commit is simply an invalid commit.

### 4.3 Checkpointing policy (v2.0)

**Every transaction is checkpointed before the operation that caused it returns** (states 5–7 are executed synchronously). Consequently at most one transaction is ever live in the journal, journal exhaustion cannot occur from accumulation, and the replay of an old transaction can never overwrite a block that a later transaction reallocated.

Lazy checkpointing (several live transactions) is **reserved** and would additionally require *revoke records*: a directory or extent-leaf block logged in transaction N, freed and reallocated as file data in N+1 would otherwise be overwritten with the stale image at replay. The format reserves the descriptor `flags` field for this; version 2 implementations must not batch checkpoints.

### 4.4 Journal space rules

- A transaction needs `n + 2` free positions. With synchronous checkpointing the journal is empty between transactions, so the only rejection is `n > max_txn_blocks` or `n + 2 > J`, which is reported as *transaction too large* **before any change is made**.
- Position arithmetic is done in 64 bits and checked; ring positions never wrap silently.
- **Invariant J1:** ring positions occupied by a transaction that is not yet checkpointed are never overwritten. **J2:** a ring position is reused only after the superblock recording the advanced tail is flushed (S1). **J3:** the head is always derived from the tail by replay; it is not stored.

### 4.5 Replay (mount, before any write)

1. Select the superblock (3.2). Take `tail_pos`, `tail_seq`.
2. At `pos = tail_pos`, `expected = tail_seq`, repeat at most `J / 3 + 1` times:
   1. Read the descriptor. Stop if its magic or checksum is wrong, `sequence ≠ expected`, `n` is 0 or exceeds `max_txn_blocks`, or `n + 2` exceeds `J`.
   2. Validate every target: it must be `< total_blocks`, **outside the journal region, not a superblock copy**, and not already listed in this transaction. A malformed descriptor ends the scan (it is an incomplete or damaged tail, reported as such if non-empty).
   3. Read the n payload blocks and the commit block. Stop if the commit magic/checksum is wrong, its `n`/sequence differ, or the payload CRC does not match.
   4. The transaction is **complete and committed**: write each payload image to its target. `pos += n + 2 (mod J)`, `expected += 1`.
3. Flush the home writes. Write a superblock with `tail_pos = pos`, `tail_seq = expected` (copy rule 3.2) and flush. The next transaction uses `seq = expected` at `pos`.
4. An incomplete transaction (descriptor without a valid commit) is **ignored**; its blocks are overwritten by the next transaction, whose payload CRC cannot match stale blocks.

Replay is **idempotent** (it rewrites identical images; a crash during replay restarts from the unchanged tail) and uses bounded memory (one transaction's blocks) and bounded iteration. After replay the full verification of section 11 runs before writes are enabled.

## 5. Inode

256-byte slots; 16 per block; inode numbers start at 1 (**root = 1**, 0 is invalid). Inode `i` is at block `inode_start + (i-1)/16`, offset `((i-1) % 16) × 256`.

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | type: 0 free, 1 file, 2 directory |
| 1 | 1 | reserved, zero |
| 2 | 1 | extent map depth: 0 or 1 (2 and above are reserved and refused) |
| 3 | 1 | reserved, zero |
| 4 | 4 | **generation** |
| 8 | 8 | size in bytes (directories: allocated blocks × 4096) |
| 16 | 4 | parent inode (the containing directory; the root's parent is 0) |
| 20 | 4 | inode feature flags: must be 0 in version 2 (future: compression, encryption, …) |
| 24 | 4 | extent_count: total extents in the file |
| 28 | 4 | block_count: data blocks plus extent-leaf blocks owned |
| 32 | 8 | txn_seq: sequence of the last transaction that modified this inode |
| 40 | 2 | root_entries: used slots in the root array |
| 42 | 22 | reserved, zero (room for a future attribute reference) |
| 64 | 176 | root array: 11 slots × 16 bytes |
| 240 | 12 | reserved, zero |
| 252 | 4 | CRC-32C (context: UUID, inode number) over bytes 0..251 |

No timestamps, owner, mode or link count exist in version 2: none has defined semantics yet.

**Free and pristine inodes.** A slot that is **entirely zero** is *pristine* (never used, generation 0). A **freed** inode has `type = 0`, **its generation preserved**, every other field zero and a valid checksum. **Allocation increments the generation** (a pristine slot becomes generation 1); an inode whose generation is `0xFFFFFFFF` is retired and never reallocated. Directory entries carry the generation, so a dangling entry that points to a reused inode number is detected.

### 5.1 Extent map

An **extent** is 16 bytes: `logical u32` (first file block), `physical u32` (first filesystem block), `length u32` (blocks, 1 ≤ length ≤ 2^20), `flags u32` (must be 0 in version 2; reserved for unwritten, shared and compressed extents).

- **depth 0:** the root array holds up to **11 extents** directly.
- **depth 1:** each root slot is an *index entry* `{first_logical u32, leaf_block u32, leaf_extents u32, reserved u32 = 0}`; each leaf block holds up to **253 extents**. Maximum **11 × 253 = 2783 extents**.
- Promotion from depth 0 to 1 allocates one leaf and moves the inline extents into it (one transaction).

**Leaf block**

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `EXTL` |
| 4 | 4 | owner inode |
| 8 | 4 | owner generation |
| 12 | 2 | entries |
| 14 | 2 | depth of this node = 0 |
| 16 | 4 | first_logical |
| 20 | 12 | reserved, zero |
| 32 | 16 × 253 | extents |
| 4080 | 12 | zero |
| 4092 | 4 | CRC-32C (context: UUID, block number) over bytes 0..4091 |

**Append-only invariant.** Files only grow at the end or are deleted whole (no truncate, no sparse files, no hole punching), so extent maps are append-only. This keeps the structure and its journaling simple and is a stated limitation, not an accident.

**Invariants (checked at mount and by `check`):**

- **E1** extents are in logical order, contiguous from 0 with no gap or overlap; index entries likewise.
- **E2** the sum of extent lengths equals `ceil(size / 4096)`; `extent_count` equals the number of extents; `block_count` equals data blocks plus leaf blocks.
- **E3** every extent lies entirely in `[data_start, total_blocks - 1)`; `physical + length` is computed in 64 bits.
- **E4** **no physical block is owned by two extents or leaves** (duplicate ownership is corruption); the writer merges an appended run into the last extent when physically adjacent.
- **E5** leaf header owner/generation/first_logical agree with the inode and index entry; leaf entry counts agree with `leaf_extents`.
- **E6** flags and all reserved fields are zero.

**Size targets.** A file can use any number of blocks up to free space, bounded by the extent limit. A file is guaranteed to fit regardless of fragmentation up to **2783 blocks (about 10.9 MiB)**; contiguous or lightly fragmented files can reach the volume size (4 GiB). The tested target is a multi-tens-of-MiB file through the VFS; larger sizes are not promised until exercised.

## 6. Block bitmap

**Choice: an allocation bitmap**, one bit per filesystem block (1 = used). Evaluated against the alternatives:

| Design | Journaling | Recovery / check | Memory | Fragmentation | Verdict |
| --- | --- | --- | --- | --- | --- |
| **Bitmap** | a touched bitmap block is logged whole; an allocation touches 1–2 blocks | bitmap is independently re-derivable from extents; mismatches are detectable and repairable | `total/8` ≤ 128 KiB (kept in memory at mount) | no extra structure | **chosen** |
| Free-extent list | variable-length records; splitting/merging touches several blocks | harder to validate and rebuild | grows with fragmentation | tracks runs natively | rejected: unbounded update size |
| Free-extent tree | balanced-tree updates inside every transaction | complex | moderate | best | rejected: too much journaled complexity for a first version |

**Bitmap block.** Block `k` of the bitmap covers filesystem blocks `[k × 32704, (k+1) × 32704)`: bytes 0..4087 hold **32704 bits** (bit `b` is byte `b/8`, bit `b%8`, least significant first), bytes 4088..4091 are zero, bytes 4092..4095 are a CRC-32C (context: UUID, block number) over bytes 0..4091.

**Invariants:** **B1** bit `b` is 1 iff block `b` is owned: superblock copies, journal, bitmap, inode table, an extent of an allocated inode, or an extent-leaf block. **B2** bits past `total_blocks` in the last bitmap block are 1. **B3** the in-memory bitmap loaded at mount equals the on-disk bitmap, and (checked at mount and by `check`) equals the set derived from all inodes' extents.

Allocation: next-fit over the in-memory bitmap; a growing file first tries to extend its last extent in place, then takes the first free run large enough, then the largest available runs (several extents) until satisfied. Every change to the bitmap is part of the same transaction as the inode change that owns the blocks.

## 7. Directories

A directory is an inode of type 2 whose blocks are mapped by its extent map and hold fixed entries.

**Directory block** (4096 bytes): 63 entries of 64 bytes (bytes 0..4031), then a footer:

| Offset | Size | Field |
| --- | --- | --- |
| 4032 | 4 | magic `DIRB` |
| 4036 | 4 | owner inode |
| 4040 | 4 | owner generation |
| 4044 | 4 | logical block index within the directory |
| 4048 | 44 | reserved, zero |
| 4092 | 4 | CRC-32C (context: UUID, block number) over bytes 0..4091 |

**Entry** (64 bytes)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | inode number (0 = free slot; the whole entry is then zero) |
| 4 | 4 | inode generation |
| 8 | 1 | type (1 file, 2 directory), must equal the inode's type |
| 9 | 1 | name length 1..31 |
| 10 | 2 | reserved, zero |
| 12 | 52 | name bytes, NUL padded |

- **Names:** 1–31 bytes, any byte except `/`, control characters (< 0x20) and 0x7F; compared bytewise (no case folding, no Unicode normalization). 31 is the VFS limit; the entry has room for 52, which a later version may allow. Names longer than 31 are corruption in version 2.
- There are no `.` or `..` entries; the VFS handles them lexically. The `parent` field of an inode provides the upward relation.
- **Expansion:** a directory grows by appending one block (zeroed except the footer) when it has no free slot. **Deletion** zeroes the entry; blocks are not reclaimed until the directory is removed. **Rename** within a directory rewrites one entry; across directories it clears one entry, writes one entry, and updates the moved inode's `parent` — all in one transaction.
- **Invariants:** **D1** an entry's `(inode, generation, type)` matches an allocated inode; **D2** every non-root allocated inode is referenced by **at most one** entry (no hard links), and by exactly one unless it is an orphan; the root is referenced by none; **D3** a referenced inode's `parent` equals the containing directory; **D4** names are unique within a directory; **D5** a directory has ≥ 1 block and `size = blocks × 4096`; **D6** footer owner/generation/index match; **D7** following `parent` from any directory reaches the root within **64 steps** (**no cycles**; the VFS path limit bounds legitimate depth at 63); creating or moving a directory refuses a destination that is the directory itself or inside it (checked by walking `parent` from the destination to the root).
- **Depth** is bounded by the VFS path limit (127 characters), so at most 63 levels.

**Handling of damage.** A dangling entry (free inode, wrong generation or type), duplicate reference, bad footer or bad name is **corruption**: mount refuses to enable writes and the checker reports it. *Orphans* (allocated, unreferenced, non-root inodes, which an interrupted-and-recovered operation can no longer produce under this format but which can arise from damage) keep their blocks, are counted and reported, and are **not** deleted automatically.

## 8. Operations and atomicity

### 8.1 Rule

Each operation below is **one transaction** (all-or-nothing after crash recovery). Home-location writes use the journal protocol of section 4; free-space and inode changes are part of the same transaction as the structure they belong to.

| Operation | Metadata blocks in the transaction |
| --- | --- |
| create file | new inode block, directory block (+ if the directory is full: directory inode block, bitmap block(s), new directory block, leaf block) |
| mkdir | as create, plus the new directory's first block and bitmap block(s) |
| unlink file / rmdir | parent directory block, child inode block, bitmap blocks of every freed extent (≤ `bitmap_blocks`); freed extent-leaf blocks need no image |
| rename (same or different directory, file or directory) | source and destination directory blocks, moved inode block (parent update), (+ directory extension as above) |
| file growth chunk | inode block (size, extents, block_count), bitmap block(s), extent-leaf block (or its promotion) |

### 8.2 Splitting rule

A single write call is processed in **chunks of at most 64 new data blocks and 3 new extents**; each chunk is its own transaction and its data is made durable first (section 9). After a crash mid-write the file has the size of the last committed chunk. A write is **not** atomic as a whole; it is atomic per chunk. A *remove of a tree* (`remove -r`) is a sequence of independent single-entry transactions, as it already is.

### 8.3 Pre-checks

An operation computes an upper bound of the blocks its transaction will contain **before changing anything**; if it exceeds `max_txn_blocks` it fails with *transaction too large* (no partial effect). With the limits of section 2 no defined operation exceeds the bound, so this is a safety net and a tested path.

### 8.4 Free-space and metadata exhaustion

Out of free blocks, out of inodes, extent limit reached or directory limit reached all return `no space` **before any journal or home write** of that transaction; in-memory allocations made while planning are released. A chunked write that runs out of space in a later chunk leaves the earlier committed chunks.

### 8.5 Transaction bound

`max_txn_blocks = 12 + bitmap_blocks`. Worst cases: create-with-directory-extension 7, mkdir 8, rename across directories with extension ≤ 12, growth chunk ≤ 6, unlink of a maximally scattered file ≤ 3 + `bitmap_blocks`. The journal is at least `4 × (max_txn_blocks + 2)` blocks.

## 9. Ordered data and flush semantics

**New data (blocks that committed metadata will reference).** Strict order:

1. write the file data to the newly allocated blocks;
2. **flush** (data durable);
3. write the journal descriptor and payload; 4. **flush**;
5. write the commit block; 6. **flush** (transaction committed);
7. apply to home locations; 8. **flush**;
9. checkpoint: write the other superblock copy with the advanced tail; **flush**.

Consequence: committed metadata never references data that is not durable, and (steps 3–6) a crash before step 6 leaves the old state intact.

**Overwriting already-allocated blocks** (rewriting existing file bytes) is done **in place with no journal and no atomicity**. Metadata journaling does not make such overwrites atomic: after a crash the affected bytes may contain the old data, the new data, or a block torn at device-sector granularity. Applications needing atomic updates must write a new file and rename it (rename is atomic). A future copy-on-write or data-journaling feature would be a new incompatible flag.

**Flush requirement.** Writable mounting requires a device that supports a reliable flush (`BLOCK_FLAG_FLUSH`): without it none of the ordering above is meaningful. A device without flush, a read-only device, or one marked dead is mounted **read-only** and the reason is reported; the kernel never claims power-failure-safe commits it cannot provide. A flush that fails is an I/O error and faults the volume read-only.

## 10. Mount states and policy

| Condition | Result |
| --- | --- |
| Valid v2, journal empty or replayable, verification passes, flush-capable writable device | read-write; superblock `state` set to 1 (and cleared at clean unmount/sync) |
| Journal replay needed and device writable | replay (4.5), verify, mount read-write |
| Replay needed but device read-only or no flush | refused: *recovery required* (never replay on a device that cannot be written safely) |
| Verification finds only **repairable** problems (bitmap mismatch, stale `state`, one damaged superblock copy) | mounted **read-only**, reported as *repair recommended*; `check --repair` fixes them |
| Verification finds structural corruption | **refused**; nothing is modified; `check` explains |
| Unknown incompatible feature, unsupported version | refused |
| Unknown read-only-compatible feature | read-only |
| v1 volume | **read-only** through the v1 driver; **never written**, not even a dirty mark (section 12) |
| Not UltriumFS | refused; never formatted automatically |

Formatting remains explicit, confirmed, restricted to writable, unmounted, scratch-flagged devices. **Writes during replay and repair obey the same flush ordering.**

## 11. Validation and the checker

`mount` and `ultrium storage check` share one verification pass, bounded by `inode_count`, total extents, directory blocks and `total_blocks`; it never recurses (explicit loops only) and uses fixed buffers. It validates, in order: superblock selection and geometry (regions in order, inside the volume, sizes consistent with the rules of section 2); journal state (4.5 in read-only mode); every inode (checksum, reserved fields, type, generation, extent invariants E1–E6, duplicate physical ownership across the whole volume with a derived ownership bitmap); every directory block (footer, entries, D1–D7); the derived ownership bitmap against the on-disk bitmap (B1–B3); counts.

**Findings** carry a class: *repairable* (bitmap mismatch, stale state, damaged/stale superblock copy, committed-but-unapplied journal), *orphan* (reported only), *structural* (checksum failure, invalid or duplicate extents, dangling/duplicate/cyclic directory references, unsupported features, both superblocks damaged).

**Repair** (`check <device> --repair`) is explicit, refused on a mounted or read-only device, and limited to: replaying validated committed transactions; rewriting a damaged or stale superblock copy from the valid one; rebuilding the bitmap from extent ownership **only when ownership is unambiguous** (no duplicate owners, no out-of-range extents); clearing a stale `state` after a clean verification. It never deletes, truncates, moves or invents files, never touches directory structure, and writes through the same journaled, flush-ordered path. The report lists problems found, changes proposed, changes made, remaining problems and whether a writable mount is now safe; *partial* repair is reported as partial. A checksum detects corruption; it does not repair it.

## 12. Compatibility and migration

- **Detection:** block 0 begins with `ULTRIUMF` (v1; version field 1) or `ULTRIUM2` (v2). v2 is also searched at the end of the device through copy B. Anything else is "not UltriumFS".
- **v1 volumes** keep working through the existing driver **read-only**: the shell mounts them read-only automatically, the kernel never writes them (no dirty mark, no repair) and a test requires a v1 image to be byte-identical after a mount, reads and unmount. v1 formatting remains reachable **only through kernel test APIs**; `ultrium storage format` creates v2.
- **No automatic or in-place upgrade.** The supported migration is to mount the v1 volume read-only, create a separate v2 volume on another scratch disk, and copy the files with the shell (`copy`; recursive directory copy is not implemented, so a tree is recreated with `new folder` and `copy`). A host-side converter is a possible later tool; in-place conversion is out of scope until a reviewed, recoverable transaction design exists.
- v1 and v2 are separate format versions with different magics; neither driver ever reinterprets the other's structures.
- Only one UltriumFS volume (v1 or v2) is mounted at a time, as today.

## 13. Extension points (not features)

Reserved and rejected-if-nonzero in version 2: inode `flags` and the 22 reserved inode bytes; extent `flags`; the descriptor `flags` (revoke records); the 96 reserved superblock bytes; extent-map depth ≥ 2; directory-entry names beyond 31 bytes. Feature flag bits are allocated per kind (compatible / read-only-compatible / incompatible) so a later feature states how an old implementation must treat it. **Snapshots, compression, encryption, sparse files, data checksums, reflinks, deduplication, online resizing and file-level metadata for other services are not implemented and must not be advertised.**

## 14. Limits summary

| Item | Value |
| --- | --- |
| Block size | 4096 (device block 512–4096, must divide 4096) |
| Volume | 2 MiB – 4 GiB (2^20 blocks) |
| Inodes | 64 – 65536 |
| Extents per file | 2783 (11 inline at depth 0) |
| Guaranteed file size at worst fragmentation | 2783 blocks (≈ 10.9 MiB); up to the volume size when contiguous |
| Filename | 1–31 bytes |
| Transaction | ≤ `max_txn_blocks` (≤ 45) payload blocks + 2 |
| Journal | 4 × (`max_txn_blocks` + 2) … 4096 blocks (≈ 16 MiB) |
| Memory at mount | bitmap ≤ 128 KiB, inode state ≤ 64 KiB, transaction staging ≤ 64 blocks (256 KiB) |
| Open handles | 16 files, 8 directory iterators; one mounted volume |
| UUID | 16 bytes. The kernel has no random source: it derives one from the time-stamp counter, the device serial and a counter, so it is **unique, not random or secret**; tests pass a fixed UUID |

## 15. Test model (design)

Journal and recovery correctness cannot be shown with failure-injecting devices that never lose data. The test plan therefore requires a **memory block device with a simulated volatile write cache**: writes stay *pending* until a flush; an injected power cut at a chosen I/O event discards **none, all, a prefix or a seeded random subset** of the pending writes (and may tear the write in flight at 512-byte granularity). With it the recovery matrix enumerates *every* I/O event of an operation sequence, power-cycles, remounts (which replays), checks all invariants with the shared verifier, and requires the result to equal the old or the new committed state. A **missing flush between payload and commit must make a test fail**, and this is demonstrated by a negative control. Separately, a smaller set of **QEMU runs stop at named phases** (the guest stops accepting I/O at the chosen point and the harness kills QEMU); these exercise real boots and the real driver but, because the host page cache survives, they are *not* volatile-cache-loss tests and are described as such. An independent host-side reader (`ufs_image.py`) verifies images without using kernel code. Physical power loss is never tested.

## 16. Implementation plan (stacked draft PRs)

1. **`feature/ultriumfs-v2-format`**: this specification (documentation only).
2. `feature/ultriumfs-journal`: CRC-32C, superblock pair, volatile-cache test device, journal engine (commit/replay/checkpoint) with its own tests.
3. `feature/ultriumfs-v2-core`: bitmap, inodes, extents, directories, operations as transactions, ordered data, VFS backend, flush-policy and virtio audit.
4. `feature/ultriumfs-checker`: shared verification, `ultrium storage check [--repair]`, mount policy states, v1 read-only policy and tests.
5. `feature/ultriumfs-crash-tests`: recovery matrix, QEMU phase runs, v2 host reader, negative controls, persistence tests for v2 and v1, CI.

Checksums (milestone five) are not a separate layer: CRC-32C is introduced in PR 2 and applied to every structure as it is introduced.
