#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests.h"
#include "terminal/terminal.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "memory/vmm_test.h"

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

static bool same_region_stats(const struct vmm_region_stats *a,
                              const struct vmm_region_stats *b) {
    return a->reserved_virtual_pages == b->reserved_virtual_pages &&
           a->mapped_virtual_pages == b->mapped_virtual_pages &&
           a->owned_backing_pages == b->owned_backing_pages &&
           a->free_virtual_pages == b->free_virtual_pages &&
           a->page_table_frames == b->page_table_frames;
}

static const char *region_allocations(void) {
    struct vmm_region_stats baseline, current;
    if (!vmm_region_stats_get(&baseline) || vmm_region_stats_get(0) ||
        baseline.reserved_virtual_pages ||
        baseline.mapped_virtual_pages || baseline.owned_backing_pages)
        return "region allocator did not start empty";
    uint64_t free_frames = pmm_free_pages();
    void *invalid = vmm_alloc_pages(0);
    bool zero_accepted = invalid != 0;
    if (invalid) vmm_free_pages(invalid);
    invalid = vmm_alloc_pages(VMM_REGION_MAX_PAGES + 1U);
    bool oversized_accepted = invalid != 0;
    if (invalid) vmm_free_pages(invalid);
    if (zero_accepted || oversized_accepted)
        return "invalid region size accepted";

    void *single = vmm_alloc_pages(1);
    void *multiple = vmm_alloc_pages(3);
    if (!single || !multiple) {
        if (single) vmm_free_pages(single);
        if (multiple) vmm_free_pages(multiple);
        return "single or multi-page allocation failed";
    }
    uint64_t one = (uint64_t)(uintptr_t)single;
    uint64_t many = (uint64_t)(uintptr_t)multiple;
    if ((one & 4095ULL) || (many & 4095ULL) ||
        !(one + 4096 <= many || many + 3 * 4096 <= one)) {
        vmm_free_pages(single); vmm_free_pages(multiple);
        return "region alignment or virtual separation failed";
    }
    uint8_t *a = (uint8_t *)single;
    uint8_t *b = (uint8_t *)multiple;
    if (a[0] || a[4095] || b[0] || b[4095] || b[4096] ||
        b[8191] || b[8192] || b[12287]) {
        vmm_free_pages(single); vmm_free_pages(multiple);
        return "region pages were not zeroed";
    }
    a[0] = 0x11; a[4095] = 0x12;
    b[0] = 0x21; b[4095] = 0x22;
    b[4096] = 0x31; b[8191] = 0x32;
    b[8192] = 0x41; b[12287] = 0x42;
    if (a[0] != 0x11 || a[4095] != 0x12 || b[0] != 0x21 ||
        b[4095] != 0x22 || b[4096] != 0x31 || b[8191] != 0x32 ||
        b[8192] != 0x41 || b[12287] != 0x42) {
        vmm_free_pages(single); vmm_free_pages(multiple);
        return "region boundary data was not independent";
    }
    uint64_t physical_pages[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t physical;
        uint64_t address = i ? many + (uint64_t)(i - 1) * 4096 : one;
        if (!vmm_translate(address + 4095, &physical) ||
            (physical & 4095ULL) != 4095ULL) {
            vmm_free_pages(single); vmm_free_pages(multiple);
            return "region translation failed";
        }
        physical_pages[i] = physical & ~4095ULL;
        for (unsigned previous = 0; previous < i; ++previous)
            if (physical_pages[previous] == physical_pages[i]) {
                vmm_free_pages(single); vmm_free_pages(multiple);
                return "allocations share physical backing unexpectedly";
            }
    }
    if (vmm_free_pages((void *)(uintptr_t)(one + 1)) ||
        vmm_free_pages((void *)(uintptr_t)1)) {
        vmm_free_pages(single); vmm_free_pages(multiple);
        return "invalid region free accepted";
    }
    if (!vmm_region_stats_get(&current) || current.reserved_virtual_pages != 4 ||
        current.mapped_virtual_pages != 4 || current.owned_backing_pages != 4 ||
        current.free_virtual_pages != VMM_REGION_MAX_PAGES - 4 ||
        pmm_free_pages() + 4 != free_frames) {
        vmm_free_pages(single); vmm_free_pages(multiple);
        return "region accounting mismatch";
    }
    uint64_t ignored;
    bool single_freed = vmm_free_pages(single);
    bool stale_mapping = vmm_translate(one, &ignored);
    bool double_free = vmm_free_pages(single);
    bool multiple_freed = vmm_free_pages(multiple);
    if (!single_freed || stale_mapping || double_free || !multiple_freed)
        return "region free or double-free check failed";
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "region cleanup accounting mismatch";

    /* Arrange a two-frame free-list gap and prove the virtual run can map it. */
    uint64_t physical[16] = {0};
    unsigned physical_count = 0;
    while (physical_count < 16 &&
           (physical[physical_count] = pmm_alloc_page()) != 0) ++physical_count;
    if (physical_count != 16) {
        for (unsigned i = 0; i < physical_count; ++i) pmm_free_page(physical[i]);
        return "noncontiguous backing setup failed";
    }
    unsigned first_frame = 16, second_frame = 16;
    for (unsigned i = 0; i < physical_count && first_frame == 16; ++i)
        for (unsigned j = i + 1; j < physical_count; ++j) {
            uint64_t difference = physical[i] > physical[j]
                ? physical[i] - physical[j] : physical[j] - physical[i];
            if (difference > 4096) { first_frame = i; second_frame = j; break; }
        }
    if (first_frame == 16) {
        for (unsigned i = 0; i < physical_count; ++i) pmm_free_page(physical[i]);
        return "could not arrange noncontiguous physical frames";
    }
    if (!pmm_free_page(physical[first_frame]) || !pmm_free_page(physical[second_frame])) {
        for (unsigned i = 0; i < physical_count; ++i)
            if (i != first_frame && i != second_frame) pmm_free_page(physical[i]);
        return "noncontiguous backing setup cleanup failed";
    }
    void *scattered = vmm_alloc_pages(2);
    uint64_t mapped0 = 0, mapped1 = 0;
    bool scattered_ok = scattered &&
        vmm_translate((uint64_t)(uintptr_t)scattered, &mapped0) &&
        vmm_translate((uint64_t)(uintptr_t)scattered + 4096, &mapped1) &&
        (mapped0 & ~4095ULL) == physical[second_frame] &&
        (mapped1 & ~4095ULL) == physical[first_frame] &&
        (mapped0 & ~4095ULL) != (mapped1 & ~4095ULL);
    bool scattered_freed = !scattered || vmm_free_pages(scattered);
    for (unsigned i = 0; i < physical_count; ++i)
        if (i != first_frame && i != second_frame) pmm_free_page(physical[i]);
    if (!scattered_ok || !scattered_freed || pmm_free_pages() != free_frames)
        return "noncontiguous physical backing was not preserved";

    for (unsigned i = 0; i < 8; ++i) {
        void *cycle = vmm_alloc_pages(2);
        if (!cycle || !vmm_free_pages(cycle)) {
            if (cycle) vmm_free_pages(cycle);
            return "repeated region allocation failed";
        }
    }
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "repeated region cycle leaked resources";

    /* Fail after two frames, then cross into a new PT and fail immediately.
       Both backing and any PT created by the failed transaction must roll back. */
    vmm_test_fail_alloc_after(2);
    void *unexpected = vmm_alloc_pages(5);
    if (unexpected) {
        vmm_free_pages(unexpected);
        vmm_test_fail_alloc_after(-1);
        return "injected partial backing failure was ignored";
    }
    vmm_test_fail_alloc_after(0);
    unexpected = vmm_alloc_pages(480);
    if (unexpected) {
        vmm_free_pages(unexpected);
        vmm_test_fail_alloc_after(-1);
        return "injected page-table rollback failure was ignored";
    }
    vmm_test_fail_alloc_after(-1);
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "failed allocation did not roll back";

    void *full = vmm_alloc_pages(VMM_REGION_MAX_PAGES);
    if (!full) return "full-window allocation failed";
    void *extra = vmm_alloc_pages(1);
    bool full_stats_ok = !extra && vmm_region_stats_get(&current) &&
        current.reserved_virtual_pages == VMM_REGION_MAX_PAGES &&
        current.mapped_virtual_pages == VMM_REGION_MAX_PAGES &&
        current.owned_backing_pages == VMM_REGION_MAX_PAGES &&
        current.free_virtual_pages == 0 && current.page_table_frames == 8;
    if (extra) vmm_free_pages(extra);
    if (!full_stats_ok) {
        vmm_free_pages(full);
        return "region exhaustion accounting failed";
    }
    uint8_t *last = (uint8_t *)full + (VMM_REGION_MAX_PAGES - 1U) * 4096U;
    last[0] = 0x5a; last[4095] = 0xa5;
    bool boundary_ok = last[0] == 0x5a && last[4095] == 0xa5;
    bool full_freed = vmm_free_pages(full);
    if (!boundary_ok || !full_freed)
        return "full-window boundary or cleanup failed";
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "full-window cleanup leaked resources";

    void *small[64];
    unsigned small_count = 0;
    while (small_count < 64 && (small[small_count] = vmm_alloc_pages(1)) != 0)
        ++small_count;
    void *record_overflow = vmm_alloc_pages(1);
    if (record_overflow) vmm_free_pages(record_overflow);
    for (unsigned i = 0; i < small_count; ++i) vmm_free_pages(small[i]);
    if (small_count != 64 || record_overflow ||
        !vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "allocation record exhaustion or reuse failed";
    return 0;
}

static const char *page_permissions(void) {
    if (!vmm_page_protection_ready()) return "NX and supervisor write protection unavailable";
    uint64_t frames_before = pmm_free_pages();
    void *pages = vmm_alloc_pages(2);
    if (!pages) return "default permission allocation failed";
    uint64_t base = (uint64_t)(uintptr_t)pages;
    uint32_t permissions = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!vmm_get_page_permissions((void *)(uintptr_t)(base + (uint64_t)i * 4096),
                                      &permissions) ||
            permissions != (VMM_PAGE_READ | VMM_PAGE_WRITE)) {
            vmm_free_pages(pages);
            return "default page permissions are not RW and NX";
        }
    }
    if (vmm_get_page_permissions((void *)(uintptr_t)(base + 1), &permissions) ||
        vmm_set_pages_permissions((void *)(uintptr_t)(base + 1), VMM_PAGE_READ) ||
        vmm_set_pages_permissions(pages, VMM_PAGE_WRITE) ||
        vmm_set_pages_permissions(pages, VMM_PAGE_READ | VMM_PAGE_WRITE |
                                          VMM_PAGE_EXECUTE)) {
        vmm_free_pages(pages);
        return "invalid permission request accepted";
    }
    if (!vmm_set_pages_permissions(pages, VMM_PAGE_READ)) {
        vmm_free_pages(pages);
        return "read-only permission update failed";
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!vmm_get_page_permissions((void *)(uintptr_t)(base + (uint64_t)i * 4096),
                                      &permissions) || permissions != VMM_PAGE_READ) {
            vmm_free_pages(pages);
            return "read-only update did not cover every page";
        }
    }
    if (!vmm_set_pages_permissions(pages, VMM_PAGE_READ | VMM_PAGE_EXECUTE)) {
        vmm_free_pages(pages);
        return "read-execute permission update failed";
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (!vmm_get_page_permissions((void *)(uintptr_t)(base + (uint64_t)i * 4096),
                                      &permissions) ||
            permissions != (VMM_PAGE_READ | VMM_PAGE_EXECUTE)) {
            vmm_free_pages(pages);
            return "read-execute update did not cover every page";
        }
    }
    if (!vmm_free_pages(pages) || pmm_free_pages() != frames_before)
        return "permissioned allocation cleanup failed";

    void *read_only = vmm_alloc_pages_with_permissions(1, VMM_PAGE_READ);
    if (!read_only) return "read-only allocation failed";
    bool freed = vmm_free_pages(read_only);
    if (!freed || pmm_free_pages() != frames_before)
        return "read-only allocation cleanup failed";
    return 0;
}

static const struct test_case memory_cases[] = {
    {"physical self", physical_self}, {"physical edges", physical_edges},
    {"heap self", heap_self}, {"heap edges", heap_edges},
    {"slab stress", slab_stress}, {"page tables", table_self},
    {"scratch mapping", mapping_self}, {"managed pages", managed_self},
    {"managed edges", managed_edges}, {"virtual regions", region_allocations},
    {"VM permissions", page_permissions}
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
