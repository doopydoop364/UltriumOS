#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "exceptions.h"
#include "terminal/terminal.h"
struct gate { uint16_t low, selector; uint8_t ist, flags; uint16_t middle; uint32_t high, zero; } __attribute__((packed));
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));
struct gdtr { uint16_t limit; uint64_t base; } __attribute__((packed));
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2, reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3, io_map_base;
} __attribute__((packed));
struct fault_frame { uint64_t vector, error, rip, cs, rflags; };
static struct gate idt[256] __attribute__((aligned(16)));
static uint64_t gdt[5] __attribute__((aligned(16)));
static struct tss64 tss __attribute__((aligned(16)));
static uint8_t double_fault_stack[16 * 1024] __attribute__((aligned(16)));
static bool initialized;
static bool tss_initialized;

/* Reload CS after installing our private GDT; Limine's GDT is borrowed and
   may not have a free descriptor slot for the TSS. */
__asm__(
".global ultrium_load_gdt\n"
"ultrium_load_gdt:\n"
"lgdt (%rdi)\n"
"pushq $0x08\n"
"leaq 1f(%rip), %rax\n"
"pushq %rax\n"
"lretq\n"
"1: movw $0x10, %ax\n"
"movw %ax, %ds\n"
"movw %ax, %es\n"
"movw %ax, %ss\n"
"retq\n");
extern void ultrium_load_gdt(const struct gdtr *descriptor);
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
    static const char *const names[32] = {
        "Divide Error", "Debug", "Non-maskable Interrupt", "Breakpoint",
        "Overflow", "Bound Range", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
        "Segment Not Present", "Stack Fault", "General Protection",
        "Page Fault", "Reserved", "x87 Floating Point", "Alignment Check",
        "Machine Check", "SIMD Floating Point", "Virtualization",
        "Control Protection", "Reserved", "Reserved", "Reserved", "Reserved",
        "Reserved", "Reserved", "Hypervisor Injection", "VMM Communication",
        "Security", "Reserved"
    };
    terminal_write("Exception: ");
    if (f->vector < 32) terminal_write(names[f->vector]);
    else terminal_write("Unknown");
    terminal_write("\nVector: "); print_hex(f->vector);
    terminal_write("\nError:  "); print_hex(f->error);
    terminal_write("\nRIP:    "); print_hex(f->rip);
    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        terminal_write("\nCR2:    "); print_hex(cr2);
        terminal_write("\nPF bits:");
        terminal_write((f->error & 1) ? " protection" : " not-present");
        if (f->error & 2) terminal_write(" write");
        else terminal_write(" read");
        if (f->error & 4) terminal_write(" user");
        if (f->error & 8) terminal_write(" reserved-bit");
        if (f->error & 16) terminal_write(" instruction-fetch");
    }
    terminal_write("\nCS:     "); print_hex(f->cs);
    terminal_write("\nRFLAGS: "); print_hex(f->rflags);
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
".global ultrium_exception_0\n"
"ultrium_exception_0:\n"
"pushq $0\n"
"pushq $0\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_1\n"
"ultrium_exception_1:\n"
"pushq $0\n"
"pushq $1\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_2\n"
"ultrium_exception_2:\n"
"pushq $0\n"
"pushq $2\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_3\n"
"ultrium_exception_3:\n"
"pushq $0\n"
"pushq $3\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_4\n"
"ultrium_exception_4:\n"
"pushq $0\n"
"pushq $4\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_5\n"
"ultrium_exception_5:\n"
"pushq $0\n"
"pushq $5\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_6\n"
"ultrium_exception_6:\n"
"pushq $0\n"
"pushq $6\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_7\n"
"ultrium_exception_7:\n"
"pushq $0\n"
"pushq $7\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_8\n"
"ultrium_exception_8:\n"
"pushq $8\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_9\n"
"ultrium_exception_9:\n"
"pushq $0\n"
"pushq $9\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_10\n"
"ultrium_exception_10:\n"
"pushq $10\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_11\n"
"ultrium_exception_11:\n"
"pushq $11\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_12\n"
"ultrium_exception_12:\n"
"pushq $12\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_13\n"
"ultrium_exception_13:\n"
"pushq $13\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_14\n"
"ultrium_exception_14:\n"
"pushq $14\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_15\n"
"ultrium_exception_15:\n"
"pushq $0\n"
"pushq $15\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_16\n"
"ultrium_exception_16:\n"
"pushq $0\n"
"pushq $16\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_17\n"
"ultrium_exception_17:\n"
"pushq $17\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_18\n"
"ultrium_exception_18:\n"
"pushq $0\n"
"pushq $18\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_19\n"
"ultrium_exception_19:\n"
"pushq $0\n"
"pushq $19\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_20\n"
"ultrium_exception_20:\n"
"pushq $0\n"
"pushq $20\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_21\n"
"ultrium_exception_21:\n"
"pushq $21\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_22\n"
"ultrium_exception_22:\n"
"pushq $0\n"
"pushq $22\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_23\n"
"ultrium_exception_23:\n"
"pushq $0\n"
"pushq $23\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_24\n"
"ultrium_exception_24:\n"
"pushq $0\n"
"pushq $24\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_25\n"
"ultrium_exception_25:\n"
"pushq $0\n"
"pushq $25\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_26\n"
"ultrium_exception_26:\n"
"pushq $0\n"
"pushq $26\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_27\n"
"ultrium_exception_27:\n"
"pushq $0\n"
"pushq $27\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_28\n"
"ultrium_exception_28:\n"
"pushq $0\n"
"pushq $28\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_29\n"
"ultrium_exception_29:\n"
"pushq $29\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_30\n"
"ultrium_exception_30:\n"
"pushq $30\n"
"jmp ultrium_exception_common\n"
".global ultrium_exception_31\n"
"ultrium_exception_31:\n"
"pushq $0\n"
"pushq $31\n"
"jmp ultrium_exception_common\n"
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
    /* Flat 64-bit kernel code/data descriptors followed by a 64-bit TSS. */
    gdt[0] = 0;
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    tss = (struct tss64){0};
    tss.ist1 = (uint64_t)(uintptr_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.io_map_base = sizeof(tss);
    uint64_t tss_base = (uint64_t)(uintptr_t)&tss;
    uint64_t tss_limit = sizeof(tss) - 1;
    gdt[3] = (tss_limit & 0xffff) | ((tss_base & 0xffffff) << 16) |
             (0x89ULL << 40) | (((tss_limit >> 16) & 0xf) << 48) |
             (((tss_base >> 24) & 0xff) << 56);
    gdt[4] = tss_base >> 32;
    struct gdtr gdtr = {(uint16_t)(sizeof(gdt) - 1), (uint64_t)(uintptr_t)gdt};
    ultrium_load_gdt(&gdtr);
    uint16_t tss_selector = 0x18;
    __asm__ volatile("ltr %0" : : "rm"(tss_selector) : "memory");
    uint16_t cs;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    for (size_t i = 0; i < 32; ++i) {
        uintptr_t p = (uintptr_t)stubs[i];
        uint8_t ist = i == 8 ? 1 : 0;
        idt[i] = (struct gate){(uint16_t)p, cs, ist, 0x8e,
            (uint16_t)(p >> 16), (uint32_t)(p >> 32), 0};
    }
    struct idtr d = {(uint16_t)(sizeof(idt) - 1), (uint64_t)(uintptr_t)idt};
    __asm__ volatile("lidt %0" : : "m"(d) : "memory");
    tss_initialized = true;
    initialized = true;
    return true;
}
bool exceptions_ready(void) { return initialized; }
bool exceptions_self_test(void) {
    uint16_t tr;
    __asm__ volatile("str %0" : "=rm"(tr));
    return initialized && tss_initialized && tr == 0x18 &&
           (idt[8].ist & 7) == 1 && (idt[8].flags & 0x8f) == 0x8e;
}
