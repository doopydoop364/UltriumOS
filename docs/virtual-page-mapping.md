# Ultrium OS v0.1.1 — Controlled scratch virtual mapping

This initial page-table writing checkpoint builds an **isolated scratch mapping** in an unused upper-half PML4 slot. It does **not** expose a general virtual memory mapping API.

## Behavior
- Only supports active x86-64 four-level paging (five-level paging is read-only for now).
- Scans for an unused PML4 entry rather than overwriting existing Limine mappings.
- Allocates three page-table frames for the empty scratch subtree, using existing physical allocation and HHDM access.
- `ultrium system memory mapping test` maps one borrowed physical page to the scratch virtual address, invalidates its TLB entry, writes through the alias, verifies the write through HHDM, clears the page-table entry, invalidates the TLB again, and returns the page to PMM.
- The scratch page-table frames stay allocated throughout the boot session. Only the temporary mapped data page is released.
- `ultrium system help` now lists the complete system command family.

## Limits
- Kernel-only, single CPU, no concurrent page-table mutations, no page sharing ownership tracking, no arbitrary mappings, no user-mode support.
- Fails gracefully when no free upper-half PML4 slot exists; do not force-map over existing memory.
- The v0.1.1 scratch mapping uses supervisor read/write, nonexecutable permission in current kernels; permission selection applies to the later region API.
- CPU faults still halt the OS; test only in QEMU.
- At the time of this v0.1.1 checkpoint, future work included region tracking, table cleanup, multi-page ranges, and page permissions. Region and permission APIs are documented in `virtual-memory-architecture.md` and `page-permissions.md`.

## QEMU checks
Check for `SCRATCH VIRTUAL MAPPING READY`, and run `ultrium system memory mapping test` (expect PASSED). Then re-run `ultrium system memory map test` and `ultrium system memory test` to catch regressions.
