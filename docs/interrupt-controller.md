# Hardware interrupt groundwork

The current x86-64 QEMU target uses the local APIC and IOAPIC with the 8259
PIC left remapped and fully masked. Exception vectors occupy 0–31; legacy IRQ
vectors are assigned 32–47. The PIC's output is routed to APIC mode through
the IMCR, while device routes used by the kernel are configured through the
IOAPIC.

The PIT channel 0 is programmed for 100 Hz. On the current Q35 machine, the
MADT legacy IRQ0 source override routes it to IOAPIC GSI 2, vector 32. The
local APIC handles the interrupt and receives an EOI after the short C
dispatcher updates the tick counter. The dispatcher does not allocate, block,
or write to the terminal. Future registered IRQ callbacks must follow the
same rule.

The local APIC MMIO page at `0xFEE00000` and IOAPIC MMIO page at `0xFEC00000`
are mapped by narrow VMM entry points as supervisor read/write, cache-disabled,
non-executable borrowed mappings. They are reserved hardware pages and are
never PMM-owned or released. These mappings occupy two fixed virtual pages
immediately after the bounded VM-region allocation window.

## Limits

- Routing currently assumes the Q35 legacy IRQ0-to-GSI2 override and the
  standard IOAPIC address. ACPI MADT parsing and general GSI routing are
  future work; other firmware routing layouts are unsupported.
- x2APIC mode is rejected. APIC ID routing targets the boot CPU only.
- The kernel remains single-CPU. Handler registration preserves local
  interrupt state but does not provide cross-CPU synchronization.
- The keyboard still uses the existing polling i8042 driver in this milestone.
  IRQ1 support is exposed for a later keyboard integration, but is not enabled
  until a handler is registered.
- The timer is a tick source only. It does not schedule tasks or provide a
  calibrated wall clock.
- If APIC setup fails, the kernel keeps the CLI available with polled input;
  `ultrium system interrupts` reports that interrupt mode is unavailable.

Run the focused and full tests with:

```sh
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py 'ultrium test interrupts'
python3 tools/qemu-memory-test.py 'ultrium test all'
```
