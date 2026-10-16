# Component architecture: Kernel, OS, GUI

Status: **architecture and migration plan, plus ownership rules**. No code has been moved. Today everything below the "Kernel" and "OS" rows still compiles into one freestanding kernel image running in ring 0, so none of the boundaries here are enforced by hardware. Do not describe them as isolation.

## The three components

| Component | Scope | Today |
| --- | --- | --- |
| **Ultrium Kernel** | Boot handoff, CPU/GDT/IDT/exceptions, PMM/heap/VMM, interrupts and timer, drivers, block devices, scheduling and system-call infrastructure | PMM, heap, VMM, exceptions, APIC timer, PS/2 and ACPI power exist. Since 0.7.0: scheduler, per-process address spaces, ring 3, syscall ABI 1.0, ELF loader (single CPU, no SMP, kernel not preemptible) |
| **Ultrium OS** | Ultrium Shell (USH), system services, configuration, permissions, applications, AI integration | USH, command registry and the CLI tests live in `kernel/src/` as kernel code; sample programs and the runtime live in `os/` and run in ring 3. VFS/RAMFS is kernel-resident. Permissions (`auth`/`as`) and AI are unavailable by design |
| **Ultrium GUI** | Optional desktop, compositor, window management, themes, graphical apps | Not started. The CLI must remain fully useful without it |

Dependency direction is strictly downward: GUI depends on OS, OS depends on Kernel, never the reverse. The kernel must boot and be useful with neither above it.

## Boundaries and APIs

- **Kernel public surface** (`kernel/src/<subsystem>/*.h`): allocator handles (PMM generation handles), VMM region API, VFS and (future) block-device API, interrupt registration, power. Header changes are public-interface changes (see ownership rules).
- **Kernel to OS** will become a system-call ABI once a ring-3 boundary exists. Until then OS code calls kernel headers directly. This is a *source-level* seam, not isolation; a bug in USH can still corrupt the kernel.
- **OS to GUI** will be a protocol (surface/buffer/input events) over an OS-provided channel. It does not exist yet and is not designed in detail here.
- **Shared interface directory** `shared/` (not created yet): create it only when a real second consumer needs a stable, versioned definition, for example syscall numbers, error codes, on-disk structure layouts or a GUI protocol. Contents must be freestanding C headers with explicit-width types, no kernel internals and a version constant. Do not pre-populate it speculatively.

## Build structure (target)

```text
kernel/   Ultrium Kernel   -> /boot/kernel   (exists)
os/       Ultrium OS       -> userspace runtime and programs (exists; embedded into the kernel image and installed in /bin)
gui/      Ultrium GUI      -> optional image (not created)
shared/   ABI/protocol headers (shared/abi/ultrium_syscall.h exists: kernel and userspace both consume it)
```

Each component gets its own makefile and tests; the root `GNUmakefile` composes the ISO. Until userspace exists, `os/` cannot be built separately from the kernel image, so creating the directory now would only imply a separation that does not hold. The existing build (`make TOOLCHAIN=llvm`, `limine.conf`, `/boot/kernel`) is unchanged by this document.

## Migration strategy

Each step must land as its own reviewable PR with the full regression suite green, and none may delay the active storage milestones.

1. **Inventory (this PR):** classify existing code by component (below) and agree ownership rules.
2. **Seams in place:** within `kernel/src/`, keep OS-side code (`ush.c`, `shell.c`, command registry, `tests` for USH) from reaching into kernel internals beyond public headers. Enforce with review first; add a header-include check only once it can run in CI without false positives.
3. **Directory split inside the kernel tree** (e.g. `kernel/src/os/`) only if the existing tests prove the build and boot are unchanged. This is organizational, not isolation.
4. **Syscall ABI and ring-3 prerequisites:** address spaces, a scheduler, a trap path and a capability/permission design (see `authorization-architecture.md`). Only then define `shared/` syscall headers.
5. **Move USH and services to userspace** one at a time, with the kernel-resident implementation retained until the userspace one passes the same QEMU regression tests.
6. **GUI** starts only after a stable userspace and a display protocol exist.

Risks: moving code early creates the illusion of isolation; a full rewrite would freeze filesystem work and invalidate working tests; `shared/` created early becomes an unreviewed dumping ground.

## Current classification

| Path | Component | Notes |
| --- | --- | --- |
| `kernel/src/main.c`, `memory/`, `system/` (exceptions, interrupts, power, reboot), `keyboard/`, `terminal/`, `pci/`, `storage/` (block layer, virtio-blk) | Kernel | terminal, keyboard and storage are drivers today |
| `kernel/src/fs/` (VFS, RAMFS, UltriumFS) | Kernel | kernel-only filesystems; the storage shell commands in `storage/storage_ops.c` are OS-facing but still kernel-resident |
| `kernel/src/ush.c`, `shell.c` | OS (kernel-resident) | first candidates for userspace migration |
| `kernel/src/tests/` | Kernel test harness, with OS-facing cases | |
| `tools/`, `docs/` | shared tooling | |
| none | GUI | not started |

## Parallel work

Components are not yet independently buildable, so parallel agents work on separate subsystems of the same tree. See the ownership rules in `AGENTS.md`.
