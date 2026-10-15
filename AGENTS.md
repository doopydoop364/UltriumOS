# Ultrium OS development guide

## Project and current state

Ultrium OS is an independent, CLI-first operating system with an x86-64 freestanding C kernel booted by Limine. It is not a Linux distribution. The CLI must remain useful without a desktop, network, or AI service. Current development is experimental; do not describe future desktop, application, AI, or filesystem plans as implemented features.

The root `GNUmakefile` builds a Limine ISO and runs QEMU. `kernel/GNUmakefile` compiles the freestanding kernel with Clang when `TOOLCHAIN=llvm`. `limine.conf` selects `/boot/kernel`. `kernel/src/main.c` owns Limine requests and boot initialization. The framebuffer terminal is in `kernel/src/terminal/`; the interrupt-driven PS/2 keyboard and interactive shell are in `kernel/src/keyboard/` and `kernel/src/shell.c`, with polling fallback when APIC setup is unavailable. Memory code is in `kernel/src/memory/`: PMM, slab heap, and VMM. CPU exception diagnostics are in `kernel/src/system/exceptions.c`; IRQ and timer setup are in `kernel/src/system/interrupts.c`. VFS and volatile RAMFS are in `kernel/src/fs/`. Read the focused files and `docs/` before editing.

At the v0.4.0 development milestone, the PMM allocates 4 KiB frames from Limine usable ranges through generation-bearing ownership handles. Its in-band per-frame state uses 32-bit generation records and reserves about 1 MiB per GiB of usable memory. Physical addresses used by mappings are borrowed values and cannot be released through an address-only API. Handles are move-only by convention and the allocators remain single-CPU and unsynchronized. The heap retains each slab frame's owner handle; the VMM retains handles for scratch tables, managed mappings, region backing, and owned page tables. Limine's active root and borrowed page-table frames are never released. The VMM provides bounded kernel virtual regions with page permissions. These remain bootstrap facilities, not general address spaces. The GDT/TSS reserves a double-fault IST stack; the IDT handles vectors 0–31. QEMU Q35 uses local APIC/IOAPIC routing for a periodic PIT tick and PS/2 keyboard input, with polled input fallback if interrupt setup fails. The timer does not schedule tasks. A kernel-only VFS currently mounts one volatile RAMFS at `/`; no persistent storage or userspace filesystem access exists. See `docs/physical-memory.md`, `docs/slab-heap.md`, `docs/page-table-inspection.md`, `docs/virtual-page-mapping.md`, `docs/managed-virtual-pages.md`, `docs/virtual-memory-architecture.md`, `docs/page-permissions.md`, `docs/cpu-exceptions.md`, `docs/interrupt-controller.md`, and `docs/ramfs.md` for details.

## Work sequence

1. Check `git status --short --branch`, recent commits, local and remote refs, and relevant PRs. Preserve unexpected changes; do not overwrite another contributor's work. Verify whether a dependency branch is merged before choosing a base.
2. Read the code, build rules, and relevant docs. Confirm the feature is not already present. Make a small, reviewable change on a descriptive feature branch.
3. Build after every kernel change with `make TOOLCHAIN=llvm`. A cached build alone does not verify newly edited source. Resolve all compiler errors and review warnings.
4. Run QEMU when available (`make run TOOLCHAIN=llvm`) and exercise the affected CLI paths. Use any documented automated smoke test plus precise manual checks for behavior automation cannot cover. Report commands, observed results, failures, and untested areas separately.
5. Review the diff for memory ownership, partial-failure cleanup, invalid inputs, and shell/boot regressions. Update focused docs. Open a draft PR for major work against `main`; leave merging to the maintainer. Do not push untested kernel changes directly to `main`.

Do not install system packages without permission. Host shell choice may vary; Make recipes use `sh`, while developer commands should not require an interactive Bash configuration.

## Kernel rules

- Use freestanding facilities only. Include the needed freestanding headers explicitly; do not assume a hosted C library, processes, or filesystem.
- Keep allocation ownership explicit: physical backing, mappings, page-table frames, who frees each resource, and what happens after partial failure. Check alignment, range bounds, and arithmetic overflow where relevant.
- Handle x86-64 canonical addresses, paging level, large pages, TLB invalidation, CPU-maintained page-table Accessed/Dirty bits, privilege, interrupt state, and physical versus virtual addresses deliberately. Do not treat a convenient address range as free without inspecting active mappings.
- Keep diagnostic code safe in exception or memory-failure paths. Do not return success without checking the intended result. Avoid broad rewrites of working components.
- Existing PMM, heap, and mapping code assumes one CPU. Do not imply concurrency safety, process isolation, dynamic page-table reclamation, or arbitrary virtual-range ownership.

## CLI and visual conventions

General shell commands are unprefixed (`help`, `clear`, `clear history`, `reboot`). OS built-ins use `ultrium`; future applications will use their own executable names. AI commands, if any, belong under `ultrium ai`, and the current status must say that AI services are absent. Each multiword namespace needs its own `help` command, dispatch entry, and a link from its parent menu. Parent menus should list immediate children rather than flattening a deep tree. Preserve command normalization, history, cursor editing, and keyboard shortcuts. See `docs/cli-architecture.md` and `docs/command-namespaces.md`; verify claims in older docs against code.

PULSE is the visual identity; the product is Ultrium OS. Preserve the black kernel terminal, VGA 8x16 glyphs, warm white `#F3F1EC`, signal red `#FF5A36`, and muted gray `#8A8F98`. The working tagline is “Built to experiment. Ready to evolve.” Do not invent or replace an official logo. See `docs/pulse-console-branding.md`. No separate brand kit or Signal U SVG is present in this repository snapshot.

## Roadmap and testing

The planned order is **Phase A (memory) → Phase C (interrupt and hardware infrastructure) → Phase B (filesystem)**. Bounded kernel memory regions and ownership checks are implemented, but general address spaces are not. Phase C currently includes the double-fault IST, Q35 APIC/IOAPIC timer routing, and interrupt-driven PS/2 input; robust recovery, scheduling, SMP, and general hardware routing remain future work. Phase B currently includes a small kernel-only VFS and volatile RAMFS; block devices and persistent filesystems remain future work. Desktop, native apps, custom hardware edition, and optional AI systems are long-term concepts.

Memory tests should cover distinct allocations, data isolation, accounting, reuse, invalid input, exhaustion where safely bounded, translation, unmapping, and cleanup. Do not deliberately corrupt unrelated kernel memory. Preserve boot, terminal, keyboard, shell, help, heap, page-table inspection, mapping, and existing diagnostics. Distinguish **implemented**, **compiled**, **QEMU-tested**, and **merged** in reports. Include exact maintainer commands, limits, remaining risks, branch, and PR URL.

The current memory suite and headless QEMU procedure are documented in `docs/kernel-testing.md`; run them when changing allocator or mapping code.

## Ultrium Shell conventions

USH has a component version separate from the kernel. Native filesystem verbs are unprefixed (`list`, `go`, `where`, `new file/folder`, `view`, `write`, `remove`, `info`); Linux-style aliases are opt-in and session-scoped. Keep aliases and native operations on shared handlers; do not imply POSIX compatibility. Metadata must come from the VFS. See `docs/ultrium-shell.md`.

Keep command routing and hierarchical help in the static USH registry; every path supports trailing `help`, and parent help lists immediate children. Register new commands with usage, argument bounds and shared handlers. Preserve the `ultrium system` hierarchy.

Removal never recurses without `-r`/`--recursive`; `-y`/`--yes` only skips confirmation. Protect root/cwd/ancestors. Use bounded iterative traversal, report partial deletion, and define mount/link policies before adding those features. Transfers refuse existing destinations; use VFS copy/rename, never bypass RAMFS ownership. Kernel callers pass canonical absolute paths; shell actions use `vfs_resolve_path` to validate intermediate traversal.

USH parsing must preserve argument case/quoted whitespace, reject malformed or over-limit input before execution, and keep completion bounded. Command completion/help use registry metadata; path completion uses VFS enumeration. Preserve input when completion cannot fit. Run both `--keyboard-regression` and `--shell-regression` after editor/parser changes; diagnostic capture is bounded and off outside explicit tests.

`auth` and `as` are reserved design concepts, currently unavailable. Never add a shell-only privilege toggle or imitation password prompt. Authorization requires enforced kernel/process/VFS boundaries and trusted human approval separate from an agent. See `docs/authorization-architecture.md`; `-y` is never authorization.

## Component ownership and parallel work

Ultrium is organized into **Ultrium Kernel**, **Ultrium OS** and **Ultrium GUI**; see `docs/component-architecture.md`. Dependencies point downward only (GUI -> OS -> Kernel). Today everything except documentation still builds into one ring-0 kernel image, so these are ownership boundaries, not isolation. Never claim userspace isolation because code lives in a different directory.

- **Ownership:** Kernel: boot, CPU, memory, interrupts, drivers, block devices, VFS and filesystems, power. OS: USH, command registry, system services, configuration, permissions, AI. GUI: not started.
- **Worktrees and branches:** each agent uses its own `git worktree` and a separate descriptive feature branch per subsystem. Do not edit another agent's worktree or branch. Stack PRs on their immediate predecessor and say so in the PR.
- **Public-interface review:** a change to a subsystem header, the command registry/help contract, an on-disk format, or (once it exists) `shared/` needs its own called-out section in the PR, updated docs, and maintainer review. Do not change another component's public interface as a side effect.
- **Subsystem tests:** run the tests for the subsystem touched plus the full suite before opening a PR: memory/allocator (`ultrium test memory`), interrupts, filesystem (`ultrium test filesystem`), shell and keyboard (`--shell-regression`, `--keyboard-regression`), power (`tools/qemu-power-test.py`, when present). See `docs/kernel-testing.md`.
- **Structural moves:** relocate code between directories only in a separate small PR whose build and QEMU tests prove it is a no-op. Do not undertake a full rewrite.
- **`shared/`:** create only when a second consumer needs a stable ABI or protocol; freestanding, explicit-width, versioned.
