# VFS and RAMFS prototype

Status: implemented on `feature/vfs-ramfs`, stacked on the filesystem design PR. It is not available on `main` until reviewed and merged. RAMFS is volatile; all files and directories are lost at reboot.

## Current implementation

The kernel initializes one RAMFS root at `/` after the heap is ready. Generic VFS calls dispatch through a filesystem operation table to this single mounted backend. Path normalization lives in the VFS; RAMFS owns node lookup and storage. The interface leaves room for another backend, while multiple mounts and mount-point lookup remain future work. It supports directories, regular files, absolute path lookup, normalized relative paths for the shell, read/write handles, and directory enumeration.

Limits are deliberately bounded:

- Path length: 127 bytes.
- Component name: 31 bytes.
- File length: 4096 bytes.
- Total allocated file-buffer capacity: 65536 bytes.
- Total nodes, including root: 128.
- Simultaneous file handles and directory iterators: 64 of each.

Names are case-sensitive in the VFS. Shell command names are case-insensitive; command arguments preserve their case. Paths support repeated separators, `.`, and `..`; attempts to move above `/` remain at root. Paths containing control characters or overlong components are rejected. Shell arguments currently do not support quoting or spaces in names.

The shell has unprefixed `pwd`, `ls [path]`, `cd path`, `mkdir path`, `touch path`, `write path text`, `cat path`, and `rm path` commands. Each command accepts a `help` argument. `touch` creates an empty file and succeeds when the path already names a regular file. `write` creates a new file and writes the unquoted remainder of the command; it does not overwrite an existing file. Use `rm` before creating a replacement. Quoting is not parsed, and repeated spaces are collapsed by the current shell.

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

For an interactive shell check, run `mkdir /demo`, `write /demo/hello Hello`, `ls /demo`, `cat /demo/hello`, `cd /demo`, `pwd`, `cd /`, `rm /demo/hello`, and `rm /demo`. Expected output includes the `hello` listing, the file text, and `/demo` from `pwd`.

## Current boundaries

There is no unmount operation, multiple mount support, persistent storage, block-device API, file permissions, symbolic links, or user-space file access. RAMFS directory enumeration is insertion ordered. Open-file deletion is rejected rather than deferred. The kernel remains single-CPU and unsynchronized; filesystem and heap operations must not run in interrupt context.

See [the architecture proposal](filesystem-architecture.md) for the intended VFS boundary and future block-device layering.
