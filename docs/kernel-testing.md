# Kernel memory test workflow

The freestanding test runner in `kernel/src/tests/` runs named cases, prints a specific failure reason (and the existing VMM stage number where relevant), and ends with `TEST SUMMARY passed=N failed=N`. It uses static case descriptors and stack storage, with no hosted C library. Only test output is copied to COM1 at 115200 8N1; framebuffer output and PULSE styling stay unchanged. UART polling is bounded, so serial failure does not block the shell. Tests are explicit CLI actions rather than automatic boot work.

Commands:

```text
ultrium test help
ultrium test memory
ultrium test all
```

`all` currently runs the memory group; future CPU, device, and filesystem groups can be added to the same runner. The legacy `ultrium system memory test` still runs the 128 × 32-byte slab stress check through the runner. Existing `ultrium system memory map test`, `ultrium system memory mapping test`, and `ultrium system memory mapping managed test` remain available.

The suite wraps the existing PMM, heap, page-table, scratch-mapping, and managed-page self-tests. Ownership cases check valid allocate/release cycles, double-free and stale-handle rejection after physical reuse, generation changes, invalid and forged handles, injected exhaustion, and accounting restoration. Other cases check four simultaneous physical allocations, heap block isolation, 32 managed mappings, bounded-pool exhaustion, stale translation removal, and cleanup. The virtual-region case checks single and multi-page mappings, boundary access, noncontiguous physical backing, translation, repeated reuse, invalid frees, both backing-frame and page-table rollback, and exhaustion of all 4096 pages and 64 allocation records. The permission case checks RW/NX defaults, read-only and RX updates, invalid W^X/input rejection, CR0.WP/NXE configuration, and cleanup without intentionally faulting. The original slab stress code has one implementation shared by both command paths.

## Run in development

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py
python3 tools/qemu-memory-test.py 'ultrium test all'
python3 tools/qemu-memory-test.py 'ultrium system memory test'
make run TOOLCHAIN=llvm
```

GitHub Actions runs the LLVM build and ISO packaging, then runs `ultrium test all`
and the legacy `ultrium system memory test` twice per QEMU boot. The workflow is
in `.github/workflows/kernel-ci.yml`. It uses Ubuntu 24.04 and downloads the
Limine and OVMF release assets referenced by the existing Makefile; those
upstream release downloads are not pinned by this workflow. A local passing run
does not imply that GitHub Actions has run or passed; check the PR's checks for
the hosted result.

The Python script uses only the standard library. It boots UEFI QEMU, types the selected test command twice into the existing PS/2 keyboard via QMP, reads COM1, and requires two passing summaries in the same boot. This checks that repeated tests return allocator accounting and ownership state to their original values. QEMU must permit a local Unix QMP socket. The script uses a fixed firmware wait, so slow hosts may need a longer wait. In the interactive QEMU session, check `help`, `ultrium help`, `ultrium test help`, the existing nested memory help menus, `ultrium system memory`, keyboard editing/history, and the mapping commands. These visual interactions are not covered by the serial script.

## Audit and limits

Low-memory exhaustion of all physical frames and generation-counter exhaustion are not run in QEMU. The suite injects a deterministic PMM allocation failure and exercises stale generations across actual frame reuse. Tests do not mutate allocator metadata or map arbitrary physical memory. A failed cleanup is reported, but the current allocator has no recovery facility for every corrupted state. The test suite assumes the current single-CPU kernel and runs after normal initialization.
