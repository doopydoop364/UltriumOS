# Kernel memory test workflow

The freestanding test runner in `kernel/src/tests/` runs named cases, prints a specific failure reason (and the existing VMM stage number where relevant), and ends with `TEST SUMMARY passed=N failed=N`. It uses static case descriptors and stack storage, with no hosted C library. Only test output is copied to COM1 at 115200 8N1; framebuffer output and PULSE styling stay unchanged. UART polling is bounded, so serial failure does not block the shell. Tests are explicit CLI actions rather than automatic boot work.

Commands:

```text
ultrium test help
ultrium test memory
ultrium test all
```

`all` currently runs the memory group; future CPU, device, and filesystem groups can be added to the same runner. The legacy `ultrium system memory test` still runs the 128 × 32-byte slab stress check through the runner. Existing `ultrium system memory map test`, `ultrium system memory mapping test`, and `ultrium system memory mapping managed test` remain available.

The suite wraps the existing PMM, heap, page-table, scratch-mapping, and managed-page self-tests. New cases check invalid inputs, four simultaneous physical allocations and accounting, heap block isolation, 32 managed mappings, bounded-pool exhaustion, stale translation removal, double-free rejection, and cleanup. The virtual-region case checks single and multi-page mappings, boundary access, noncontiguous physical backing, translation, repeated reuse, invalid frees, both backing-frame and page-table rollback, and exhaustion of all 4096 pages and 64 allocation records. The original slab stress code has one implementation shared by both command paths.

## Run in development

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py
python3 tools/qemu-memory-test.py 'ultrium test all'
python3 tools/qemu-memory-test.py 'ultrium system memory test'
make run TOOLCHAIN=llvm
```

The Python script uses only the standard library. It boots UEFI QEMU, types into the existing PS/2 keyboard via QMP, reads COM1, and fails unless the expected pass summary arrives. QEMU must permit a local Unix QMP socket. The script uses a fixed firmware wait, so slow hosts may need a longer wait. In the interactive QEMU session, check `help`, `ultrium help`, `ultrium test help`, the existing nested memory help menus, `ultrium system memory`, keyboard editing/history, and the mapping commands. These visual interactions are not covered by the serial script.

## Audit and limits

The existing boot tests only produced brief pass/fail text. The VMM mapping tests exposed numeric stages, and the slab stress command checked one allocation pattern. The managed-page test covered eight of 32 slots. Most failure paths have no fault injection; low-memory and PMM range-exhaustion behavior are not safely reproducible in the current QEMU setup. The PMM uses a linear free-stack double-free check; tests do not mutate metadata or map arbitrary physical memory. A failed cleanup is reported, but the current allocator has no recovery facility for every corrupted state. The test suite assumes the current single-CPU kernel and runs after normal initialization.
