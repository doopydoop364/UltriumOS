# UltriumFS in the VFS: mounts and storage commands

Status: implemented and QEMU-tested (memory-backed devices in `ultrium test ultriumfs`; virtio disks in the persistence test).

## Mount model

The root filesystem is still the volatile RAMFS at `/`. Other filesystems mount on an **existing, empty directory of the root filesystem**. The VFS (`kernel/src/fs/vfs.c`) keeps a table of 4 mounts (root plus 3) and routes each canonical path by longest mount-point prefix; the backend receives the path relative to its mount (`/volumes/disk/a` becomes `/a`, the mount point itself becomes `/`). Paths outside every mount point reach RAMFS unchanged.

```text
/                    RAMFS (volatile)
├── notes.txt
└── volumes/
    └── disk/        UltriumFS (persistent), mounted from a block device
```

Because RAMFS is volatile, the mount-point folders (`/volumes/disk`) must be recreated each boot before mounting. Nothing is mounted automatically.

### Rules

- Mount points: canonical absolute, not `/`, an existing directory in RAMFS, **empty**, not already a mount point, no nesting (not inside a mount, not an ancestor of one).
- Handles carry their mount index in the top byte of `slot`; reads, writes and closes are routed by it. Per-mount open-handle counts make a volume **busy**.
- A mount point or an ancestor of one cannot be unlinked or renamed (`busy`). `remove -r` never descends into or removes a mount point: it stops with `busy` and the report shows where.
- **Rename across mounts is refused** (`unsupported`); use `copy` then `remove`.
- **Copy** within RAMFS uses its own handler; every other copy (across mounts, or within UltriumFS) is a chunked VFS copy: it refuses an existing destination, copies files only, and removes the partial destination if it fails (for example a 100 KB UltriumFS file copied to RAMFS, whose files are limited to 4096 bytes).
- `info` metadata (filesystem name, persistent or volatile) comes from the VFS, not from the shell.
- **Freeze**: `vfs_freeze(true)` makes creates, removes, renames, copies and write-opens return `busy`, and writes through existing handles fail; reads continue. Used by shutdown.
- Unmount: refuses open handles; the storage command also refuses while the shell's current folder is inside the volume. A failed sync keeps the volume mounted so pending changes are never discarded. The one exception is a *faulted* volume (a metadata write failed): it is released, left marked unclean, and the error is reported.

Only one UltriumFS volume (v1 or v2) can be mounted at a time. The existing USH filesystem commands (`list`, `go`, `where`, `new file/folder`, `view`, `write`, `remove`, `info`, `copy`, `move`, `rename`) work unchanged on the mounted volume because they only call the VFS.

## `ultrium storage` commands

```text
ultrium storage help
ultrium storage devices                       block devices and capacity
ultrium storage volumes                       mounted filesystems
ultrium storage format <device> [-y|--yes]    destructive; asks for confirmation unless -y
ultrium storage mount <device> <path> [-r]    verify and mount UltriumFS (read-only with -r)
ultrium storage unmount <path>                sync, release; busy volumes are refused
ultrium storage sync                          flush all mounted persistent volumes
ultrium storage info <path>                   filesystem, capacity, free space, mode
ultrium storage check <device> [--repair]     verify (read-only) or conservatively repair an UltriumFS 2 volume
```

Every path supports a trailing `help`. **Format creates UltriumFS 2** (journaled; needs a flush-capable device). `mount` detects the version: UltriumFS 2 mounts read-write when verified (otherwise read-only or refused, see [recovery](ultriumfs-v2-recovery.md)); UltriumFS 1 mounts **read-only only**. `format` never runs automatically and never at boot. It refuses unless the device exists, is not mounted or claimed, is writable, and is a **scratch disk** (virtio serial starting `ULTRIUM-SCRATCH`, see `docs/block-devices.md`); the check is repeated after confirmation. `-y` only skips the question. A disk that fails mount verification is left untouched. `info` reports capacity only where the filesystem can provide it; RAMFS reports "not provided".

## Shutdown integration

`shutdown` (see `docs/power-management.md`) now runs, before masking interrupts: freeze the VFS, `sync` every persistent volume (device flush, superblock marked clean, flush again). If any sync fails, shutdown is **aborted**: the VFS is unfrozen, the shell returns, and the message says data may not be persisted. To proceed, fix the problem or release the volume with `ultrium storage unmount <path>` and retry. Shutdown never reports data as saved when it is not. A volume with nothing mounted prints "nothing to synchronize".

## Tests

`ultrium test ultriumfs` includes three VFS cases (routing and shell commands, mount protection and policy, unmount/handles/freeze/shutdown sync with a dead-device failure). `tools/qemu-persistence-test.py` (next PR) proves persistence across real boots.

## Limits

One UltriumFS mount; four mounts total; RAMFS files stay limited to 4096 bytes (a RAMFS destination of a copy fails cleanly); `remove -r` handles at most `VFS_NODE_MAX × 3` steps per call, so a very large tree may need repeating; no symbolic links, hard links or mount namespaces.
