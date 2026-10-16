# Persistence testing

The default test now exercises **UltriumFS 2** and checks every image with the independent v2 host reader (`tools/ufs2_image.py`): superblock pair, empty journal, extents, directories, bitmap and checksums, in addition to content. `python3 tools/qemu-persistence-test.py --v1` is the UltriumFS 1 compatibility run (mounted read-only, data readable, writes refused, image byte-identical). The older `tools/ufs_image.py` reads v1 images.

The defining test: files written in one boot are read back in the next, from a disposable virtual disk, across real QEMU shutdowns.

```sh
make TOOLCHAIN=llvm
make edk2-ovmf-bins            # once, for the UEFI firmware
python3 tools/qemu-persistence-test.py
```

It creates `test-images/persistence.img` (64 MiB sparse, git-ignored). `tools/ultrium_qemu.py` refuses to attach any disk that is not a regular, non-symlink `.img` file directly inside `test-images/`, and nothing runs as root.

## What it does

| Boot | Actions | Evidence |
| --- | --- | --- |
| 1 | `ultrium test persist blank` (a blank disk is rejected, nothing auto-formats); `ultrium storage format vda` answered **n** (declined), blank again; format answered **yes**; mount; shell commands create nested folders and files, `copy`, `rename`, `remove`, `remove -r -y`; kernel populates multi-block files; `persist stage1` (4 cases); `ultrium storage sync`; `shutdown` | QEMU exits by itself with QMP reason `guest-shutdown`; the image is then decoded **on the host** and must equal the expected tree, superblock state clean |
| 2 | mount (no format); `persist clean` and `stage1` again; shell `write`; `persist modify` (in-place update, file growth across blocks, remove); `persist stage3`; sync; `shutdown` | host decode of the new tree |
| 3 | mount; `stage3`; `ultrium storage unmount`; `shutdown` | host decode |
| 4 | mount; write a file; **kill QEMU** (an abrupt stop; never counted as a shutdown) | host decode: file present, superblock state *modified* |
| 5 | mount the unclean volume; `persist unclean` (reported as unclean, data written before the stop survived); sync; `shutdown` | host decode: clean again |

`tools/ufs_image.py` is an independent reader written from `docs/ultriumfs-format.md`: it checks the superblock and inode checksums, walks the tree, rejects a double reference, and returns every file's bytes. A kernel bug that kept data only in memory cannot pass, because the comparison reads the image file.

Also checked by hand: `shutdown` with no explicit `sync` still marks the volume clean (the shutdown hook syncs).

## Manual run

```sh
python3 -c "open('test-images/demo.img','wb').truncate(64<<20)"
qemu-system-x86_64 -M q35 -m 2G \
  -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
  -cdrom template-x86_64.iso \
  -drive if=none,id=ud0,format=raw,file=test-images/demo.img \
  -device virtio-blk-pci,drive=ud0,serial=ULTRIUM-SCRATCH,disable-legacy=on,disable-modern=off
```

Then at the `ultrium>` prompt:

```text
new folder /volumes
new folder /volumes/disk
ultrium storage format vda            (answer yes)
ultrium storage mount vda /volumes/disk
new folder /volumes/disk/projects
write /volumes/disk/projects/notes.txt "Persistent data"
ultrium storage sync
shutdown
```

Boot again with the same `-drive file=` and, **without formatting**:

```text
new folder /volumes
new folder /volumes/disk
ultrium storage mount vda /volumes/disk
view /volumes/disk/projects/notes.txt
```

## Covered elsewhere

Multi-block files, growth, disk full, space reuse, metadata exhaustion, invalid paths, malformed superblocks, unsupported versions, invalid block references, rollback, mount/unmount cycles, read-only and failure injection are in the 20 in-kernel cases of `ultrium test ultriumfs` (memory-backed devices), documented in `docs/ultriumfs-format.md` and `docs/ultriumfs-vfs.md`.

## Not tested

Real power loss (no journal; see the format document), physical disks and controllers other than QEMU virtio-blk, device disappearance on real hardware, a hung device (timeout/reset path), the persistence of data written by *other* hosts or QEMU versions. An abrupt stop is simulated by killing QEMU, whose host page cache survives; that is not a power-loss test.

## Interrupted-operation recovery

Targeted recovery tests (files larger than 4 MiB, fragmented files, nested folders, renames, block reuse, deterministic power cuts on the virtio disk, an abrupt QEMU stop during a large write) are in `tools/qemu-ufs2-recovery-test.py`; see `docs/ultriumfs-v2-crash-testing.md`. They distinguish simulated cache loss, deterministic cuts, abrupt QEMU termination and physical power loss (not tested).
