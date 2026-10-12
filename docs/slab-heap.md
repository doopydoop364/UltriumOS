# Ultrium OS v0.0.8 — Size-class slab kernel heap

Phase A, first checkpoint: a more efficient replacement for the original one-page-per-allocation kernel heap.

- Allocates objects in power-of-two size classes from 16 through 4096 bytes.
- Multiple small objects share one 4 KiB physical page (for example 128 independent 32-byte objects can share a page).
- Uses four 64-bit allocation bitmap words per slab and a static table tracking up to 512 slabs.
- Allocates physical pages from the v0.0.6 PMM and addresses them through Limine's existing higher-half direct map.
- Zeroes an individual block on each allocation, validates ownership/alignment on free, ignores duplicate frees, and returns completely empty slabs to the PMM.
- Boot self-test checks distinct small allocations, independent writes, a large allocation, and return of pages/counters.

## Remaining limits
- No allocation larger than 4096 bytes yet, no configurable alignment above the class size, no thread safety, and no custom page-table mapping.
- The slab descriptor table is statically bounded at 512 pages.
- The allocator assumes a single-core boot context and Limine's existing HHDM virtual mappings.
- Next Phase A steps: safer page-table management, virtual allocation ranges, multipage allocations, exception handlers, and robust heap accounting.
