# Ultrium OS 0.0.9 — Page table inspection (Phase A)

Read-only virtual-address-to-physical-address translation through the active x86-64 CPU page tables.

The `vmm_translate(virtual, &physical)` helper reads CR3 and CR4, handles 4-level or 5-level paging, rejects noncanonical/unmapped addresses, and accounts for regular 4 KiB pages and 2 MiB / 1 GiB huge pages. It does not modify mappings.

The boot-time PAGE TABLE SELF TEST and `ultrium system memory map test` allocate a physical page, translate an address inside its higher-half direct mapping, compare the computed physical address, release the page and verify the PMM free-page counter is restored.

## Deliberate limitations

- Translation is a snapshot of the current address space; this early single-core kernel lacks synchronized page-table changes.
- The table walk assumes Limine has direct-mapped active page-table frames.
- No mapping, unmapping, permissions updates, address-space switching, TLB invalidation, or virtual-page allocation is implemented in this checkpoint.
- This is an architectural verification checkpoint before making page-table modifications.
- Do not merge until QEMU confirms both PAGE TABLE SELF TEST PASSED and PAGE TABLE TEST PASSED.

Next: exception handlers and controlled page-table editing, including safe invalidation and ownership tracking.
