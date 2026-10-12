#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests.h"
#include "terminal/terminal.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"

/* COM1 is only used for test results. A missing UART never blocks the CLI. */
static bool serial_ready;
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static void serial_init(void) {
    outb(0x3f9, 0); outb(0x3fb, 0x80);
    outb(0x3f8, 1); outb(0x3f9, 0);
    outb(0x3fb, 3); outb(0x3fa, 0xc7); outb(0x3fc, 0x0b);
    serial_ready = inb(0x3fd) != 0xff;
}
static void report(const char *s) {
    terminal_write(s);
    if (!serial_ready) return;
    for (; *s; ++s) {
        /* Bound polling so a faulty UART cannot hang a kernel test. */
        unsigned spins = 100000;
        while (!(inb(0x3fd) & 0x20) && --spins) { }
        if (!spins) { serial_ready = false; return; }
        if (*s == '\n') outb(0x3f8, '\r');
        outb(0x3f8, (uint8_t)*s);
    }
}
static void report_number(uint64_t value) {
    char digits[21]; size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) { char c[2] = {digits[--n], 0}; report(c); }
}

/* A NULL reason means pass. Tests own their cleanup and must leave the
   allocator counters at their entry values, including after failure. */
typedef const char *(*test_fn)(void);
struct test_case { const char *name; test_fn run; };
static const char *physical_self(void) { return pmm_self_test() ? 0 : "PMM self-test failed"; }
static const char *heap_self(void) { return heap_self_test() ? 0 : "heap self-test failed"; }
static const char *table_self(void) { return vmm_self_test() ? 0 : "HHDM translation failed"; }
static const char *mapping_self(void) { return vmm_map_test() ? 0 : "scratch mapping stage"; }
static const char *managed_self(void) { return vmm_managed_test() ? 0 : "managed mapping stage"; }

static const char *physical_edges(void) {
    uint64_t before = pmm_free_pages();
    uint64_t pages[4] = {0};
    const char *error = 0;
    if (pmm_free_page(0) || pmm_free_page(1)) return "invalid physical free accepted";
    for (unsigned i = 0; i < 4; ++i) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) { error = "four-page allocation failed"; break; }
        for (unsigned j = 0; j < i; ++j)
            if (pages[i] == pages[j]) error = "duplicate physical frame";
    }
    if (!error && pmm_free_pages() + 4 != before) error = "PMM debit mismatch";
    if (pages[1]) {
        if (!pmm_free_page(pages[1])) error = "physical free failed";
        else {
            uint64_t reused = pmm_alloc_page();
            if (reused != pages[1]) {
                error = "released frame was not reused";
                pages[1] = 0; /* That frame is on the free stack. */
                if (reused && !pmm_free_page(reused)) error = "physical cleanup failed";
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i)
        if (pages[i] && !pmm_free_page(pages[i])) error = "physical cleanup failed";
    if (pmm_free_pages() != before) error = "PMM accounting not restored";
    return error;
}

static const char *heap_edges(void) {
    uint64_t before = pmm_free_pages(), live = heap_live_allocations();
    void *blocks[32] = {0};
    const char *error = 0;
    if (kmalloc(0) || kmalloc(4097)) return "invalid heap size accepted";
    for (unsigned i = 0; i < 32; ++i) {
        blocks[i] = kmalloc(32);
        if (!blocks[i]) { error = "small allocation failed"; break; }
        ((uint8_t *)blocks[i])[0] = (uint8_t)(i + 1);
    }
    if (!error) for (unsigned i = 0; i < 32; ++i)
        if (((uint8_t *)blocks[i])[0] != (uint8_t)(i + 1)) error = "heap blocks overlap";
    for (unsigned i = 0; i < 32; ++i) if (blocks[i]) kfree(blocks[i]);
    if (pmm_free_pages() != before || heap_live_allocations() != live)
        error = "heap cleanup accounting mismatch";
    return error;
}

/* Preserve the historical 128 x 32-byte shell stress test. */
static const char *slab_stress(void) {
    enum { COUNT = 128 };
    void *blocks[COUNT] = {0};
    uint64_t before = pmm_free_pages(), live = heap_live_allocations();
    unsigned allocated = 0;
    const char *error = 0;
    for (unsigned i = 0; i < COUNT; ++i) {
        blocks[i] = kmalloc(32);
        if (!blocks[i]) { error = "slab allocation failed"; break; }
        ++allocated;
        for (unsigned j = 0; j < 32; ++j)
            ((uint8_t *)blocks[i])[j] = (uint8_t)(i ^ j ^ 0x5a);
    }
    if (!error && pmm_free_pages() + 1 != before) error = "slab page count mismatch";
    for (unsigned i = 0; i < allocated; ++i)
        for (unsigned j = 0; j < 32; ++j)
            if (((uint8_t *)blocks[i])[j] != (uint8_t)(i ^ j ^ 0x5a)) error = "slab data mismatch";
    for (unsigned i = 0; i < allocated; ++i) kfree(blocks[i]);
    if (pmm_free_pages() != before || heap_live_allocations() != live)
        error = "slab cleanup accounting mismatch";
    void *again = kmalloc(32);
    if (!again) error = "slab reuse failed";
    kfree(again);
    if (pmm_free_pages() != before || heap_live_allocations() != live)
        error = "slab reuse cleanup mismatch";
    return error;
}

static const char *managed_edges(void) {
    if (vmm_managed_live()) return "managed pool already in use";
    uint64_t before = pmm_free_pages();
    void *pages[32] = {0};
    const char *error = 0;
    if (vmm_managed_free(0) || vmm_managed_free((void *)(uintptr_t)1))
        return "invalid managed free accepted";
    for (unsigned i = 0; i < 32; ++i) {
        pages[i] = vmm_managed_alloc();
        if (!pages[i]) { error = "managed pool filled early"; break; }
        ((uint8_t *)pages[i])[0] = (uint8_t)(i + 1);
    }
    if (!error) {
        void *extra = vmm_managed_alloc();
        if (extra) {
            error = "managed pool overflow";
            if (!vmm_managed_free(extra)) error = "overflow cleanup failed";
        }
    }
    if (!error) for (unsigned i = 0; i < 32; ++i)
        if (((uint8_t *)pages[i])[0] != (uint8_t)(i + 1)) error = "managed pages overlap";
    for (unsigned i = 0; i < 32; ++i) if (pages[i]) {
        uint64_t address = (uint64_t)(uintptr_t)pages[i], ignored;
        if (!vmm_managed_free(pages[i])) error = "managed free failed";
        else if (vmm_translate(address, &ignored)) error = "stale managed mapping";
    }
    if (vmm_managed_live() || pmm_free_pages() != before)
        error = "managed cleanup accounting mismatch";
    if (pages[0] && vmm_managed_free(pages[0])) error = "double managed free accepted";
    return error;
}

static const struct test_case memory_cases[] = {
    {"physical self", physical_self}, {"physical edges", physical_edges},
    {"heap self", heap_self}, {"heap edges", heap_edges},
    {"slab stress", slab_stress}, {"page tables", table_self},
    {"scratch mapping", mapping_self}, {"managed pages", managed_self},
    {"managed edges", managed_edges}
};
static void run(const struct test_case *cases, size_t count) {
    serial_init();
    unsigned passed = 0, failed = 0;
    report("TEST BEGIN\n");
    for (size_t i = 0; i < count; ++i) {
        const char *reason = cases[i].run();
        report(reason ? "FAIL " : "PASS "); report(cases[i].name);
        if (reason) {
            ++failed; report(": "); report(reason);
            if (cases[i].run == mapping_self || cases[i].run == managed_self) {
                report(" "); report_number(vmm_test_error());
            }
        } else ++passed;
        report("\n");
    }
    report("TEST SUMMARY passed="); report_number(passed);
    report(" failed="); report_number(failed); report("\n");
}
void kernel_tests_memory(void) { run(memory_cases, sizeof(memory_cases)/sizeof(memory_cases[0])); }
void kernel_tests_all(void) { kernel_tests_memory(); }
void kernel_test_slab_stress(void) { run(&memory_cases[4], 1); }
