# Block devices and storage discovery

Status: implemented and QEMU-tested (virtio-blk on Q35 with OVMF). Not tested on physical hardware or non-QEMU virtio.

## Layers

```text
filesystem (future UltriumFS)
        |
   block API  (kernel/src/storage/block.[ch])   validation, registry, windows, failure injection
        |
   driver     virtio_blk.c (modern virtio-pci)  | memory-backed test device (block.c)
        |
   PCI        kernel/src/pci/pci.c               bus-0 enumeration, BAR decode
```

## Block API

`struct block_device` carries name, driver, serial, `block_size` (power of two, 512..4096, never assumed 512), `block_count`, flags (`READ_ONLY`, `FLUSH`, `SCRATCH`, `DEAD`) and per-device statistics. `block_read`/`block_write` take a first block, a block count and a buffer with its size. Before reaching a driver they validate: device registered and not dead, non-null buffer, nonzero count, write permission, `first <= count_total` and `count <= total - first` (no overflow), `count * block_size` fits `size_t`, and the buffer is large enough. `block_flush` reports `BLOCK_UNSUPPORTED` when the device has no flush. Errors: `BLOCK_INVALID`, `RANGE`, `READ_ONLY`, `IO`, `TIMEOUT`, `NO_DEVICE`, `UNSUPPORTED`.

`block_register_window()` registers an offset+length view of a registered device. It is the extension point for partitions (no GPT parsing exists yet) and inherits flags; a parent cannot be unregistered while windows exist. The registry holds 8 devices, single-CPU and unsynchronized like the other allocators.

Multi-block transfers loop one block at a time and are **not atomic**: a failure midway leaves earlier blocks written and returns the error.

## Backend choice: virtio-blk

| Option | Verdict |
| --- | --- |
| **virtio-blk (modern PCI)** | **chosen**: small, documented, works in QEMU without legacy hardware, reports block size and flush support, polling is simple |
| AHCI | much larger (command lists, FIS, port setup); no benefit for a QEMU-first target |
| Legacy ATA PIO | trivial but 28-bit LBA, slow, not representative of modern hardware |

Why *modern* virtio rather than legacy 0.9.5: legacy requires descriptor table and both rings in one physically contiguous, 4096-aligned region of at least two frames, and the PMM only allocates single frames. Modern virtio-pci programs each ring address separately, so with queue size 16 all three rings fit in one frame. Legacy-only devices are rejected with a clear reason (`ultrium storage devices` shows it when no device is found). Transitional devices (ID 0x1001) are used through their modern capabilities.

## DMA, memory and polling

- Each device owns three PMM handles for its lifetime: ring frame (descriptors at +0, available ring at +256, used ring at +512), request frame (16-byte header at +0, status byte at +16), and one bounce data frame holding a single logical block. These are accessed through the direct map; their *physical* addresses go in descriptors. Frames are 4 KiB aligned, ordinary write-back RAM; x86 DMA is cache-coherent, ordering uses `mfence`.
- One request in flight, polled. Interrupts from the device are disabled (INTx disable and `VRING_AVAIL_F_NO_INTERRUPT`). The wait is bounded by a spin count (200 M `pause` iterations), not timer ticks.
- On timeout the device is reset (status 0, confirmed by reading 0) **before** any frame is released, and marked dead. If the reset is not confirmed the frames are intentionally leaked rather than freed under possible DMA.
- Device MMIO (BARs) is mapped by `vmm_map_mmio()`: uncached, non-executable, borrowed (never PMM-owned), from a fixed 16-page pool, never unmapped. PCI Bus Master and Memory Space are enabled by the driver. BAR sizes are probed with decoding disabled and capability offsets validated against them.
- Data is copied through the bounce frame (no scatter/gather, no caller-buffer DMA), which also means arbitrary caller buffers never need to be physically contiguous.

## Limits

Bus 0 only; no PCI bridges, MSI/MSI-X, hot-plug, or multiple queues. No `SEG_MAX`/`SIZE_MAX` negotiation (a single segment per request). Maximum two virtio-blk devices. Logical block sizes above 4096 are refused. No partition table parsing; raw disks and windows only.

## Formatting guard

`BLOCK_FLAG_SCRATCH` marks a disposable disk. The virtio driver sets it only when the device serial (read with `VIRTIO_BLK_T_GET_ID`) starts with `ULTRIUM-SCRATCH`. Destructive commands in later milestones require it.

## Commands

```text
ultrium storage help
ultrium storage devices     name, driver, geometry, capacity, flags, serial
ultrium test storage        7 memory-backed block tests (part of `ultrium test all`)
ultrium test storage disk run   4 tests against the scratch virtio disk
```

`ultrium test storage disk run` writes only to the last 16 blocks of a disk flagged scratch and refuses otherwise.

## Running with a disposable disk

Images are regular files under the git-ignored `test-images/`. `tools/ultrium_qemu.py` refuses to attach any other path.

```sh
python3 -c "open('test-images/demo.img','wb').truncate(64<<20)"
make TOOLCHAIN=llvm
qemu-system-x86_64 -M q35 -m 2G \
  -drive if=pflash,unit=0,format=raw,file=edk2-ovmf-bins/ovmf-code-x86_64.fd,readonly=on \
  -cdrom template-x86_64.iso \
  -drive if=none,id=ud0,format=raw,file=test-images/demo.img \
  -device virtio-blk-pci,drive=ud0,serial=ULTRIUM-SCRATCH,disable-legacy=on,disable-modern=off
```

Add `logical_block_size=4096,physical_block_size=4096` to the device for 4 KiB blocks, or `readonly=on` to the drive for a read-only disk. Never point `file=` at a real device.

## Tests

`python3 tools/qemu-storage-test.py --diskless|--disk|--disk-4k|--disk-readonly`. The disk modes run the hardware cases twice in one boot, then `ultrium test all`, and check the host image file: modified for read-write, byte-identical for read-only.

Tested failure cases: invalid ranges and overflow, short/null buffers, read-only writes and flush, injected transfer failures (including midway through a span), dead and removed devices, window escape, frame accounting. **Not tested** (QEMU cannot easily provoke them): real device I/O errors, a hung virtio device (the timeout/reset path), device hot-removal.
