# Filesystem architecture proposal

Status: architecture proposal for Phase B. A minimal VFS/RAMFS prototype is being implemented on `feature/vfs-ramfs`, stacked on PR #32; that work is not yet merged. There is no block-device filesystem or persistent file storage. The prototype's current scope and limits are recorded in [ramfs.md](ramfs.md).

## Goals and boundaries

The first filesystem milestone should provide a useful in-memory namespace and a stable interface for later filesystem drivers. It should support files, directories, path lookup, open file handles, basic operations, and directory enumeration. RAMFS contents are temporary and disappear on reboot.

The initial implementation is kernel-only and single-CPU, consistent with the current PMM, heap, VMM, and interrupt assumptions. It is not POSIX, does not provide userspace isolation, permissions, symbolic links, hard links, mount namespaces, or crash consistency. Do not build persistent storage or a custom on-disk format into the first milestone.

The intended layering is:

```text
shell built-ins (pwd, ls, cd, mkdir, touch, cat)
                    |
            path and file API
                    |
                   VFS
              /           \
           RAMFS       future filesystem drivers
                              |
                    future block-device API
```

VFS owns generic path handling, mount lookup, open handles, and dispatch. A filesystem implementation owns its node representation and storage. A future block-device layer supplies bounded block reads and writes to disk-backed drivers; it is not part of RAMFS or this first VFS milestone.

## Proposed object model

Use small opaque objects rather than exposing RAMFS node layouts to the shell:

```c
struct vfs_node;       /* filesystem-owned file or directory */
struct vfs_file;       /* VFS-owned open handle */
struct vfs_dir_iter;   /* VFS/filesystem-owned enumeration state */
```

An operation table attached to a mounted filesystem provides only the operations currently needed: lookup a child, create a regular file, create a directory, unlink an empty file, read, write, and enumerate children. Unsupported operations return a defined error. Keep filesystem-specific state behind `void *` private data or an equivalent opaque field.

Use a small kernel error enum (for example: not found, exists, not directory, is directory, invalid path, no space, unsupported, bad handle, and I/O error). Do not make callers infer errors from null pointers or partially initialized objects. Avoid introducing `errno`, file descriptors, or broad POSIX compatibility solely for this layer.

### Ownership and lifetime

- The mounted filesystem owns its root node and all nodes reachable from that root. A successful create links a node into its parent and transfers node lifetime to the filesystem.
- A failed create leaves the tree unchanged and frees every allocation made by that attempt.
- A `vfs_file` handle is owned by the caller after successful open and released only by `vfs_close`. The handle keeps its node alive, or deletion is rejected while handles exist; choose one policy and test it before implementation. Prefer reference-counted node pins if deletion of open files is required later. The initial RAMFS can reject unlinking an open node.
- A directory iterator is caller-owned after successful open and must be closed. It holds a stable directory reference and reports end-of-directory distinctly from errors.
- Paths and names passed to VFS are borrowed for the duration of the call. The VFS or filesystem copies names it retains. Returned directory-entry names are copied into caller-provided bounded storage or represented by a documented short-lived borrowed view; do not return pointers into a mutable scratch buffer.
- Mount records are owned by the VFS. A mounted filesystem cannot be destroyed while it is mounted or has open handles. Unmounting is not required in the first milestone; if included, it must fail while references remain.
- RAMFS node contents and directory entries are allocated through the established heap. A growing file buffer owns its allocation; reallocation is transactional: allocate and copy new storage before replacing the old buffer. Never use a physical address as a freeing capability; all physical backing must continue to flow through the PMM ownership-handle API beneath the heap.

These rules assume no concurrent callers. Interrupt handlers must not call VFS or heap routines. Add synchronization only with a later explicit concurrency milestone.

## Path and mount behavior

The initial namespace has one RAMFS mounted at `/`. Paths are absolute or relative to a shell working directory. The shell begins at `/`. Repeated separators can be collapsed, and `.` can resolve to the current directory. Reject `..` in the first implementation unless parent tracking and root clamping are defined; never let normalization escape the mounted root. Reject empty names, names containing `/`, and names exceeding a documented fixed maximum. Bound total path length and component count before allocating or traversing.

Path normalization should use caller storage or a fixed bounded stack buffer. It must not modify the caller's string. Lookup walks one component at a time and fails if an intermediate component is not a directory. Directory entries have unique names within a directory. Creation checks for an existing name before committing an entry.

Represent a mount as a normalized path prefix plus a filesystem root. Start with only `/`; add nested mount points when a second filesystem exists and there is a concrete need. If prefix mounts are later implemented, resolve the longest component-boundary prefix (`/dev` must not match `/device`) and define how open handles behave across unmount.

Initial RAMFS capacities should be explicit and bounded: name length, path length, directory entries, and file size/total bytes. Prefer predictable `NO_SPACE` over unbounded growth. Set limits based on available heap and test them. Integer additions and capacity growth must be overflow-checked.

## Minimal API behavior

Proposed generic calls (final names should follow current header conventions):

```c
enum vfs_error vfs_lookup(const char *path, struct vfs_node **out);
enum vfs_error vfs_open(const char *path, uint32_t flags,
                        struct vfs_file **out);
enum vfs_error vfs_close(struct vfs_file *file);
enum vfs_error vfs_read(struct vfs_file *file, void *buffer,
                        size_t capacity, size_t *bytes_read);
enum vfs_error vfs_write(struct vfs_file *file, const void *buffer,
                         size_t length, size_t *bytes_written);
enum vfs_error vfs_mkdir(const char *path);
enum vfs_error vfs_create(const char *path);
enum vfs_error vfs_unlink(const char *path);
```

Read and write advance a per-handle offset. Reads at EOF succeed with zero bytes. A write either completes the requested bounded write or reports a clear partial count and error; the initial RAMFS should prefer all-or-nothing capacity reservation before modifying file contents. Directory enumeration should be a separate iterator API and must reject regular-file handles. Creation and unlink operate on a complete normalized path and may not replace a nonempty directory.

Keep `vfs_node` lookup references and `vfs_file` handles distinct. A borrowed node pointer returned from lookup must either be pinned and explicitly released or remain private to VFS; the shell should use file and directory handles instead of retaining raw nodes.

## RAMFS implementation plan

1. Initialize one stable root directory during filesystem initialization.
2. Store directory entries as owned child-node references with bounded names.
3. Store regular-file length, capacity, and an owned byte buffer; an empty file uses no data buffer.
4. Implement path lookup and create operations with allocation-before-linking, so failures never expose half-created entries.
5. Implement reads, writes, enumeration, empty-file deletion, and nested directories.
6. Reject deleting nonempty directories and, initially, files with open handles.
7. Add exact cleanup paths for failed node, entry, and data-buffer allocation. For tree destruction in tests or shutdown, walk children and free each object exactly once.

No persistent-storage claim is appropriate: RAMFS state is volatile and lost at reboot.

## Shell integration

Use conventional unprefixed commands because these are ordinary file operations: `pwd`, `ls`, `cd`, `mkdir`, `touch`, `cat`, `write`, and `rm`. The first prototype provides this small subset. Keep OS diagnostics under the established `ultrium` namespace. Shell path arguments must be copied or consumed during dispatch; the current command buffer cannot be retained as filesystem-owned memory. `write path text` creates a new file, and `rm path` removes a file or empty directory; neither command adds shell quoting or overwrite behavior.

`cd` changes only the shell's current working directory and must validate the target as a directory. `ls` enumerates through a directory handle. `cat` reads through a file handle and writes bounded chunks to the terminal. Each command needs concise help consistent with existing CLI rules; if commands are grouped into a multiword namespace later, provide help at each namespace level.

## Testing and QEMU reporting

Prefer tests against the VFS/RAMFS API that run through the existing named kernel test framework and report on COM1. The QEMU harness should invoke the filesystem suite and require a named pass/fail summary, as it currently does for memory and interrupt groups. Do not infer success from a live QEMU process or screenshot.

Required cases:

- Root initialization and root lookup.
- File and directory creation, including nested paths.
- Empty-file reads and EOF behavior.
- Read/write contents, offsets, and capacity boundaries.
- Directory enumeration and entry order contract (if order is promised).
- Duplicate names, invalid names, invalid paths, and traversal above root.
- File and directory deletion rules, including nonempty directories and open handles.
- Invalid handles and wrong node types.
- Injected allocation failures at each allocation point, verifying the tree and accounting remain unchanged.
- Repeated create/read/write/delete cycles, closing every file and iterator.
- Full RAMFS teardown in tests, with heap live-allocation accounting returning to its baseline.

Tests must use bounded data and must not expose arbitrary physical-memory access. Add deterministic heap failure injection only if it can be scoped and reset safely; otherwise test allocation failure through a narrow RAMFS allocator hook rather than corrupting global heap state.

## Future block devices and persistence

RAMFS depends only on VFS and the kernel heap. A later block-device API should expose device capacity and sector/block read/write operations with explicit bounds and errors. A disk filesystem driver will translate VFS node operations into block operations and own its cache/metadata policy. Keep block devices below filesystem drivers; do not add device-specific logic to generic path traversal.

Only after storage drivers and recovery behavior are established should Ultrium choose an on-disk format. That decision should be a separate design review covering crash recovery, checksums, versioning, tooling, and compatibility. This proposal does not select a custom format.

## Prerequisites and open implementation choices

Begin implementation after the memory, ownership, and interrupt branches are approved and the QEMU regression suite remains green on the integrated baseline. Before coding, settle these bounded choices in the implementation PR: exact maximum file/path/name sizes; whether node references are private or explicitly pinned; the initial unlink-while-open policy (proposed: reject); whether directory enumeration order is guaranteed (proposed: insertion order for deterministic tests); and how deterministic allocation failure is injected. None of these choices requires a POSIX redesign.
