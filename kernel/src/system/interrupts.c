#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "exceptions.h"
#include "interrupts.h"
#include "../memory/vmm.h"

enum { PIC_MASTER_COMMAND = 0x20, PIC_MASTER_DATA = 0x21,
       PIC_SLAVE_COMMAND = 0xa0, PIC_SLAVE_DATA = 0xa1,
       PIT_COMMAND = 0x43, PIT_CHANNEL0 = 0x40, IRQ_COUNT = 16,
       IRQ_BASE = 0x20, PIT_HZ = 100 };

static volatile uint64_t timer_ticks;
static volatile uint64_t irq_counts[IRQ_COUNT];
static volatile uint64_t last_irq_vector;
static void (*irq_handlers[IRQ_COUNT])(void);
static volatile uint32_t *local_apic;
static volatile uint32_t *io_apic;
static bool initialized;
static void (*tick_hook)(bool from_user);

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline void io_wait(void) { outb(0x80, 0); }

static void pic_remap(void) {
    /* Select APIC mode for the legacy PIC; IOAPIC routes are used directly. */
    outb(0x22, 0x70);
    outb(0x23, 0x01);
    outb(PIC_MASTER_COMMAND, 0x11); io_wait();
    outb(PIC_SLAVE_COMMAND, 0x11); io_wait();
    outb(PIC_MASTER_DATA, IRQ_BASE); io_wait();
    outb(PIC_SLAVE_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC_MASTER_DATA, 4); io_wait();
    outb(PIC_SLAVE_DATA, 2); io_wait();
    outb(PIC_MASTER_DATA, 1); io_wait();
    outb(PIC_SLAVE_DATA, 1); io_wait();
    /* Leave every device masked until it has an installed handler. */
    outb(PIC_MASTER_DATA, 0xff);
    outb(PIC_SLAVE_DATA, 0xff);
}

static void local_apic_eoi(void) {
    if (local_apic) local_apic[0xb0 / sizeof(uint32_t)] = 0;
}

static bool ioapic_route(unsigned gsi, uint8_t vector, uint8_t destination,
                         bool masked) {
    if (!io_apic) return false;
    io_apic[0] = 1;
    uint32_t version = io_apic[4];
    if (gsi > ((version >> 16) & 0xff)) return false;
    unsigned low_register = 0x10 + gsi * 2;
    io_apic[0] = low_register;
    uint32_t low = io_apic[4] | (1u << 16);
    io_apic[4] = low;
    io_apic[0] = low_register + 1;
    io_apic[4] = (uint32_t)destination << 24;
    io_apic[0] = low_register;
    io_apic[4] = (low & ~((1u << 16) | 0xffu)) | vector |
                 (masked ? (1u << 16) : 0);
    return true;
}

static bool ioapic_mask_all(void) {
    if (!io_apic) return false;
    io_apic[0] = 1;
    uint32_t version = io_apic[4];
    unsigned maximum_gsi = (version >> 16) & 0xff;
    for (unsigned gsi = 0; gsi <= maximum_gsi; ++gsi) {
        io_apic[0] = 0x10 + gsi * 2;
        io_apic[4] |= 1u << 16;
    }
    return true;
}

static bool local_apic_prepare(void) {
    uint32_t eax, ebx, ecx, edx;
    eax = 1;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 9))) return false;

    uint32_t msr_low, msr_high;
    __asm__ volatile("rdmsr" : "=a"(msr_low), "=d"(msr_high) : "c"(0x1b));
    uint64_t apic_base = ((uint64_t)msr_high << 32) | msr_low;
    if (apic_base & (1u << 10)) return false; /* x2APIC needs a different access path. */
    if (!(apic_base & (1u << 11))) {
        apic_base |= 1u << 11;
        __asm__ volatile("wrmsr" : : "a"((uint32_t)apic_base),
                         "d"((uint32_t)(apic_base >> 32)), "c"(0x1b) : "memory");
    }

    uint64_t physical = apic_base & 0x0000000ffffff000ULL;
    void *mapped = vmm_map_local_apic(physical);
    if (!mapped) return false;
    local_apic = (volatile uint32_t *)mapped;
    const unsigned svr = 0xf0 / sizeof(uint32_t);
    uint32_t svr_value = local_apic[svr];
    local_apic[svr] = (svr_value & ~0xffu) | 0x1ffu;
    local_apic[0x80 / sizeof(uint32_t)] = 0;

    return true;
}

void ultrium_irq_dispatch(uint64_t vector, const uint64_t *frame) {
    last_irq_vector = vector;
    if (vector < IRQ_BASE || vector >= IRQ_BASE + IRQ_COUNT) {
        local_apic_eoi();
        return;
    }
    unsigned irq = (unsigned)(vector - IRQ_BASE);
    ++irq_counts[irq];
    if (irq == 0) {
        ++timer_ticks;
        local_apic_eoi();                              /* before a possible task switch, or no further tick could arrive */
        /* frame[0]=vector, frame[1]=RIP, frame[2]=CS: ring 3 was interrupted when the CS RPL is nonzero. */
        if (tick_hook) tick_hook((frame[2] & 3) != 0);
        return;
    }
    if (irq_handlers[irq]) irq_handlers[irq]();
    local_apic_eoi();
}

__asm__(
".macro ULTRIUM_IRQ_STUB n\n"
".global ultrium_irq_\\n\n"
"ultrium_irq_\\n:\n"
"pushq $(32+\\n)\n"
"jmp ultrium_irq_common\n"
".endm\n"
"ULTRIUM_IRQ_STUB 0\n" "ULTRIUM_IRQ_STUB 1\n"
"ULTRIUM_IRQ_STUB 2\n" "ULTRIUM_IRQ_STUB 3\n"
"ULTRIUM_IRQ_STUB 4\n" "ULTRIUM_IRQ_STUB 5\n"
"ULTRIUM_IRQ_STUB 6\n" "ULTRIUM_IRQ_STUB 7\n"
"ULTRIUM_IRQ_STUB 8\n" "ULTRIUM_IRQ_STUB 9\n"
"ULTRIUM_IRQ_STUB 10\n" "ULTRIUM_IRQ_STUB 11\n"
"ULTRIUM_IRQ_STUB 12\n" "ULTRIUM_IRQ_STUB 13\n"
"ULTRIUM_IRQ_STUB 14\n" "ULTRIUM_IRQ_STUB 15\n"
".global ultrium_irq_common\n"
"ultrium_irq_common:\n"
"cld\n"
"pushq %rax\n" "pushq %rbx\n" "pushq %rcx\n" "pushq %rdx\n"
"pushq %rbp\n" "pushq %rsi\n" "pushq %rdi\n"
"pushq %r8\n" "pushq %r9\n" "pushq %r10\n" "pushq %r11\n"
"pushq %r12\n" "pushq %r13\n" "pushq %r14\n" "pushq %r15\n"
"movq %rsp, %r12\n"
"movq 120(%r12), %rdi\n"
"leaq 120(%r12), %rsi\n"
"andq $-16, %rsp\n"
"call ultrium_irq_dispatch\n"
"movq %r12, %rsp\n"
"popq %r15\n" "popq %r14\n" "popq %r13\n" "popq %r12\n"
"popq %r11\n" "popq %r10\n" "popq %r9\n" "popq %r8\n"
"popq %rdi\n" "popq %rsi\n" "popq %rbp\n" "popq %rdx\n"
"popq %rcx\n" "popq %rbx\n" "popq %rax\n"
"addq $8, %rsp\n"
"iretq\n");

#define IRQ_STUB_DECL(n) extern void ultrium_irq_##n(void)
IRQ_STUB_DECL(0); IRQ_STUB_DECL(1); IRQ_STUB_DECL(2); IRQ_STUB_DECL(3);
IRQ_STUB_DECL(4); IRQ_STUB_DECL(5); IRQ_STUB_DECL(6); IRQ_STUB_DECL(7);
IRQ_STUB_DECL(8); IRQ_STUB_DECL(9); IRQ_STUB_DECL(10); IRQ_STUB_DECL(11);
IRQ_STUB_DECL(12); IRQ_STUB_DECL(13); IRQ_STUB_DECL(14); IRQ_STUB_DECL(15);
static void (*const irq_stubs[IRQ_COUNT])(void) = {
    ultrium_irq_0, ultrium_irq_1, ultrium_irq_2, ultrium_irq_3,
    ultrium_irq_4, ultrium_irq_5, ultrium_irq_6, ultrium_irq_7,
    ultrium_irq_8, ultrium_irq_9, ultrium_irq_10, ultrium_irq_11,
    ultrium_irq_12, ultrium_irq_13, ultrium_irq_14, ultrium_irq_15
};

bool interrupts_init(void) {
    __asm__ volatile("cli" ::: "memory");
    if (!exceptions_ready()) return false;
    if (!local_apic_prepare()) return false;
    void *ioapic_page = vmm_map_ioapic(0xfec00000ULL);
    if (!ioapic_page) return false;
    io_apic = (volatile uint32_t *)ioapic_page;
    pic_remap();
    if (!ioapic_mask_all()) return false;
    for (unsigned irq = 0; irq < IRQ_COUNT; ++irq)
        if (!exceptions_install_irq_gate(IRQ_BASE + irq, irq_stubs[irq]))
            return false;
    uint16_t divisor = (uint16_t)(1193182u / PIT_HZ);
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xff));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
    uint32_t apic_id = local_apic[0x20 / sizeof(uint32_t)] >> 24;
    if (!ioapic_route(2, IRQ_BASE, (uint8_t)apic_id, false) ||
        !ioapic_route(1, IRQ_BASE + 1, (uint8_t)apic_id, true)) return false;
    initialized = true;
    __asm__ volatile("sti" ::: "memory");
    return true;
}

bool interrupts_ready(void) { return initialized; }
void interrupts_set_tick_hook(void (*hook)(bool from_user)) { tick_hook = hook; }
uint64_t interrupts_timer_ticks(void) { return timer_ticks; }

bool interrupts_register_irq_handler(unsigned irq, void (*handler)(void)) {
    if (!initialized || irq == 0 || irq >= 8 || !handler || irq_handlers[irq])
        return false;
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    uint8_t destination = (uint8_t)(local_apic[0x20 / sizeof(uint32_t)] >> 24);
    bool routed = ioapic_route(irq, (uint8_t)(IRQ_BASE + irq), destination, true);
    if (routed) {
        irq_handlers[irq] = handler;
        routed = ioapic_route(irq, (uint8_t)(IRQ_BASE + irq), destination, false);
        if (!routed) irq_handlers[irq] = 0;
    }
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
    return routed;
}

bool interrupts_self_test(void) {
    if (!initialized) return false;
    uint64_t before = timer_ticks;
    for (volatile uint64_t spins = 0; spins < 100000000; ++spins) {
        if (timer_ticks != before) return true;
        __asm__ volatile("pause");
    }
    return false;
}

uint32_t interrupts_test_status(void) {
    uint8_t mask, irr;
    uint64_t flags;
    __asm__ volatile("inb %1, %0" : "=a"(mask) : "Nd"(PIC_MASTER_DATA));
    outb(PIC_MASTER_COMMAND, 0x0a); /* Read PIC interrupt-request register. */
    __asm__ volatile("inb %1, %0" : "=a"(irr) : "Nd"(PIC_MASTER_COMMAND));
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return (uint32_t)mask | ((uint32_t)irr << 8) |
           ((flags & (1u << 9)) ? (1u << 16) : 0) |
           ((timer_ticks != 0) ? (1u << 17) : 0) |
           ((uint32_t)(last_irq_vector & 0xff) << 24);
}
