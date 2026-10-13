# Ultrium OS v0.4.0 — Kernel virtual memory architecture

## Boot mappings and current page tables

Limine enters `kmain` with paging enabled and supplies the HHDM offset. Ultrium keeps that active CR3 and uses the HHDM to reach physical pages. `vmm_translate` reads CR3/CR4 and walks the active 4- or 5-level tables, including 2 MiB and 1 GiB leaves. Mapping changes currently require four-level paging; `vmm_mapping_init` rejects CR4.LA57 because it does not build a PML5 path.

The VMM reads CR3/CR4; this prototype does not install a new address space. The PMM supplies generation-bearing ownership handles for page-aligned frames. VMM metadata retains handles for owned backing frames and page-table frames; PTEs and HHDM pointers contain only borrowed physical addresses. The Limine root and pre-existing boot tables are borrowed and never returned to PMM. Page-table memory is zeroed and accessed through the HHDM.

```text
Limine CR3
  └─ first unused upper-half PML4 entry (selected at boot)
       └─ VMM-owned PDPT
            └─ VMM-owned page directory, entry 0
                 └─ VMM-owned initial PT
                      ├─ PTE 0: legacy temporary mapping test
                      ├─ PTE 1–32: legacy managed-page API
                      ├─ PTE 33 onward: virtual-region allocator
                      └─ later 2 MiB chunks: VMM-owned PTs created on demand
```

At initialization, Ultrium scans PML4 indexes 256 through 510 and uses the first entry whose Present bit is clear. This avoids changing a present Limine or kernel mapping. The chosen slot describes a 512 GiB canonical range; the region manager owns only a bounded 16 MiB window at its beginning, starting after the 33 legacy pages. The fixed page limit avoids assuming the rest of that PML4 slot is available.

## Region allocation and ownership

`vmm_alloc_pages(count)` reserves one contiguous virtual run in the 4096-page (16 MiB) window, with at most 64 live allocations. Each virtual page gets a separate PMM frame, so physical backing need not be contiguous. The default is read/write, nonexecutable, supervisor-only, zeroed, and 4 KiB. The permissions API also supports read-only and read/execute mappings, with W^X enforced. The manager owns those backing frames and the additional PT frames it creates. The initial PDPT, directory, and PT belong to the older scratch mapping setup and are retained for the boot lifetime. `ultrium system memory` shows current region page and PT counts.

The manager has static metadata for at most 64 live allocations and 4096 virtual pages. Allocation checks count, range capacity, record capacity, and every target PTE before changing mappings. It creates any needed PTs and obtains all backing frames before publishing leaf mappings and allocation metadata. If setup fails, it removes newly installed entries, invalidates affected TLB entries, returns acquired frames, and removes empty PTs created by that attempt. A PMM free failure is treated as an allocator invariant failure: the affected pages remain recorded as orphaned and unavailable instead of being reused.

`vmm_free_pages(address)` accepts only the exact base of a live allocation. It validates all recorded leaf mappings before unmapping any page, clears each PTE and invalidates its address, then returns the owned frames. Empty dynamically-created PTs are detached and returned to PMM; the pre-existing PT at directory index 0 is retained. Invalid pointers, interior pointers, borrowed pages, and duplicate frees are rejected.

The manager is single-CPU and non-concurrent. It does not provide guard pages, create process address spaces, support 5-level writable mappings, or reclaim the initial scratch tables. It assumes no other subsystem writes inside the selected PML4 slot. MMIO and user mappings are outside this API. Read-only and NX bits protect the mapped alias; kernel code can still reach the same physical frame through the Limine HHDM, so these permissions are not a security boundary between kernel subsystems. See [physical memory ownership](physical-memory.md) for metadata cost, generation exhaustion, and the move-only-by-convention handle contract.

## Diagnostics and validation

`vmm_region_stats_get` reports reserved and mapped pages, owned backing frames, remaining window capacity, and dynamically allocated region PT frames. The kernel test suite exercises data at page boundaries, independent allocations, accounting, invalid and double frees, repeated reuse, full-window exhaustion, and injected backing-allocation failure so rollback can be checked without exhausting host memory.

Run `make TOOLCHAIN=llvm`, `python3 tools/qemu-memory-test.py`, and `python3 tools/qemu-memory-test.py 'ultrium test all'`. The headless QEMU script drives the CLI through QMP and checks the COM1 test summary. The first PML4 entry selection and QEMU environment are still assumptions to revisit before multiple address spaces or SMP.
