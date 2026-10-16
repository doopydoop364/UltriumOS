# Power management

Status: implemented and QEMU-tested for Q35 with OVMF (ACPI). Not tested on physical hardware.

## Commands

```text
shutdown                       power off (also: shutdown help)
ultrium system power help
ultrium system power status    mechanism, ACPI details, support state
ultrium system power shutdown  same handler as `shutdown`
reboot                         unchanged: i8042 reset pulse
```

## Mechanism: ACPI S5 soft-off

Nothing is hardcoded to a chipset. At boot `power_init()` (`kernel/src/system/power.c`):

1. Takes the RSDP from the Limine RSDP request. Base revision 4+ returns an HHDM *virtual* address; the code converts it to physical and rejects addresses below the HHDM base.
2. Reads tables through the HHDM only when the whole range lies inside a Limine memory-map entry of type ACPI reclaimable, ACPI NVS or reserved-mapped (the protocol guarantees ACPI tables are there). Lengths are bounded (16 MiB) and checksums verified.
3. Walks the XSDT (RSDT for ACPI 1.0) to the FADT, takes the PM1a/PM1b control ports (only system-I/O space; memory-mapped blocks are refused), and finds the DSDT.
4. Parses `\_S5_` from the DSDT AML (NameOp, PackageOp, byte/zero/one elements) for SLP_TYPa/b.

If any step fails, support is "unavailable" with a specific reason shown by `ultrium system power status`; `shutdown` then reports failure and returns to the shell.

## Shutdown sequence

1. Print status, mask interrupts (no further input is processed).
2. Synchronize persistent filesystems and flush block devices. **Currently none exist**; only the volatile RAMFS is mounted, and the command says so. This is the hook for the storage milestones.
3. Refuse if `SCI_EN` is clear. The kernel does not enable ACPI through the SMI command port, because that was not verified.
4. Write `SLP_EN | SLP_TYP<<10` to PM1a (and PM1b if present).
5. If the platform does not power off within a bounded spin, print failure, restore the interrupt flag and return to the shell.

There is no QEMU-specific fallback port; unsupported hardware fails clearly rather than guessing ports.

## Tests

`tools/qemu-power-test.py` runs QEMU headless with `-no-reboot` and a QMP socket, and uses the `SHUTDOWN` event reason to tell outcomes apart:

| Mode | Expectation |
| --- | --- |
| `--shutdown` | event `guest-shutdown`, QEMU exits by itself, status 0 within a bounded wait |
| `--reboot` | event `guest-reset`, QEMU exits by itself, status 0 |
| `--unsupported` | `-M q35,acpi=off`: no SHUTDOWN event, QEMU still running, shell still answers `ultrium test interrupts` |

A QEMU that must be terminated by the script never counts as a shutdown, and a crash (no event or another reason) fails. Set `ULTRIUM_QEMU_SCREENSHOT=<abs .ppm>` (optionally `ULTRIUM_QEMU_SCREENSHOT_CMD`) to capture a status screen for visual QA.

## Limits

- Only S5 soft-off; no sleep states, no battery/power button events, no ACPI AML interpreter (only the `\_S5_` pattern is read; an `\_S5_` defined elsewhere, e.g. in an SSDT or computed by a method, is reported unsupported).
- Single CPU assumption; no other CPUs to quiesce.
