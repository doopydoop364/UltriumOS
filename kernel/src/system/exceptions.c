#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "exceptions.h"
#include "terminal/terminal.h"
struct gate { uint16_t low, selector; uint8_t ist, flags; uint16_t middle; uint32_t high, zero; } __attribute__((packed));
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
struct fault_frame { uint64_t vector, error, rip, cs, rflags; };
static struct gate idt[256] __attribute__((aligned(16)));
static bool initialized;
static void print_hex(uint64_t value) {
    terminal_write("0x");
    for (int n = 15; n >= 0; --n) {
        unsigned digit = (unsigned)((value >> (4 * n)) & 15);
        terminal_putc_color((char)(digit < 10 ? '0' + digit : 'A' + digit - 10), TERMINAL_NORMAL);
    }
}
__attribute__((noreturn)) void ultrium_exception_report(const struct fault_frame *f) {
    __asm__ volatile("cli");
    terminal_write_color("\nCPU EXCEPTION - KERNEL HALTED\n", TERMINAL_ACCENT);
    terminal_write("Vector: "); print_hex(f->vector);
    terminal_write("\nError:  "); print_hex(f->error);
    terminal_write("\nRIP:    "); print_hex(f->rip);
    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_write("\nCR2:    "); print_hex(cr2);
    }
    terminal_write("\nReboot to continue.\n");
    for (;;) __asm__ volatile("hlt");
}
__asm__(
".global ultrium_exception_common\n"
"ultrium_exception_common:\n"
"movq %rsp, %rdi\n"
"andq $-16, %rsp\n"
"call ultrium_exception_report\n"
"cli\n"
"1: hlt\n"
"jmp 1b\n"
".global ultrium_exception_0\\n"
"ultrium_exception_0:\\n"
"pushq $0\\n"
"pushq $0\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_1\\n"
"ultrium_exception_1:\\n"
"pushq $0\\n"
"pushq $1\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_2\\n"
"ultrium_exception_2:\\n"
"pushq $0\\n"
"pushq $2\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_3\\n"
"ultrium_exception_3:\\n"
"pushq $0\\n"
"pushq $3\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_4\\n"
"ultrium_exception_4:\\n"
"pushq $0\\n"
"pushq $4\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_5\\n"
"ultrium_exception_5:\\n"
"pushq $0\\n"
"pushq $5\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_6\\n"
"ultrium_exception_6:\\n"
"pushq $0\\n"
"pushq $6\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_7\\n"
"ultrium_exception_7:\\n"
"pushq $0\\n"
"pushq $7\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_8\\n"
"ultrium_exception_8:\\n"
"pushq $8\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_9\\n"
"ultrium_exception_9:\\n"
"pushq $0\\n"
"pushq $9\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_10\\n"
"ultrium_exception_10:\\n"
"pushq $10\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_11\\n"
"ultrium_exception_11:\\n"
"pushq $11\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_12\\n"
"ultrium_exception_12:\\n"
"pushq $12\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_13\\n"
"ultrium_exception_13:\\n"
"pushq $13\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_14\\n"
"ultrium_exception_14:\\n"
"pushq $14\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_15\\n"
"ultrium_exception_15:\\n"
"pushq $0\\n"
"pushq $15\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_16\\n"
"ultrium_exception_16:\\n"
"pushq $0\\n"
"pushq $16\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_17\\n"
"ultrium_exception_17:\\n"
"pushq $17\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_18\\n"
"ultrium_exception_18:\\n"
"pushq $0\\n"
"pushq $18\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_19\\n"
"ultrium_exception_19:\\n"
"pushq $0\\n"
"pushq $19\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_20\\n"
"ultrium_exception_20:\\n"
"pushq $0\\n"
"pushq $20\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_21\\n"
"ultrium_exception_21:\\n"
"pushq $21\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_22\\n"
"ultrium_exception_22:\\n"
"pushq $0\\n"
"pushq $22\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_23\\n"
"ultrium_exception_23:\\n"
"pushq $0\\n"
"pushq $23\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_24\\n"
"ultrium_exception_24:\\n"
"pushq $0\\n"
"pushq $24\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_25\\n"
"ultrium_exception_25:\\n"
"pushq $0\\n"
"pushq $25\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_26\\n"
"ultrium_exception_26:\\n"
"pushq $0\\n"
"pushq $26\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_27\\n"
"ultrium_exception_27:\\n"
"pushq $0\\n"
"pushq $27\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_28\\n"
"ultrium_exception_28:\\n"
"pushq $0\\n"
"pushq $28\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_29\\n"
"ultrium_exception_29:\\n"
"pushq $29\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_30\\n"
"ultrium_exception_30:\\n"
"pushq $30\\n"
"jmp ultrium_exception_common\\n"
".global ultrium_exception_31\\n"
"ultrium_exception_31:\\n"
"pushq $0\\n"
"pushq $31\\n"
"jmp ultrium_exception_common\\n"
);
extern void ultrium_exception_0(void);
extern void ultrium_exception_1(void);
extern void ultrium_exception_2(void);
extern void ultrium_exception_3(void);
extern void ultrium_exception_4(void);
extern void ultrium_exception_5(void);
extern void ultrium_exception_6(void);
extern void ultrium_exception_7(void);
extern void ultrium_exception_8(void);
extern void ultrium_exception_9(void);
extern void ultrium_exception_10(void);
extern void ultrium_exception_11(void);
extern void ultrium_exception_12(void);
extern void ultrium_exception_13(void);
extern void ultrium_exception_14(void);
extern void ultrium_exception_15(void);
extern void ultrium_exception_16(void);
extern void ultrium_exception_17(void);
extern void ultrium_exception_18(void);
extern void ultrium_exception_19(void);
extern void ultrium_exception_20(void);
extern void ultrium_exception_21(void);
extern void ultrium_exception_22(void);
extern void ultrium_exception_23(void);
extern void ultrium_exception_24(void);
extern void ultrium_exception_25(void);
extern void ultrium_exception_26(void);
extern void ultrium_exception_27(void);
extern void ultrium_exception_28(void);
extern void ultrium_exception_29(void);
extern void ultrium_exception_30(void);
extern void ultrium_exception_31(void);
static void (*const stubs[32])(void) = {
ultrium_exception_0,
ultrium_exception_1,
ultrium_exception_2,
ultrium_exception_3,
ultrium_exception_4,
ultrium_exception_5,
ultrium_exception_6,
ultrium_exception_7,
ultrium_exception_8,
ultrium_exception_9,
ultrium_exception_10,
ultrium_exception_11,
ultrium_exception_12,
ultrium_exception_13,
ultrium_exception_14,
ultrium_exception_15,
ultrium_exception_16,
ultrium_exception_17,
ultrium_exception_18,
ultrium_exception_19,
ultrium_exception_20,
ultrium_exception_21,
ultrium_exception_22,
ultrium_exception_23,
ultrium_exception_24,
ultrium_exception_25,
ultrium_exception_26,
ultrium_exception_27,
ultrium_exception_28,
ultrium_exception_29,
ultrium_exception_30,
ultrium_exception_31,
};
bool exceptions_init(void) {
    __asm__ volatile("cli" ::: "memory");
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (size_t i = 0; i < 32; ++i) {
        uintptr_t p = (uintptr_t)stubs[i];
        idt[i] = (struct gate){(uint16_t)p, cs, 0, 0x8e,
            (uint16_t)(p >> 16), (uint32_t)(p >> 32), 0};
    }
    struct idtr d = {(uint16_t)(sizeof(idt) - 1), (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(d) : "memory");
    initialized = true;
    return true;
}
bool exceptions_ready(void) { return initialized; }
