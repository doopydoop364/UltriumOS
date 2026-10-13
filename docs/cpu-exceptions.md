# CPU exception handling

Ultrium installs exception gates for vectors 0–31 and uses a private flat
kernel GDT. The GDT includes a 64-bit TSS descriptor; the kernel loads it into
TR during exception initialization. The TSS has a statically reserved,
16 KiB, 16-byte-aligned emergency stack for the double-fault gate (IST1).
Static storage is used because exception initialization happens before the PMM
and heap are available. The stack is kernel image storage and is never passed
to the physical allocator.

The fatal exception path reports the exception name, vector, error code, RIP,
CS, and RFLAGS. Page faults also report CR2 and the architectural error bits.
It disables interrupts and halts; recovery is not supported. `ultrium system
exceptions` reports IDT initialization. The kernel test suite checks that TR
contains the expected TSS selector and that vector 8 selects IST1. This check
validates descriptor setup without intentionally faulting the kernel.

## Limits and safety

- Kernel-only, single CPU, and no privilege-level transitions. RSP0 is not yet
  used for entering from user mode.
- Only double faults use an emergency IST stack. NMI and machine-check stacks
  remain future work.
- Exception reporting still uses the framebuffer terminal. A fault inside the
  terminal path, a corrupted GDT/IDT, or failure of the emergency stack can
  still lead to a triple fault.
- The test suite does not force a double fault. Validating that delivery path
  needs an isolated QEMU fault-test configuration with an expected halt and
  timeout.
- External interrupts remain disabled; device IRQ routing and scheduling are
  not implemented.

Build and test with:

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py 'ultrium test all'
```
