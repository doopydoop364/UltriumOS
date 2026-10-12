# Physical memory manager — Ultrium OS 0.0.6

This early x86-64 physical-page allocator consumes Limine's memory-map and higher-half direct-map (HHDM) responses.

- Only LIMINE_MEMMAP_USABLE regions are eligible.
- 4096-byte pages are aligned; physical page zero is excluded.
- A sequential range cursor provides new pages, and freed pages form an intrusive free list through HHDM.
- Caller receives a **physical** address. To dereference a page in the current kernel, map it first or use the known HHDM offset. This API does not implement virtual memory mapping.
- `pmm_free_page` validates addresses and checks a bounded free-list traversal before publishing a frame. Initialization rejects overlapping usable ranges and page-count overflow.
- `ultrium system memory` displays managed usable memory, free pages, and pages currently checked out.
- A boot-time self-test obtains two distinct pages, rejects a double free, and ensures counters are restored.

## Limitations
This is intentionally **not** yet a bitmap allocator, kernel heap, paging subsystem, or thread-safe allocator. The fixed maximum of 128 usable memory ranges can cause initialization failure on complex memory maps. Free-list double-free checking is linear in the number of released pages. The physical-address-only free API cannot detect a stale owner freeing a frame again after that frame has been reallocated; callers must maintain exclusive, current ownership. Resolving this requires an allocation-token/generation design across PMM, heap, and VMM. Memory is session-only and should not be used concurrently on multiple CPUs without locking. Reclaimable bootloader/ACPI memory is excluded.

On boot, verify `MEMORY SELF TEST PASSED`; if it fails, don't build on this allocator until debugged. The CLI should still work, but the test needs investigation.
