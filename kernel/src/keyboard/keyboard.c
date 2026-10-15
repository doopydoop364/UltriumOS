#include <stdbool.h>
#include <stdint.h>
#include "keyboard.h"
#include "../system/interrupts.h"

/* Set-1 translation remains in the shell's polling context. The IRQ handler
   only moves bytes from the i8042 data port into this bounded SPSC queue. */
enum { KEYBOARD_QUEUE_SIZE = 128 };
static volatile uint8_t scan_queue[KEYBOARD_QUEUE_SIZE];
static volatile uint16_t queue_head;
static volatile uint16_t queue_tail;
static volatile uint32_t dropped_scancodes;
static bool interrupt_input;

static inline uint8_t port_in(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void port_out(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static bool wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000; ++i) {
        if (!(port_in(0x64) & 2)) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static bool wait_output_full(void) {
    for (uint32_t i = 0; i < 100000; ++i) {
        if (port_in(0x64) & 1) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static bool controller_read_config(uint8_t *config) {
    if (!config || !wait_input_clear()) return false;
    port_out(0x64, 0x20);
    if (!wait_output_full()) return false;
    *config = port_in(0x60);
    return true;
}
static bool controller_write_config(uint8_t config) {
    if (!wait_input_clear()) return false;
    port_out(0x64, 0x60);
    if (!wait_input_clear()) return false;
    port_out(0x60, config);
    return wait_input_clear();
}

static void keyboard_irq(void) {
    if (!(port_in(0x64) & 1)) return;
    uint8_t code = port_in(0x60);
    uint16_t next = (uint16_t)((queue_head + 1) % KEYBOARD_QUEUE_SIZE);
    if (next == queue_tail) {
        ++dropped_scancodes;
        return;
    }
    scan_queue[queue_head] = code;
    __asm__ volatile("" ::: "memory");
    queue_head = next;
}

bool keyboard_init_interrupts(void) {
    if (!interrupts_ready()) return false;
    /* Discard stale boot-time bytes before enabling IRQ1. */
    for (unsigned i = 0; i < KEYBOARD_QUEUE_SIZE && (port_in(0x64) & 1); ++i)
        (void)port_in(0x60);

    uint8_t old_config;
    if (!controller_read_config(&old_config)) return false;
    if (!controller_write_config((uint8_t)(old_config | 1u))) return false;

    interrupt_input = true;
    if (!interrupts_register_irq_handler(1, keyboard_irq)) {
        interrupt_input = false;
        (void)controller_write_config(old_config);
        return false;
    }
    return true;
}

bool keyboard_interrupt_input(void) { return interrupt_input; }
uint32_t keyboard_dropped_scancodes(void) { return dropped_scancodes; }

static int translate_scancode(uint8_t code) {
    static int left_shift, right_shift, caps_lock, extended, ctrl;
    int shift;
    if (code == 0xe0) { extended = 1; return 0; }
    if (extended) {
        extended = 0;
        if (code & 0x80) return 0;
        if (code == 0x4b) return KEY_LEFT;
        if (code == 0x4d) return KEY_RIGHT;
        if (code == 0x53) return KEY_DELETE;
        if (code == 0x47) return KEY_HOME;
        if (code == 0x4f) return KEY_END;
        if (code == 0x48) return KEY_UP;
        if (code == 0x50) return KEY_DOWN;
        return 0;
    }
    if (code == 0x1d) { ctrl = 1; return 0; }
    if (code == 0x9d) { ctrl = 0; return 0; }
    if (code == 0x2a) { left_shift = 1; return 0; }
    if (code == 0x36) { right_shift = 1; return 0; }
    if (code == 0xaa) { left_shift = 0; return 0; }
    if (code == 0xb6) { right_shift = 0; return 0; }
    if (code == 0x3a) { caps_lock = !caps_lock; return 0; }
    shift = left_shift || right_shift;
    if (code & 0x80 || code == 0xe0 || code == 0xe1) return 0;
    if (ctrl) {
        if (code == 0x2e) return KEY_CTRL_C;
        if (code == 0x16) return KEY_CTRL_U;
        if (code == 0x11) return KEY_CTRL_W;
        if (code == 0x26) return KEY_CTRL_L;
        return 0;
    }
    if (code == 0x1c) return '\n';
    if (code == 0x0e) return '\b';
    if (code == 0x0f) return '\t';
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

int keyboard_poll(void) {
    uint8_t code;
    if (interrupt_input) {
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        if (queue_tail == queue_head) {
            if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
            return 0;
        }
        code = scan_queue[queue_tail];
        __asm__ volatile("" ::: "memory");
        queue_tail = (uint16_t)((queue_tail + 1) % KEYBOARD_QUEUE_SIZE);
        if (flags & (1ULL << 9)) __asm__ volatile("sti" ::: "memory");
    } else {
        if (!(port_in(0x64) & 1)) return 0;
        code = port_in(0x60);
    }
    return translate_scancode(code);
}
