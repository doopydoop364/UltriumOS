# Kernel memory test workflow

The freestanding test runner in `kernel/src/tests/` runs named cases, prints a specific failure reason (and the existing VMM stage number where relevant), and ends with `TEST SUMMARY passed=N failed=N`. It uses static case descriptors and stack storage, with no hosted C library. Only test output is copied to COM1 at 115200 8N1; framebuffer output and PULSE styling stay unchanged. UART polling is bounded, so serial failure does not block the shell. Tests are explicit CLI actions rather than automatic boot work.

Commands:

```text
ultrium test help
ultrium test memory
ultrium test interrupts help
ultrium test interrupts
ultrium test all
```

`memory` runs 14 memory and exception descriptor checks. `interrupts` checks that the periodic timer IRQ advances the system tick. `all` runs both groups (15 cases). The legacy `ultrium system memory test` still runs the 128 × 32-byte slab stress check through the runner. Existing `ultrium system memory map test`, `ultrium system memory mapping test`, and `ultrium system memory mapping managed test` remain available.

The suite wraps the existing PMM, heap, page-table, scratch-mapping, and managed-page self-tests. Ownership cases check valid allocate/release cycles, double-free and stale-handle rejection after physical reuse, generation changes, invalid and forged handles, injected exhaustion, and accounting restoration. Other cases check four simultaneous physical allocations, heap block isolation, 32 managed mappings, bounded-pool exhaustion, stale translation removal, and cleanup. The virtual-region case checks single and multi-page mappings, boundary access, noncontiguous physical backing, translation, repeated reuse, invalid frees, both backing-frame and page-table rollback, and exhaustion of all 4096 pages and 64 allocation records. The permission case checks RW/NX defaults, read-only and RX updates, invalid W^X/input rejection, CR0.WP/NXE configuration, and cleanup without intentionally faulting. The original slab stress code has one implementation shared by both command paths.

## Run in development

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py
python3 tools/qemu-memory-test.py 'ultrium test all'
python3 tools/qemu-memory-test.py 'ultrium test interrupts'
python3 tools/qemu-memory-test.py 'ultrium system memory test'
python3 tools/qemu-memory-test.py --keyboard-regression
make run TOOLCHAIN=llvm
```

GitHub Actions runs the LLVM build and ISO packaging, then runs `ultrium test all`,
the legacy memory command, and the shell/keyboard regression. The workflow is in
`.github/workflows/kernel-ci.yml`. It uses Ubuntu 24.04 and downloads Limine and
OVMF release assets through existing Makefile targets; those upstream release
downloads are not pinned by the workflow. A local passing run does not imply
that GitHub Actions has run or passed; check the PR's checks for the hosted result.

The Python script uses only the standard library. It boots UEFI QEMU, types test
commands through QMP, reads COM1, and requires passing summaries. The repeated
suite checks allocator cleanup and timer progress. The `--keyboard-regression`
mode checks Home/End, Left/Right, insertion, Backspace, Delete, Up history, and
Ctrl+C/U/W/L by requiring six successful slab-test summaries. QEMU must permit a
local Unix QMP socket. QMP startup retries and test waits are bounded, and QEMU
startup diagnostics are reported. These editing paths are covered by the QMP
test; graphical interaction remains useful for manual boot and appearance checks.

## Audit and limits

Low-memory exhaustion of all physical frames and generation-counter exhaustion are not run in QEMU. The suite injects a deterministic PMM allocation failure and exercises stale generations across actual frame reuse. Tests do not mutate allocator metadata or map arbitrary physical memory. A failed cleanup is reported, but the current allocator has no recovery facility for every corrupted state. The test suite assumes the current single-CPU kernel and runs after normal initialization.
