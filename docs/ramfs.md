# VFS and RAMFS prototype

The original VFS/RAMFS prototype is merged on `main`. USH extensions are in the stacked USH draft PRs. RAMFS is volatile; all files and directories are lost at reboot.

## Current implementation

The kernel initializes one RAMFS root at `/` after the heap is ready. Generic VFS calls dispatch through a filesystem operation table to this single mounted backend. Path normalization lives in the VFS; RAMFS owns node lookup and storage. The interface leaves room for another backend, while multiple mounts and mount-point lookup remain future work. It supports directories, regular files, absolute path lookup, normalized relative paths for the shell, read/write handles, and directory enumeration.

Limits are deliberately bounded:

- Path length: 127 bytes.
- Component name: 31 bytes.
- File length: 4096 bytes.
- Total allocated file-buffer capacity: 65536 bytes.
- Total nodes, including root: 128.
- Simultaneous file handles and directory iterators: 64 of each.

Names are case-sensitive in the VFS. Shell command names are case-insensitive; command arguments preserve their case. Paths support repeated separators, `.`, and `..`; attempts to move above `/` remain at root. Paths containing control characters or overlong components are rejected. See the USH grammar for shell quoting support.

The native USH vocabulary, optional compatibility aliases and safety contracts are documented in [Ultrium Shell](ultrium-shell.md). Shell actions validate intermediate path traversal; low-level VFS operations require canonical absolute paths. VFS stat reports stored type and file byte length. RAMFS supports allocation-free rename and copy with allocation rollback.

## Ownership and failure behavior

The filesystem owns the root, linked nodes, and each file's data buffer. Creating a node allocates and initializes it before linking it into its parent. File-buffer growth allocates the replacement, copies existing bytes, and commits the pointer only after allocation succeeds. Unlink frees only an unreferenced file or an empty directory. Deleting a node with an open handle, or mutating a directory while it has an open iterator, returns `VFS_UNSUPPORTED`; this keeps iterator links stable in the current single-CPU design.

Open handles are caller-owned, zero-initialized `{slot, generation}` tokens backed by bounded static slot tables. Callers should move handles by convention; copying a live token creates a second capability to the same handle. Open refuses to overwrite a nonempty token. Close invalidates that token and advances its generation, so stale copies are rejected after slot reuse. Generation exhaustion retires the slot rather than wrapping. These tokens do not provide synchronization or protection from kernel code forging an active token.

The VFS and RAMFS use `kmalloc`/`kfree`; no filesystem code frees physical frames directly. Allocation failure injection is one-shot and confined to VFS-owned dynamic allocations. Failed node creation and file-buffer growth leave the namespace or file content unchanged. The node, byte-capacity, and handle limits return `VFS_NO_SPACE` predictably.

## Tests

`ultrium test filesystem` checks path normalization and root clamping, file read/write and EOF behavior, directory enumeration order, invalid/stale/forged handles, handle exhaustion, allocation rollback, deletion rules, and heap/physical-memory accounting. `ultrium test all` includes the filesystem suite. Tests run twice in one QEMU boot through the automated harness.

Run locally:

```sh
make -B kernel TOOLCHAIN=llvm
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py 'ultrium test filesystem'
python3 tools/qemu-memory-test.py 'ultrium test all'
```

For interactive checks use the native examples in [Ultrium Shell](ultrium-shell.md).

## Current boundaries

There is no unmount operation, multiple mount support, persistent storage, block-device API, file permissions, symbolic links, or user-space file access. RAMFS directory enumeration is insertion ordered. Open-file deletion is rejected rather than deferred. The kernel remains single-CPU and unsynchronized; filesystem and heap operations must not run in interrupt context.

See [the architecture proposal](filesystem-architecture.md) for the intended VFS boundary and future block-device layering.
