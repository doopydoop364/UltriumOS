# Ultrium OS v0.4.0 — Kernel mapping permissions

## Model

Region mappings support `VMM_PAGE_READ`, `VMM_PAGE_WRITE`, and `VMM_PAGE_EXECUTE`. Read is required because x86-64 does not provide a read-disable bit for ordinary page mappings. Every mapping is supervisor-only; there is no user permission option. Writable and executable cannot be requested together (W^X).

`vmm_alloc_pages(count)` defaults to read/write, nonexecutable pages. `vmm_alloc_pages_with_permissions(count, permissions)` chooses permissions at allocation. `vmm_set_pages_permissions(base, permissions)` changes every page in a live allocation after validating all current entries. `vmm_get_page_permissions(page, &permissions)` accepts an aligned page address within a live allocation and verifies the actual PTE against the allocation ledger. `vmm_free_pages` validates the stored permission state before unmapping and releasing owned frames.

The allocator zeroes each PMM frame through the existing HHDM before installing the requested mapping. This permits allocating a read-only region without writing through a read-only alias. The caller owns the returned virtual range and must free only its exact base.

## CPU configuration

Before creating the writable scratch subtree, the kernel checks the extended CPUID NX feature, enables EFER.NXE, sets CR0.WP, and reads the controls back. The region allocator is available only when both NX and supervisor write protection are active. The current QEMU x86-64 setup supports these controls.

Leaf PTE changes preserve CPU-maintained Accessed and Dirty bits during identity checks. Updates invalidate each affected virtual page. Upper-level entries remain writable to permit mappings with different leaf permissions; leaf entries stay supervisor-only.

## Limits and validation

The API applies only to the fixed VMM-owned window in the current CR3. It is single-CPU and does not provide process isolation. Since the HHDM still maps physical memory for kernel access, another kernel component can write or execute the same frame through that alias; these PTE permissions are a mapping contract, not a boundary between trusted kernel subsystems. The test suite checks the PTE-derived permission state and CR0.WP/NXE readiness without deliberately triggering fatal page faults.

Run `make TOOLCHAIN=llvm` and `python3 tools/qemu-memory-test.py 'ultrium test all'`.
