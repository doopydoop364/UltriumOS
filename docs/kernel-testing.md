# Kernel memory test workflow

The freestanding test runner in `kernel/src/tests/` runs named cases, prints a specific failure reason (and the existing VMM stage number where relevant), and ends with `TEST SUMMARY passed=N failed=N`. It uses static case descriptors and stack storage, with no hosted C library. Only test output is copied to COM1 at 115200 8N1; framebuffer output and PULSE styling stay unchanged. UART polling is bounded, so serial failure does not block the shell. Tests are explicit CLI actions rather than automatic boot work.

Commands:

```text
ultrium test help
ultrium test memory
ultrium test interrupts help
ultrium test interrupts
ultrium test filesystem help
ultrium test filesystem
ultrium test shell
ultrium test all
```

`memory` runs 14 memory and exception descriptor checks. `interrupts` checks that the periodic timer IRQ advances the system tick and that PS/2 IRQ input is enabled without queue overflow. `filesystem` runs eight RAMFS/VFS tests. `shell` runs seven USH cases for native/structured commands, registry/help, compatibility, safe operations, quoting, completion and output contracts. `all` runs all groups (58 cases, including 7 block-device and 20 UltriumFS cases). The legacy `ultrium system memory test` still runs the 128 × 32-byte slab stress check through the runner. Existing `ultrium system memory map test`, `ultrium system memory mapping test`, and `ultrium system memory mapping managed test` remain available.

The suite wraps the existing PMM, heap, page-table, scratch-mapping, and managed-page self-tests. Ownership cases check valid allocate/release cycles, double-free and stale-handle rejection after physical reuse, generation changes, invalid and forged handles, injected exhaustion, and accounting restoration. Other cases check four simultaneous physical allocations, heap block isolation, 32 managed mappings, bounded-pool exhaustion, stale translation removal, and cleanup. The virtual-region case checks single and multi-page mappings, boundary access, noncontiguous physical backing, translation, repeated reuse, invalid frees, both backing-frame and page-table rollback, and exhaustion of all 4096 pages and 64 allocation records. The permission case checks RW/NX defaults, read-only and RX updates, invalid W^X/input rejection, CR0.WP/NXE configuration, and cleanup without intentionally faulting. The RAMFS cases check normalized paths, file contents and EOF, directory iteration, generation-checked handle reuse, allocation rollback, and allocator accounting. The original slab stress code has one implementation shared by both command paths.

## Run in development

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py
python3 tools/qemu-memory-test.py 'ultrium test all'
python3 tools/qemu-memory-test.py 'ultrium test interrupts'
python3 tools/qemu-memory-test.py 'ultrium test filesystem'
python3 tools/qemu-memory-test.py 'ultrium system memory test'
python3 tools/qemu-memory-test.py --keyboard-regression
python3 tools/qemu-memory-test.py --shell-regression
make run TOOLCHAIN=llvm
```

GitHub Actions runs the LLVM build and ISO packaging, then runs `ultrium test all`,
the filesystem suite separately, the legacy memory command, and the shell/keyboard regression. The workflow is in
`.github/workflows/kernel-ci.yml`. It uses Ubuntu 24.04 and downloads Limine and
OVMF release assets through existing Makefile targets; those upstream release
downloads are not pinned by the workflow. A local passing run does not imply
that GitHub Actions has run or passed; check the PR's checks for the hosted result.

The Python script uses only the standard library. It boots UEFI QEMU, types the selected test command twice through QMP keyboard events, reads COM1, and requires two passing summaries in the same boot. In the interrupt-enabled Q35 configuration, this exercises the PS/2 IRQ queue as well as repeated memory cleanup and timer IRQs. `ultrium test interrupts` reports both timer and PS/2 IRQ readiness. The optional `--keyboard-regression` mode edits a command using Home/End, Left/Right, insertion, Backspace and Delete, replays it with Up history, and verifies that Ctrl+C, Ctrl+U, Ctrl+W and Ctrl+L still allow a known test command to execute. Each command must produce its expected COM1 test summary. This test works on the polling-keyboard baseline as well as the later IRQ-backed driver. QEMU must permit a local Unix QMP socket. QMP startup retries are bounded, and the script reports QEMU startup diagnostics. In the interactive QEMU session, check the nested help menus, memory and interrupt status, keyboard editing/history, and mapping commands. These visual interactions are not covered by the serial script.

## Audit and limits

Low-memory exhaustion of all physical frames and generation-counter exhaustion are not run in QEMU. The suite injects a deterministic PMM allocation failure and exercises stale generations across actual frame reuse. Tests do not mutate allocator metadata or map arbitrary physical memory. A failed cleanup is reported, but the current allocator has no recovery facility for every corrupted state. The test suite assumes the current single-CPU kernel and runs after normal initialization.

## USH keyboard regression

`--shell-regression` runs two sessions in one QEMU boot through real PS/2 events. It checks native boot state, alias toggling, quoted names/text, unique command/namespace/path completion, ambiguous completion listing and input restoration, incomplete quote rejection, suggestions, middle editing, history, rejected 71-character input and edited removal confirmations (empty/no/Ctrl+C/yes). It uses explicit `ultrium test shell begin` and `ultrium test shell interactive` diagnostics to capture a bounded 8192-byte terminal-output buffer, verify the resulting VFS fixture and output, clean up, and require restored heap/frame accounting. Capture is off during normal shell use and never mirrors arbitrary commands to COM1. UART still carries test reports only. Any capture overflow fails verification.

To capture the completion listing for visual QA, set `ULTRIUM_QEMU_SCREENSHOT` to an absolute `.ppm` path while running `--shell-regression`. Existing memory/IRQ and keyboard checks remain separate. The harness now fails immediately on a nonzero test-failure summary.

## Power tests

`python3 tools/qemu-power-test.py --shutdown|--reboot|--unsupported` verifies guest power-off, reset and the ACPI-disabled failure path; see `docs/power-management.md`.

## Storage tests

`python3 tools/qemu-storage-test.py --diskless|--disk|--disk-4k|--disk-readonly` exercises the block layer and virtio-blk driver; see `docs/block-devices.md`.

`ultrium test ultriumfs` runs the 20 UltriumFS and VFS-mount cases on memory-backed devices; see `docs/ultriumfs-format.md`.

## Persistence

`python3 tools/qemu-persistence-test.py` runs five QEMU boots on one disposable image; see `docs/persistence-testing.md`. The `ultrium test persist ...` commands it uses need a mounted scratch volume and are not part of `ultrium test all`.
