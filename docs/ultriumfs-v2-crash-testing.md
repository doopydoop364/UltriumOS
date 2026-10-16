# UltriumFS 2: crash-recovery testing and recovery guarantees

Status: implemented. **QEMU-tested** (in-kernel recovery matrix, virtio-disk power cuts, abrupt QEMU stop) and independently checked by the host-side reader `tools/ufs2_image.py`. **Not tested on physical hardware.** No physical power-loss reliability is claimed. Format: [ultriumfs-v2-format.md](ultriumfs-v2-format.md); journal layer: [ultriumfs-v2-journal.md](ultriumfs-v2-journal.md); checker and repair: [ultriumfs-v2-recovery.md](ultriumfs-v2-recovery.md).

## Three kinds of interruption (kept separate)

| Kind | What it is | What it models | What it does **not** show |
| --- | --- | --- | --- |
| **Simulated volatile-cache loss** (`ultrium test ufs2crash`) | A memory block device with a write cache. Power is cut before a chosen I/O event; pending (unflushed) writes are then kept, dropped, cut to a prefix or suffix (reordering), randomly subset, or torn mid-block. | A device whose cache loses or reorders writes that were not flushed, within the documented model. | Real device behaviour, firmware bugs, writes that are lost *despite* a completed flush. |
| **Deterministic cut on a real virtio disk** (`qemu-ufs2-recovery-test.py`) | `block_arm_power_cut(phase, n)`: from the n-th entry of a named journal phase every transfer and flush fails, so the image file holds exactly the writes that completed before. | Power lost at a phase boundary with everything earlier durable. | Cache loss or reordering. |
| **Abrupt QEMU termination** (persistence boot 4; mid-stream kill) | QEMU is stopped by signal. | The guest stopping without a shutdown. Completed guest writes stay in the host page cache. | Power loss: the host did not lose power. |
| **Physical power loss** | – | – | **Not tested.** |

## What is verified after every interruption

The volume is remounted (replaying the journal) and must:

1. mount **read-write** with `repair_recommended` clear;
2. verify with **zero findings of any class**: no leaked or doubly owned block, no unmarked owned block, no orphan, no damaged inode, directory, extent or bitmap checksum;
3. agree on allocation accounting: `total - free` equals the blocks the inodes own;
4. have a whole-tree digest (every path, type, size, content CRC-32C; unrelated files included) equal to an **allowed state** (below), and equal to the final state when the operation had already returned success;
5. remain usable: create and remove a file, still verifying clean.

## Allowed results per operation

Each VFS call is a list of *steps*; a step is one journal transaction unless noted.

| Operation | Atomic? | Allowed result after a cut |
| --- | --- | --- |
| create file, mkdir, unlink, rmdir | yes (1 transaction) | tree before **or** after |
| rename in place / across directories / a directory / into a full directory (directory grows by a block) | yes | before or after; never both names, neither name or a lost file |
| create in a full directory (directory extension) | yes | before or after |
| unlink of a fragmented file (many extents freed) | yes | before or after |
| unlink then create (inode reuse, new generation) | no, 2 steps | before the first, between, or after |
| unlink, create, write 11 blocks into the freed blocks | no, 3 steps (the write is chunked) | any step boundary; the new file may be any committed prefix of the written data |
| file growth, extent-map promotion (inline extents to a leaf block) | per chunk | the old file or any chunk boundary up to the full size; **every byte below the committed size is the data that was written** (ordered data); unrelated files unchanged |
| sync | no change visible | unchanged tree |
| unmount | superblock state clean/mounted | unchanged tree; the next mount reports an unclean session when the clean mark was lost |
| mount (marks the superblock mounted, replays) | – | unchanged tree |
| cut **during journal replay** at every I/O event of the recovery mount | – | replay is idempotent: the next mount ends in the same allowed state |

A multi-transaction operation such as a large write is **not** atomic as a whole, and an incomplete one is not corruption: the file simply has the size of its last committed chunk. In-place overwrites of bytes inside the current file size are not journaled and not atomic (documented in `ultriumfs-v2-format.md`); they are outside this matrix.

## Test inventory

`ultrium test ufs2crash` (12 cases, **not** part of `ultrium test all` because it takes several minutes; about 6,500 interrupted-and-recovered runs, printed as `CRASH MATRIX RUNS`):

1. create, mkdir, unlink, rmdir
2. renames: in place, across directories, a directory, into a full directory
3. directory extension, fragmented unlink
4. chunked growth over two chunks and extent-map promotion (11 to 12 extents)
5. inode reuse and block reuse
6. create + sync + unmount (superblock updates)
7. interrupted mount
8. journal ring wrap-around, head at the last ring block (every transaction straddles the wrap)
9. journal ring wrap-around, head mid-ring
10. power cut during journal replay (committed transaction left by a first cut, recovery interrupted at every event under three more policies)
11. power cut during replay across the ring end
12. power cut during replay of growth and block reuse

Each operation is cut before **every** write and flush event it performs, under eight loss policies (keep all, lose all, keep a 2-write prefix, torn block, two random subsets, keep the last 1 or 3 writes). Growth operations use five policies. Replay cuts use two first-cut and three recovery-cut policies.

`python3 tools/qemu-ufs2-recovery-test.py` (QEMU, virtio disk, 128 MiB image under `test-images/`): builds a fixture (6 MiB file, fragmented file, nested folders, renames, block reuse), then for each of five cuts (`data-flush`, `journal-write`, `commit-write`, `home-write`, `checkpoint`) copies the image, cuts power during one operation, stops QEMU, has the **host decode the image without the kernel** and require the exact expected old/new tree and a clean independent check, boots the kernel (replay), shuts down cleanly and requires the host-predicted tree again. A final stage kills QEMU during a 40 MiB write and requires a valid prefix of the written data, unrelated files unchanged, and a clean host check before and after replay. Expected host results: commit not reached = old tree with 0 pending transactions and the transaction **not** replayed; commit reached = new tree with 1 pending transaction replayed.

`python3 tools/ufs2-host-negative-controls.py`: eight deliberate faults on a copy of the fixture (flipped file byte, used block cleared / free block marked in the bitmap with a fixed-up checksum, bitmap bit without checksum fix, inode field, superblock copy A, both copies, directory name) must all be detected by the host reader; the original image must stay unchanged.

The existing five-boot persistence test and the v1 read-only compatibility test are unchanged (`docs/persistence-testing.md`).

## Negative controls on the kernel (temporary mutations, restored afterwards)

| Deliberate defect | Result |
| --- | --- |
| no flush after the commit record | matrix fails (`home-write`, keep-last-1: findings after recovery) |
| no flush after the journal payload | matrix fails (`commit-flush`, random: volume does not mount) |
| no flush after the home-location writes | matrix fails (`sb-flush`: findings / does not mount) |
| freed blocks never cleared in the on-disk bitmap | matrix fails (volume does not mount read-write after an unlink) |
| data of newly allocated blocks never written | matrix fails (`committed file data is not the data that was written`) |
| **no flush of file data before the metadata transaction** | **not detected, by design**: the journal-payload flush that follows makes the data durable before the commit record, so the explicit data flush is redundant for ordering. It is kept as the documented ordering. |

## Findings

- The recovery matrix found **no filesystem or journal bug**: all operations recover to an allowed state under all modelled interruptions. No on-disk format change was needed.
- One test-model error was found while writing the matrix: a write into freed, fragmented space is chunked into several transactions, so intermediate sizes are valid (not corruption). The check was generalised to "a committed prefix of the written data", not relaxed; the ordered-data content check still applies to every surviving byte.
- Removing the explicit data flush before a metadata transaction is not detectable (see the negative-control table); it is redundant given the journal-payload flush, as already noted in `ultriumfs-v2-journal.md`.

## Known limitations

- Single CPU, no concurrency. Write-through, one live transaction at a time, synchronous checkpoint.
- Overwrites in place inside the file size are not atomic and not journaled; data of an interrupted chunk beyond the committed size is unreferenced.
- The device model assumes a flush makes all earlier writes durable. A device that acknowledges a flush without doing so is outside the model.
- Torn writes are modelled at 512-byte granularity inside one block; metadata and journal blocks are checksummed, file data is not.
- Not run: physical power loss, real SSD/HDD behaviour, other hypervisors, disks larger than the 128 MiB test image in QEMU.
- `ufs2crash` takes about six minutes in QEMU TCG.

## Maintainer commands

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py 'ultrium test all'          # 103 cases, twice
python3 tools/qemu-memory-test.py 'ultrium test ufs2crash'    # recovery matrix, once, ~6 min
python3 tools/qemu-ufs2-recovery-test.py                      # virtio cuts + host verification, ~4 min
python3 tools/ufs2-host-negative-controls.py                  # needs the image left by the previous command
python3 tools/qemu-persistence-test.py && python3 tools/qemu-persistence-test.py --v1
```
