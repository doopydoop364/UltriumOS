#include <stdint.h>
#include "storage_cmd.h"
#include "block.h"
#include "virtio_blk.h"
#include "pci/pci.h"
#include "terminal/terminal.h"

void storage_write_number(uint64_t value) {
    char digits[21]; unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    char out[22]; unsigned o = 0;
    while (n) out[o++] = digits[--n];
    out[o] = 0;
    terminal_write(out);
}

void storage_cmd_devices(void) {
    size_t count = block_count_devices();
    if (!count) {
        terminal_write("No block devices. Last probe: "); terminal_write(virtio_blk_last_error());
        terminal_write("\nPCI devices on bus 0: "); storage_write_number(pci_device_count());
        terminal_write("\nAttach a disposable virtio-blk disk to QEMU; see docs/block-devices.md.\n");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const struct block_device *d = block_device_at(i);
        terminal_write(d->name); terminal_write("  ");
        terminal_write(d->driver); terminal_write("  ");
        storage_write_number(d->block_count); terminal_write(" x ");
        storage_write_number(d->block_size); terminal_write(" B = ");
        uint64_t bytes = d->block_count * d->block_size;
        if (bytes >= (1ULL << 20)) { storage_write_number(bytes >> 20); terminal_write(" MiB"); }
        else { storage_write_number(bytes >> 10); terminal_write(" KiB"); }
        terminal_write((d->flags & BLOCK_FLAG_READ_ONLY) ? "  read-only" : "  read-write");
        if (d->flags & BLOCK_FLAG_FLUSH) terminal_write("  flush");
        if (d->flags & BLOCK_FLAG_SCRATCH) terminal_write("  scratch");
        if (d->flags & BLOCK_FLAG_DEAD) terminal_write("  DEAD");
        if (d->serial[0]) { terminal_write("  serial="); terminal_write(d->serial); }
        terminal_write("\n");
    }
}
