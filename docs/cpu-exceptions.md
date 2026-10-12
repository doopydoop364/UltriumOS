# Ultrium OS 0.1.0 — CPU exception diagnostics

The first 32 x86-64 exception vectors are installed in a 256-entry Interrupt Descriptor Table (IDT) using the current kernel code segment. The boot sequence installs this IDT before initializing the memory subsystem.

When an exception occurs, the handler prints the vector number, CPU error code, and instruction pointer. Page faults also display CR2 (the faulting virtual address). The kernel then halts: recovery is not supported.

The non-destructive command `ultrium system exceptions` reports whether the IDT was installed. There is deliberately **no command that triggers a page fault**.

## Scope and safety limitations
- Kernel-only, single CPU, no user-mode transitions, no interrupt-service routines (ISRs) for devices, and external interrupts remain disabled.
- No dedicated emergency exception stacks (IST/TSS). Stack corruption or double faults may still cause a triple fault/reboot without a diagnostic.
- No attempt to recover from exceptions or return from the fatal exception stub.
- This creates a diagnostic prerequisite for later controlled page-table changes; **page mappings cannot yet be edited**.
- Must be boot tested in QEMU before merging. Check that the shell and existing memory tests still work.
