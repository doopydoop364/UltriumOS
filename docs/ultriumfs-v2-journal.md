# UltriumFS 2: superblock pair and journal layer (implementation notes)

Status: implemented and QEMU-tested **as a layer**. There are still no files, directories or extents in v2 (next PR); this layer provides the superblock pair, the write-ahead metadata journal, replay and checkpointing that the v2 core builds on. The format is specified in [ultriumfs-v2-format.md](ultriumfs-v2-format.md); this document records what the code does and how it was verified.

Code: `kernel/src/fs/crc32c.[ch]`, `ufs2.h`, `ufs2_core.c`; tests `kernel/src/tests/ufs2_journal_tests.c`; crash model in `kernel/src/storage/block.c`.

## API

`ufs2_core_format` (writable, unmounted, **scratch, flush-capable** device), `ufs2_core_open(core, device, want_write)`, `ufs2_core_close`, and transactions: `ufs2_txn_begin`, `ufs2_txn_block` (a staged, writable image of a metadata block), `ufs2_meta_read`, `ufs2_txn_commit`, `ufs2_txn_abort`. Plain `ufs2_block_read/write` and `ufs2_flush` are for file data. Home locations in the superblock copies and the journal region are refused. Limits are `max_txn_blocks` (13 on a 2 MiB volume, 45 at 4 GiB) and the sequence counter; both are rejected **before any write**.

## Commit sequence (as implemented)

1. descriptor + payload written to the ring (`journal-write`), flush (`journal-flush`)
2. commit block written (`commit-write`), flush (`commit-flush`): **committed**
3. images written to home locations (`home-write`), flush (`home-flush`)
4. checkpoint: next superblock generation written to the other copy (`sb-write`), flush (`sb-flush`)

Every I/O error faults the volume read-only; remounting replays anything committed. The phase names are recorded when a simulated crash fires, so every result below names its failure window.

## Open policy (implemented)

- Superblock selection per section 3.2 of the specification: higher valid generation wins; copies must agree on static fields; one damaged copy is tolerated and **rewritten by the first superblock update** (`sb_was_degraded` reports it); both damaged is unmountable and changes nothing.
- A read-write open requires a flush-capable, writable device; otherwise the volume opens **read-only with the reason** (`ro_reason`). A read-only open of a volume whose journal holds a committed transaction fails with *recovery required*; replay happens only on writable opens, before the superblock is marked mounted.
- Unknown incompatible features and other versions are refused; unknown read-only-compatible features force read-only; unknown compatible features are ignored.
- A failed open never writes (checked by counting device events) and releases the device.

## Findings recorded during implementation

- **The payload CRC carries the safety of the first journal flush.** Removing the flush between payload and commit is *not* detectable by crash simulation, because a commit whose payload was lost fails its payload checksum and recovery falls back to the old state. The flush is kept (it is the specified ordering and costs little); the test suite detects its absence structurally (the `journal-flush` window disappears).
- **Prefix-only loss policies are not enough.** A first version of the crash model only kept a prefix of pending writes, and so missed a deliberately removed commit flush in the matrix. A *suffix* policy (keep only the last k pending writes) models reordering and now catches it as `window 'home-write' policy keep-last-1: recovered state is not a committed state`.
- A "damaged tail" report must not fire for stale records from an earlier lap of the ring; it fires only for a torn descriptor or a descriptor with exactly the expected sequence number that fails validation.

## Verification

16 kernel cases (`ultrium test ufs2`): geometry limits for 2 MiB and 4 GiB; format and open; format safety; flush/read-only policy; superblock pair (alternation, each copy damaged, both damaged, healing); superblock feature policy (8 cases); transaction basics and limits; **crash matrix** (every I/O event of a three-transaction script × 14 loss policies, hundreds of runs: the recovered state must be exactly one of the committed states, never behind what had returned success, and the volume must be writable and recoverable again); **named crash windows** (journal-write, journal-flush, commit-write, commit-flush, home-write, home-flush, sb-write, sb-flush, each with its required outcome: old, new or either); **replay idempotence** (power cut at every event of recovery itself); 13 kinds of damaged journal records (payload, checksums, sequence, counts, targets in the journal or superblocks, duplicates); journal wrap-around and several laps; I/O errors at every transfer of a commit; 512-byte device blocks; and rejection of v1/blank data.

### Negative controls (temporary mutations; code restored after each run)

| Deliberate defect | Result |
| --- | --- |
| commit block not flushed before applying | **crash matrix fails** (`home-write`, keep-last-1: torn transaction) plus 3 other cases |
| home writes not flushed before the checkpoint | crash matrix fails (`sb-flush`, random-1: torn transaction) plus 2 other cases |
| payload checksum not verified at replay | damaged-journal case fails (a damaged transaction was replayed) |
| replay accepts any sequence number | damaged-journal and basics cases fail |
| payload flush removed | only the structural window test fails (see findings: safe by design) |

## Not covered

Physical power loss; real device cache behaviour (virtio flush is exercised by the existing block tests, but data loss is only *simulated* by the memory device); multiple live transactions (lazy checkpointing is reserved, see the specification); concurrency (single-CPU kernel).
