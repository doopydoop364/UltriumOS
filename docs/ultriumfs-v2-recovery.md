# UltriumFS 2: mount policy, checker and conservative repair

Status: implemented and QEMU-tested (memory-backed devices and virtio disks). Specification: [ultriumfs-v2-format.md](ultriumfs-v2-format.md) sections 10–12.

## Commands

```text
ultrium storage check <device>             read-only verification; never modifies the device
ultrium storage check <device> --repair    conservative repair (explicit)
ultrium storage check help
```

`check` works on an **unmounted** UltriumFS 2 device only. It refuses a mounted or in-use device, a v1 volume (no journal, no checker), and a blank device. It prints the findings (class, description, inode/block), counts, whether writable mounting is safe, and for `--repair` the report below.

## What the verifier checks

One bounded pass shared with mount: superblock copies, journal state, every inode checksum and reserved field, extent order/range/overflow, **duplicate block ownership across the volume**, extent leaf blocks, block counts, every directory block footer and entry (generation-checked), duplicate references, parent links, parent-chain cycles, duplicate names, orphans, and the on-disk bitmap against the ownership derived from the inodes (leaks, unmarked owned blocks, padding, block checksums). Findings are classified **repairable**, **orphan** or **structural**.

## Mount policy (implemented)

| Situation | Result |
| --- | --- |
| Valid UltriumFS 2, journal empty or replayable, verification clean, writable flush-capable device | read-write |
| Journal holds committed transactions and the device is writable | replayed, verified, mounted read-write |
| Journal holds committed transactions and the mount is read-only or the device cannot be written | **refused: recovery required** (nothing replayed); `check --repair` or a read-write mount fixes it |
| Only **repairable** findings (bitmap mismatch, damaged superblock copy, stale state) | mounted **read-only**, problems printed, `repair recommended` |
| **Structural** findings | **refused**, nothing modified, findings printed |
| Orphans only | read-write; counted and reported, never removed |
| Device without flush, or read-only device | read-only with the reason |
| UltriumFS 1 volume | **read-only, never written** (below) |
| Unknown or blank disk | refused; never formatted automatically |

The volume is marked *mounted* only after verification passes, so a refused mount leaves the on-disk state untouched. After an unclean session the mount replays the journal, verifies, and reports `unclean session`.

## Repair

Refused when: the device is mounted/in use, read-only, or without flush; **any structural finding** exists (the options are listed: restore from backup or reformat the scratch disk); both superblock copies are damaged (no third copy exists). Otherwise it performs only these, all through the journaled, flush-ordered paths:

1. replay validated committed journal transactions;
2. rewrite a damaged superblock copy from the valid one (next generation);
3. rebuild the block bitmap from extent ownership **only when ownership is unambiguous** (no structural findings), as one atomic transaction;
4. clear a stale *mounted* state after a clean verification.

It **never** deletes, truncates, moves or invents files, never changes directories, and leaves orphans in place (reported). The report lists problems found, changes made, remaining problems, and whether writable mounting is safe; a repair that leaves anything but orphans is reported as **PARTIAL**. A checksum only detects corruption.

## UltriumFS 1 compatibility (tested)

- `ultrium storage mount` recognises v1 by its magic and mounts it **read-only** through the v1 driver; it prints that v1 volumes are never modified. The v1 path never writes (no dirty mark): a test mounts a v1 image, reads, attempts changes (refused), unmounts and requires the device image to be **byte-identical** (kernel test and the `--v1` QEMU persistence run).
- `ultrium storage format` creates **UltriumFS 2**. UltriumFS 1 can only be created from kernel test code. Formatting a v1 disk still needs the explicit confirmation like any format.
- Migration: mount the v1 volume read-only, format a **separate** scratch disk (v2) and copy files with the shell (`copy`; recreate folders with `new folder`). There is no automatic or in-place upgrade.

## Tests

`ultrium test ufs2fs` (29 cases): the checker never writes (event counter and CRC of the whole image), reports seven kinds of structural damage and **refuses repair leaving the image byte-identical**, repairs bitmap leaks / unmarked blocks / bad bitmap checksums / padding (then the volume checks clean, mounts read-write, keeps its data and a second repair is a no-op), repairs a damaged superblock copy and a stale state, repairs a **committed-but-unapplied journal transaction** (built with the volatile-cache crash model; the committed create reappears), refuses both-superblocks-damaged, leaves orphans alone, refuses on read-only and no-flush devices, and drives the shell commands end to end (blank disk refused and untouched, format, mount, write, info, check refused while mounted, unmount, check, repair). The v1 case proves read-only and byte-identical.

### Negative controls (temporary mutations, restored)

| Deliberate defect | Result |
| --- | --- |
| repair attempts structural damage | refusal case fails |
| rebuilt bitmap blocks not checksummed | bitmap repair case fails |
| v1 volume mounted writable | v1 policy case fails |
| damaged superblock copy never rewritten | superblock repair case fails |

A defect found while writing these: repair did not actually rewrite a damaged superblock copy unless the journal happened to need replay, and the verifier then flagged the same copy again. Repair now writes it explicitly.

## Not covered

Repair of structural damage (by design), reconnecting or removing orphans, journal repair beyond replaying valid committed transactions, bad-sector remapping, and repair of volumes with both superblock copies damaged.
