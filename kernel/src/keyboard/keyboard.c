#include <stdint.h>
#include "keyboard.h"

// QEMU's emulated i8042 controller, set-1 scan codes. Early-boot prototype.
// No USB/HID support, interrupts, or advanced keyboard layout yet.
static inline uint8_t port_in(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

int keyboard_poll(void) {
    if (!(port_in(0x64) & 1)) return 0;
    uint8_t code = port_in(0x60);
    static int left_shift, right_shift, caps_lock, extended;
    int shift;
    if (code == 0xe0) { extended = 1; return 0; }
    if (extended) {
        extended = 0;
        if (code == 0x48) return KEY_UP;
        if (code == 0x50) return KEY_DOWN;
        return 0;
    }
    if (code == 0x2a) { left_shift = 1; return 0; }
    if (code == 0x36) { right_shift = 1; return 0; }
    if (code == 0xaa) { left_shift = 0; return 0; }
    if (code == 0xb6) { right_shift = 0; return 0; }
    if (code == 0x3a) { caps_lock = !caps_lock; return 0; }
    shift = left_shift || right_shift;
    if (code & 0x80 || code == 0xe0 || code == 0xe1) return 0;
    if (code == 0x1c) return '\n';
    if (code == 0x0e) return '\b';
    if (code == 0x39) return ' ';
    static const char plain[58] = {
        [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
        [0x07]='6', [0x08]='7', [0x09]='8', [0x0a]='9', [0x0b]='0',
        [0x0c]='-', [0x0d]='=', [0x10]='q', [0x11]='w', [0x12]='e',
        [0x13]='r', [0x14]='t', [0x15]='y', [0x16]='u', [0x17]='i',
        [0x18]='o', [0x19]='p', [0x1a]='[', [0x1b]=']',
        [0x1e]='a', [0x1f]='s', [0x20]='d', [0x21]='f', [0x22]='g',
        [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l', [0x27]=';',
        [0x28]='\'', [0x29]='`', [0x2b]='\\', [0x2c]='z', [0x2d]='x',
        [0x2e]='c', [0x2f]='v', [0x30]='b', [0x31]='n', [0x32]='m',
        [0x33]=',', [0x34]='.', [0x35]='/'
    };
    static const char shifted[58] = {
        [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
        [0x07]='^', [0x08]='&', [0x09]='*', [0x0a]='(', [0x0b]=')',
        [0x0c]='_', [0x0d]='+', [0x1a]='{', [0x1b]='}',
        [0x27]=':', [0x28]='"', [0x29]='~', [0x2b]='|',
        [0x33]='<', [0x34]='>', [0x35]='?'
    };
    if (code >= sizeof plain) return 0;
    char c = plain[code];
    if ((shift ^ caps_lock) && c >= 'a' && c <= 'z') c -= 'a' - 'A';
    else if (shift && shifted[code]) c = shifted[code];
    return c;
}
