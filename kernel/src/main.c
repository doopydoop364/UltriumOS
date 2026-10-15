#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0
};
__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID, .revision = 0
};

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .c file, as seen fit.

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

// Halt and catch fire function.
static void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}

#include "terminal/terminal.h"
#include "shell.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "system/exceptions.h"
#include "system/interrupts.h"
#include "system/power.h"
#include "pci/pci.h"
#include "storage/block.h"
#include "storage/virtio_blk.h"
#include "keyboard/keyboard.h"
#include "version.h"

void kmain(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) hcf();
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) hcf();
    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
    if (fb->memory_model != LIMINE_FRAMEBUFFER_RGB || fb->bpp != 32) hcf();

    terminal_init(fb);
    terminal_write_color("ULTRIUM", TERMINAL_ACCENT);
    terminal_write(" OS  " ULTRIUM_VERSION "\n");
    terminal_write_color("BUILT TO EXPERIMENT. READY TO EVOLVE.\n\n", TERMINAL_MUTED);
    terminal_write("KERNEL LOADED\n");
    terminal_write("FRAMEBUFFER READY\n");
    terminal_write_color("CLI MODE\n", TERMINAL_ACCENT);
    if (!exceptions_init()) hcf();
    terminal_write("CPU EXCEPTION HANDLERS READY\n");
    if (!memmap_request.response || !hhdm_request.response ||
        !pmm_init(memmap_request.response, hhdm_request.response->offset)) {
        terminal_write_color("PHYSICAL MEMORY INIT FAILED\n", TERMINAL_MUTED);
        hcf();
    }
    vmm_init(hhdm_request.response->offset);
    terminal_write("PHYSICAL MEMORY READY\n");
    /* Base revision 4+ reports the RSDP as an HHDM virtual address. */
    power_init(memmap_request.response, hhdm_request.response->offset,
               rsdp_request.response ? (uint64_t)(uintptr_t)rsdp_request.response->address : 0);
    terminal_write(power_available() ? "ACPI SOFT-OFF READY\n" : "ACPI SOFT-OFF UNAVAILABLE\n");
    terminal_write(vmm_self_test() ? "PAGE TABLE SELF TEST PASSED\n" : "PAGE TABLE SELF TEST FAILED\n");
    terminal_write(vmm_mapping_init() ? "SCRATCH VIRTUAL MAPPING READY\n" : "SCRATCH VIRTUAL MAPPING UNAVAILABLE\n");
    terminal_write(pmm_self_test() ? "MEMORY SELF TEST PASSED\n" : "MEMORY SELF TEST FAILED\n");
    if (!heap_init()) {
        terminal_write_color("KERNEL HEAP INIT FAILED\n", TERMINAL_MUTED);
        hcf();
    }
    terminal_write(heap_self_test() ? "SLAB HEAP SELF TEST PASSED\n" : "SLAB HEAP SELF TEST FAILED\n");
    if (!vfs_init()) {
        terminal_write_color("RAM FILESYSTEM INIT FAILED\n", TERMINAL_MUTED);
        hcf();
    }
    terminal_write("RAM FILESYSTEM READY\n");
    pci_init();
    size_t block_devices = virtio_blk_probe();
    terminal_write(block_devices ? "VIRTIO BLOCK DEVICE READY\n" : "NO BLOCK DEVICE (DISKLESS BOOT)\n");
    if (interrupts_init()) {
        terminal_write("LOCAL APIC, IOAPIC AND 100 HZ TIMER READY\n");
        if (keyboard_init_interrupts())
            terminal_write("PS/2 KEYBOARD IRQ INPUT READY\n");
        else
            terminal_write_color("PS/2 KEYBOARD IRQ UNAVAILABLE; USING POLLED INPUT\n", TERMINAL_MUTED);
    } else {
        terminal_write_color("HARDWARE INTERRUPTS UNAVAILABLE; USING POLLED INPUT\n", TERMINAL_MUTED);
    }
    terminal_write("INTERACTIVE SHELL READY\n\n");
    shell_run();
    hcf();
}
