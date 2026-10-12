#include <stdint.h>
#include "reboot.h"

/* i8042 reset pulse. Supported by QEMU's emulated controller.
   A hardware-independent reboot API can replace this as platforms grow. */
static uint8_t in8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void out8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
void system_reboot(void) {
    for (unsigned i = 0; i < 1000000; ++i) {
        if (!(in8(0x64) & 0x02)) {
            out8(0x64, 0xfe);
            break;
        }
    }
    for (;;) __asm__ volatile ("cli; hlt");
}
