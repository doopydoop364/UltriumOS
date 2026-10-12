# Ultrium OS v0.1.2 — Managed scratch virtual pages

This Phase A checkpoint extends the v0.1.1 scratch PML4 subtree to support **32 independently managed virtual pages**, using page-table slots 1–32. Slot 0 remains dedicated to the original scratch mapping test.

## API
- `vmm_managed_alloc()` reserves one physical 4 KiB page, maps it into an available scratch slot, invalidates the corresponding virtual address in the TLB, zeros the page, and returns the virtual address.
- `vmm_managed_free(ptr)` validates the page-aligned virtual address, confirms that it belongs to this manager, unmaps it, invalidates the TLB entry, and returns the physical page to the PMM.
- `vmm_managed_live()` reports the number of outstanding managed mappings.
- `vmm_managed_test()` allocates eight pages, verifies zeroed memory and independent writes, checks virtual-to-physical translations, frees the pages, confirms translations disappear and checks PMM accounting.

## CLI
Use `ultrium system memory mapping help` and `ultrium system memory mapping managed test`.

## Limits
This is not yet a general-purpose address-space manager. It is a fixed 32-page kernel-only pool, requires successful v0.1.1 scratch initialization and four-level paging, assumes one CPU, and uses only supervisor writable mappings. No concurrency, process isolation, user mappings, virtual ranges beyond the scratch subtree, or dynamic page-table cleanup is supported. CPU faults are fatal.

Test in QEMU before merging. Run the new managed test, the earlier virtual mapping test, page-table translation test, and slab stress test.
