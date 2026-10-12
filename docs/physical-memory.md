# Physical memory ownership — Ultrium OS 0.4.0

This early x86-64 physical-page allocator consumes Limine's memory-map and higher-half direct-map (HHDM) responses.

- Only LIMINE_MEMMAP_USABLE regions are eligible.
- 4096-byte pages are aligned; physical page zero is excluded.
- A sequential range cursor provides new pages, and freed pages form an intrusive free list through HHDM.
- Allocation returns a `pmm_page_handle` containing a physical address and allocation generation. `pmm_page_release` validates both against authoritative per-frame state and clears the passed handle on success. There is no address-only release API.
- Physical addresses used for page-table entries and HHDM access are borrowed mapping values. A borrowed address does not carry release authority; the subsystem that owns a frame retains its handle.
- Each usable range reserves enough leading pages for one 32-bit state/generation record per frame in that range. The record storage is in-band, zeroed during bootstrap, and costs about 1 MiB per GiB of usable memory. Metadata pages are excluded from allocation. This avoids heap recursion and a large fixed kernel BSS table.
- The first allocation has generation 1. Odd records are live allocation generations; nonzero even records are released generations. A stale copy fails after release and continues to fail if the same frame is reused with a newer generation. Frames are retired instead of wrapping at generation exhaustion.
- Initialization rejects overlapping usable ranges and arithmetic overflow. Limine-owned, reserved, and MMIO ranges are never admitted.
- `ultrium system memory` displays managed usable memory, free pages, and pages currently checked out.
- A boot-time self-test obtains two distinct pages, rejects a double free, and ensures counters are restored.

## Limitations
Handles are move-only by convention: exactly one subsystem owns and releases each handle. C cannot prevent code from copying a live handle; a duplicate can release the current allocation while it is still live. Generation checks reject stale copies after release/reallocation, not simultaneous duplicate ownership or malicious kernel code that can reproduce a live handle. The PMM, heap, and VMM remain single-CPU and unsynchronized. The fixed maximum of 128 usable ranges remains. Page-table frames borrowed from Limine are not released; VMM-created page tables retain their own handles until detached and reclaimed. Reclaimable bootloader/ACPI memory is excluded.

The kernel suite verifies valid allocate/release, double-free and stale-handle rejection, generation changes on physical reuse, invalid/forged handles, injected allocation exhaustion, and accounting restoration. On boot, verify `MEMORY SELF TEST PASSED`; if it fails, don't build on this allocator until debugged.
