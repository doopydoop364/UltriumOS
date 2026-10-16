#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests.h"
#include "tests_internal.h"
#include "scheduler/task.h"
#include "terminal/terminal.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "memory/vmm_test.h"
#include "system/exceptions.h"
#include "system/interrupts.h"
#include "keyboard/keyboard.h"
#include "fs/vfs.h"
#include "memory.h"
#include "ush.h"

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
static const char *physical_self(void) { return pmm_self_test() ? 0 : "PMM self-test failed"; }
static const char *exception_descriptors(void) {
    return exceptions_self_test() ? 0 : "TSS, task register, or double-fault IST invalid";
}
static const char *timer_interrupt(void) {
    if (interrupts_self_test()) return 0;
    report("timer IRQ status mask|IRR<<8|IF<<16|tick-seen<<17|last-vector<<24=");
    report_number(interrupts_test_status());
    report("\n");
    return "PIT IRQ0 did not advance the system tick";
}
static const char *keyboard_interrupt(void) {
    if (!keyboard_interrupt_input()) return "PS/2 IRQ1 input was not enabled";
    return keyboard_dropped_scancodes() ? "PS/2 scancode queue overflowed" : 0;
}
static const char *physical_ownership(void) {
    uint64_t before = pmm_free_pages();
    struct pmm_page_handle owner = {0}, stale, forged;
    if (!pmm_page_alloc(&owner)) return "owned frame allocation failed";
    stale = owner;
    forged = owner;
    forged.generation += 2;
    if (pmm_page_handle_is_empty(&owner) || !pmm_page_handle_valid(&owner) ||
        pmm_page_release(&forged) || !pmm_page_handle_valid(&owner)) {
        pmm_page_release(&owner);
        return "forged generation accepted or valid owner rejected";
    }
    forged = owner;
    forged.physical = 1;
    if (pmm_page_release(&forged)) {
        pmm_page_release(&owner);
        return "forged physical address accepted";
    }
    uint64_t old_physical = owner.physical;
    uint32_t old_generation = owner.generation;
    if (!pmm_page_release(&owner) || !pmm_page_handle_is_empty(&owner) ||
        pmm_page_release(&owner) || pmm_page_release(&stale))
        return "double or stale release accepted";
    struct pmm_page_handle reused = {0};
    if (!pmm_page_alloc(&reused) || reused.physical != old_physical ||
        reused.generation == old_generation || pmm_page_release(&stale)) {
        if (reused.physical) pmm_page_release(&reused);
        return "reused frame did not reject stale generation";
    }
    if (!pmm_page_release(&reused) || pmm_free_pages() != before)
        return "owned frame accounting did not recover";
    return 0;
}
static const char *physical_exhaustion(void) {
    uint64_t before = pmm_free_pages();
    struct pmm_page_handle first = {0}, failed = {0};
    pmm_test_fail_alloc_after(1);
    bool first_ok = pmm_page_alloc(&first);
    bool exhausted = !pmm_page_alloc(&failed);
    pmm_test_fail_alloc_after(-1);
    bool restored = pmm_page_handle_is_empty(&failed) && first.physical &&
                    pmm_page_release(&first) && pmm_free_pages() == before;
    return first_ok && exhausted && restored ? 0 : "injected PMM exhaustion corrupted ownership";
}
static const char *heap_self(void) { return heap_self_test() ? 0 : "heap self-test failed"; }
static const char *table_self(void) { return vmm_self_test() ? 0 : "HHDM translation failed"; }
static const char *mapping_self(void) { return vmm_map_test() ? 0 : "scratch mapping stage"; }
static const char *managed_self(void) { return vmm_managed_test() ? 0 : "managed mapping stage"; }

static const char *physical_edges(void) {
    uint64_t before = pmm_free_pages();
    struct pmm_page_handle pages[4] = {{0}};
    const char *error = 0;
    struct pmm_page_handle forged = {.physical = 0, .generation = 1};
    if (pmm_page_release(&forged)) return "invalid physical owner accepted";
    for (unsigned i = 0; i < 4; ++i) {
        if (!pmm_page_alloc(&pages[i])) { error = "four-page allocation failed"; break; }
        for (unsigned j = 0; j < i; ++j)
            if (pages[i].physical == pages[j].physical) error = "duplicate physical frame";
    }
    if (!error && pmm_free_pages() + 4 != before) error = "PMM debit mismatch";
    if (pages[1].physical) {
        struct pmm_page_handle stale = pages[1];
        if (!pmm_page_release(&pages[1])) error = "physical free failed";
        else {
            struct pmm_page_handle reused = {0};
            if (!pmm_page_alloc(&reused) || reused.physical != stale.physical ||
                reused.generation == stale.generation) {
                error = "released frame was not reused";
                if (reused.physical) pmm_page_release(&reused);
            } else if (pmm_page_release(&stale)) error = "stale physical owner accepted";
            else if (!pmm_page_release(&reused)) error = "reused frame release failed";
        }
    }
    for (unsigned i = 0; i < 4; ++i)
        if (pages[i].physical && !pmm_page_release(&pages[i])) error = "physical cleanup failed";
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
    /* Live kernel tasks (idle, test tasks) keep their stacks in the region; everything else must be free. */
    const uint64_t resident_pages = (uint64_t)task_stack_allocations() * TASK_KSTACK_PAGES;
    const unsigned resident_records = task_stack_allocations();
    if (!vmm_region_stats_get(&baseline) || vmm_region_stats_get(0) ||
        baseline.reserved_virtual_pages != resident_pages ||
        baseline.mapped_virtual_pages != resident_pages || baseline.owned_backing_pages != resident_pages)
        return "region allocator did not start with only the kernel task stacks";
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
    if (!vmm_region_stats_get(&current) || current.reserved_virtual_pages != resident_pages + 4 ||
        current.mapped_virtual_pages != resident_pages + 4 || current.owned_backing_pages != resident_pages + 4 ||
        current.free_virtual_pages != VMM_REGION_MAX_PAGES - resident_pages - 4 ||
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
    struct pmm_page_handle physical[16] = {{0}};
    unsigned physical_count = 0;
    while (physical_count < 16 &&
           pmm_page_alloc(&physical[physical_count])) ++physical_count;
    if (physical_count != 16) {
        for (unsigned i = 0; i < physical_count; ++i) pmm_page_release(&physical[i]);
        return "noncontiguous backing setup failed";
    }
    unsigned first_frame = 16, second_frame = 16;
    for (unsigned i = 0; i < physical_count && first_frame == 16; ++i)
        for (unsigned j = i + 1; j < physical_count; ++j) {
            uint64_t difference = physical[i].physical > physical[j].physical
                ? physical[i].physical - physical[j].physical
                : physical[j].physical - physical[i].physical;
            if (difference > 4096) { first_frame = i; second_frame = j; break; }
        }
    if (first_frame == 16) {
        for (unsigned i = 0; i < physical_count; ++i) pmm_page_release(&physical[i]);
        return "could not arrange noncontiguous physical frames";
    }
    uint64_t first_physical = physical[first_frame].physical;
    uint64_t second_physical = physical[second_frame].physical;
    if (!pmm_page_release(&physical[first_frame]) ||
        !pmm_page_release(&physical[second_frame])) {
        for (unsigned i = 0; i < physical_count; ++i)
            if (i != first_frame && i != second_frame) pmm_page_release(&physical[i]);
        return "noncontiguous backing setup cleanup failed";
    }
    void *scattered = vmm_alloc_pages(2);
    uint64_t mapped0 = 0, mapped1 = 0;
    bool scattered_ok = scattered &&
        vmm_translate((uint64_t)(uintptr_t)scattered, &mapped0) &&
        vmm_translate((uint64_t)(uintptr_t)scattered + 4096, &mapped1) &&
        (mapped0 & ~4095ULL) == second_physical &&
        (mapped1 & ~4095ULL) == first_physical &&
        (mapped0 & ~4095ULL) != (mapped1 & ~4095ULL);
    bool scattered_freed = !scattered || vmm_free_pages(scattered);
    for (unsigned i = 0; i < physical_count; ++i)
        if (i != first_frame && i != second_frame) pmm_page_release(&physical[i]);
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

    /* Fail through the PMM itself after page-table/backing acquisitions have
       started, exercising ownership-handle rollback across subsystem APIs. */
    pmm_test_fail_alloc_after(2);
    unexpected = vmm_alloc_pages(5);
    pmm_test_fail_alloc_after(-1);
    if (unexpected) {
        vmm_free_pages(unexpected);
        return "injected PMM failure was ignored by VMM";
    }
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "PMM failure leaked region ownership";

    void *full = vmm_alloc_pages(VMM_REGION_MAX_PAGES - resident_pages);
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
    uint8_t *last = (uint8_t *)full + (VMM_REGION_MAX_PAGES - resident_pages - 1U) * 4096U;
    last[0] = 0x5a; last[4095] = 0xa5;
    bool boundary_ok = last[0] == 0x5a && last[4095] == 0xa5;
    bool full_freed = vmm_free_pages(full);
    if (!boundary_ok || !full_freed)
        return "full-window boundary or cleanup failed";
    if (!vmm_region_stats_get(&current) || !same_region_stats(&baseline, &current) ||
        pmm_free_pages() != free_frames) return "full-window cleanup leaked resources";

    void *small[64];
    unsigned small_count = 0;
    const unsigned small_limit = 64U - resident_records;
    while (small_count < small_limit && (small[small_count] = vmm_alloc_pages(1)) != 0)
        ++small_count;
    void *record_overflow = vmm_alloc_pages(1);
    if (record_overflow) vmm_free_pages(record_overflow);
    for (unsigned i = 0; i < small_count; ++i) vmm_free_pages(small[i]);
    if (small_count != small_limit || record_overflow ||
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

static const char *filesystem_paths(void) {
    char normalized[VFS_PATH_MAX + 1U];
    if (!vfs_is_directory("/")) return "RAMFS root is unavailable";
    if (vfs_normalize("/alpha/./beta/../file", "/", normalized,
                      sizeof(normalized)) != VFS_OK ||
        memcmp(normalized, "/alpha/file", sizeof("/alpha/file")))
        return "absolute path normalization failed";
    if (vfs_normalize("../../alpha", "/", normalized, sizeof(normalized)) != VFS_OK ||
        memcmp(normalized, "/alpha", sizeof("/alpha")))
        return "root path clamping failed";
    if (vfs_normalize("relative", 0, normalized, sizeof(normalized)) == VFS_OK ||
        vfs_normalize("/name-that-is-far-too-long-for-one-component", "/",
                      normalized, sizeof(normalized)) == VFS_OK)
        return "invalid path accepted";
    if (vfs_create_directory("/vfs-test") != VFS_OK)
        return "test directory creation failed";
    enum vfs_error duplicate = vfs_create_directory("/vfs-test");
    bool directory_ok = duplicate == VFS_EXISTS && vfs_is_directory("/vfs-test");
    bool cleanup_ok = vfs_unlink("/vfs-test") == VFS_OK;
    return directory_ok && cleanup_ok ? 0 : "duplicate handling or directory cleanup failed";
}

static const char *filesystem_file_io(void) {
    uint64_t frames = pmm_free_pages(), allocations = heap_live_allocations();
    struct vfs_file file = {0}, stale = {0};
    struct vfs_file forged = {VFS_HANDLE_SLOTS, 1};
    const char *error = 0;
    size_t amount = 99;
    char data[8] = {0};
    if (vfs_create_directory("/vfs-test") != VFS_OK) return "file test directory failed";
    if (vfs_create_file("/vfs-test/data") != VFS_OK) { error = "file creation failed"; goto cleanup; }
    if (vfs_file_close(&forged) != VFS_BAD_HANDLE) {
        error = "forged file handle was accepted"; goto cleanup;
    }
    if (vfs_file_open("/vfs-test/data", VFS_OPEN_READ | VFS_OPEN_WRITE, &file) != VFS_OK) {
        error = "read/write open failed"; goto cleanup;
    }
    if (vfs_file_open("/vfs-test/data", VFS_OPEN_READ, &file) != VFS_BAD_HANDLE) {
        error = "open overwrote a live file handle"; goto cleanup;
    }
    if (vfs_unlink("/vfs-test/data") != VFS_UNSUPPORTED) {
        error = "open file deletion was not rejected"; goto cleanup;
    }
    if (vfs_file_read(&file, data, sizeof(data), &amount) != VFS_OK || amount != 0) {
        error = "empty file did not return EOF"; goto cleanup;
    }
    static const char expected[] = "hello";
    if (vfs_file_write(&file, expected, sizeof(expected) - 1, &amount) != VFS_OK ||
        amount != sizeof(expected) - 1) { error = "file write failed"; goto cleanup; }
    if (vfs_file_read(&file, data, sizeof(data), &amount) != VFS_OK || amount != 0) {
        error = "read at end of file failed"; goto cleanup;
    }
    stale = file;
    if (vfs_file_close(&file) != VFS_OK) { error = "file close failed"; goto cleanup; }
    if (vfs_file_close(&stale) != VFS_BAD_HANDLE) { error = "stale file handle was accepted"; goto cleanup; }
    if (vfs_file_open("/vfs-test/data", VFS_OPEN_READ, &file) != VFS_OK) {
        error = "read-only reopen failed"; goto cleanup;
    }
    if (vfs_file_read(&stale, data, sizeof(data), &amount) != VFS_BAD_HANDLE) {
        error = "stale handle survived slot reuse"; goto cleanup;
    }
    if (vfs_file_write(&file, expected, 1, &amount) != VFS_BAD_HANDLE) {
        error = "read-only handle allowed a write"; goto cleanup;
    }
    if (vfs_file_read(&file, data, sizeof(data), &amount) != VFS_OK ||
        amount != sizeof(expected) - 1 || memcmp(data, expected, amount)) {
        error = "file contents did not round-trip"; goto cleanup;
    }
    if (vfs_file_close(&file) != VFS_OK) { error = "read handle close failed"; goto cleanup; }
    if (vfs_file_open("/vfs-test/data", VFS_OPEN_WRITE, &file) != VFS_OK) {
        error = "write-only reopen failed"; goto cleanup;
    }
    if (vfs_file_write(&file, expected, VFS_FILE_MAX + 1U, &amount) != VFS_NO_SPACE || amount) {
        error = "oversized file write was accepted"; goto cleanup;
    }
    if (vfs_file_close(&file) != VFS_OK) { error = "write handle close failed"; goto cleanup; }
cleanup:
    if (file.generation && vfs_file_close(&file) != VFS_OK) error = "file handle cleanup failed";
    if (vfs_unlink("/vfs-test/data") != VFS_OK && !error) error = "file cleanup failed";
    if (vfs_unlink("/vfs-test") != VFS_OK && !error) error = "directory cleanup failed";
    if (pmm_free_pages() != frames || heap_live_allocations() != allocations)
        error = "file test leaked heap or physical memory";
    return error;
}

static const char *filesystem_directory_iteration(void) {
    uint64_t allocations = heap_live_allocations();
    struct vfs_dir dir = {0};
    const char *error = 0;
    char name[VFS_NAME_MAX + 1U];
    enum vfs_node_type type;
    if (vfs_create_directory("/vfs-list") != VFS_OK) return "listing directory creation failed";
    if (vfs_create_file("/vfs-list/first") != VFS_OK ||
        vfs_create_directory("/vfs-list/second") != VFS_OK) {
        error = "listing children creation failed"; goto cleanup;
    }
    if (vfs_dir_open("/vfs-list", &dir) != VFS_OK) { error = "directory open failed"; goto cleanup; }
    if (vfs_dir_open("/vfs-list", &dir) != VFS_BAD_HANDLE) {
        error = "open overwrote a live directory handle"; goto cleanup;
    }
    if (vfs_unlink("/vfs-list") != VFS_UNSUPPORTED) {
        error = "open directory removal was not rejected"; goto cleanup;
    }
    if (vfs_unlink("/vfs-list/first") != VFS_UNSUPPORTED) {
        error = "mutation during directory iteration was accepted"; goto cleanup;
    }
    if (vfs_dir_next(&dir, name, sizeof(name), &type) != VFS_OK ||
        type != VFS_NODE_FILE || memcmp(name, "first", sizeof("first"))) {
        error = "first directory entry mismatch"; goto cleanup;
    }
    if (vfs_dir_next(&dir, name, sizeof(name), &type) != VFS_OK ||
        type != VFS_NODE_DIRECTORY || memcmp(name, "second", sizeof("second"))) {
        error = "second directory entry mismatch"; goto cleanup;
    }
    if (vfs_dir_next(&dir, name, sizeof(name), &type) != VFS_END) {
        error = "directory end marker missing"; goto cleanup;
    }
cleanup:
    if (dir.generation && vfs_dir_close(&dir) != VFS_OK) error = "directory handle cleanup failed";
    if (vfs_unlink("/vfs-list/first") != VFS_OK && !error) error = "file entry cleanup failed";
    if (vfs_unlink("/vfs-list/second") != VFS_OK && !error) error = "directory entry cleanup failed";
    if (vfs_unlink("/vfs-list") != VFS_OK && !error) error = "listing directory cleanup failed";
    if (heap_live_allocations() != allocations) error = "directory enumeration leaked a handle";
    return error;
}

static const char *filesystem_rollback(void) {
    uint64_t frames = pmm_free_pages(), allocations = heap_live_allocations();
    struct vfs_file file = {0};
    const char *error = 0;
    size_t written = 123;
    char payload[128];
    memset(payload, 0x5a, sizeof(payload));
    if (vfs_create_directory("/vfs-fail") != VFS_OK) return "rollback directory creation failed";
    vfs_test_fail_alloc_after(0);
    enum vfs_error failed_create = vfs_create_file("/vfs-fail/item");
    vfs_test_fail_alloc_after(-1);
    if (failed_create != VFS_NO_SPACE || vfs_create_file("/vfs-fail/item") != VFS_OK) {
        error = "failed node allocation changed directory state"; goto cleanup;
    }
    if (vfs_file_open("/vfs-fail/item", VFS_OPEN_WRITE, &file) != VFS_OK) {
        error = "rollback file open failed"; goto cleanup;
    }
    vfs_test_fail_alloc_after(0);
    enum vfs_error failed_write = vfs_file_write(&file, payload, sizeof(payload), &written);
    vfs_test_fail_alloc_after(-1);
    if (failed_write != VFS_NO_SPACE || written != 0) {
        error = "failed buffer growth was not atomic"; goto cleanup;
    }
    if (vfs_file_write(&file, payload, sizeof(payload), &written) != VFS_OK ||
        written != sizeof(payload)) { error = "write did not recover after allocation failure"; goto cleanup; }
    if (vfs_file_close(&file) != VFS_OK) { error = "rollback handle close failed"; goto cleanup; }

    struct vfs_file handles[VFS_HANDLE_SLOTS + 1U] = {{0}};
    unsigned opened = 0;
    for (; opened < VFS_HANDLE_SLOTS; ++opened) {
        if (vfs_file_open("/vfs-fail/item", VFS_OPEN_READ, &handles[opened]) != VFS_OK) {
            error = "file handle pool filled early"; break;
        }
    }
    if (!error && vfs_file_open("/vfs-fail/item", VFS_OPEN_READ,
                                &handles[VFS_HANDLE_SLOTS]) != VFS_NO_SPACE)
        error = "file handle pool exhaustion was not reported";
    while (opened) {
        --opened;
        if (vfs_file_close(&handles[opened]) != VFS_OK) error = "file handle pool cleanup failed";
    }
cleanup:
    vfs_test_fail_alloc_after(-1);
    if (file.generation && vfs_file_close(&file) != VFS_OK) error = "rollback file close failed";
    if (vfs_unlink("/vfs-fail/item") != VFS_OK && !error) error = "rollback file cleanup failed";
    if (vfs_unlink("/vfs-fail") != VFS_OK && !error) error = "rollback directory cleanup failed";
    if (pmm_free_pages() != frames || heap_live_allocations() != allocations)
        error = "allocation failure left leaked resources";
    return error;
}

static const char *shell_native(void) {
    uint64_t allocations = heap_live_allocations(), frames = pmm_free_pages();
    const char *reason = 0;
    if (ush_execute("new folder /ush-test") != USH_OK ||
        ush_execute("go /ush-test") != USH_OK ||
        ush_execute("write data Hello Ultrium") != USH_OK) reason = "native create/write/go failed";
    struct vfs_info info;
    if (!reason && (vfs_stat("/ush-test/data", &info) != VFS_OK || info.size != 13 ||
        ush_execute("where") != USH_OK || ush_execute("list") != USH_OK ||
        ush_execute("view data") != USH_OK || ush_execute("info data") != USH_OK))
        reason = "native read/list/stat failed";
    if (ush_execute("go /") != USH_OK) reason = "cwd cleanup failed";
    if (ush_execute("remove /ush-test/data") != USH_OK ||
        ush_execute("remove /ush-test") != USH_OK) reason = "native remove failed";
    if (heap_live_allocations() != allocations || pmm_free_pages() != frames) reason = "native commands leaked";
    return reason;
}
static const char *shell_aliases(void) {
    bool saved = ush_compatibility();
    const char *reason = 0;
    ush_compatibility_disable();
    if (ush_compatibility() || ush_execute("ls") != USH_UNAVAILABLE)
        reason = "native mode accepted alias";
    ush_compatibility_enable();
    if (!ush_compatibility() || ush_execute("mkdir /ush-alias") != USH_OK ||
        ush_execute("touch /ush-alias/a") != USH_OK ||
        ush_execute("cp /ush-alias/a /ush-alias/b") != USH_OK ||
        ush_execute("mv /ush-alias/b /ush-alias/c") != USH_OK ||
        ush_execute("rm /ush-alias/c") != USH_OK ||
        ush_execute("ls /ush-alias") != USH_OK || ush_execute("cat /ush-alias/a") != USH_OK ||
        ush_execute("cd /ush-alias") != USH_OK || ush_execute("pwd") != USH_OK ||
        ush_execute("cd /") != USH_OK || ush_execute("rm /ush-alias/a") != USH_OK ||
        ush_execute("rm /ush-alias") != USH_OK) reason = "enabled aliases failed";
    ush_compatibility_disable();
    if (ush_execute("pwd") != USH_UNAVAILABLE || ush_execute("no-such-command") != USH_UNKNOWN ||
        ush_execute("go") != USH_USAGE || ush_execute("where extra") != USH_USAGE)
        reason = "disable or invalid command behavior failed";
    if (saved) ush_compatibility_enable();
    return reason;
}
static const char *shell_registry(void) {
    const char *help[] = {"help", "go help", "new help", "new file help", "file help",
        "folder help", "ultrium help", "ultrium system help", "ultrium system memory help",
        "ultrium shell help", "ultrium shell compatibility help", "help file new",
        "ultrium system memory mapping managed help", "ultrium test all help"};
    if (!ush_registry_valid()) return "invalid or duplicate registry metadata";
    for (size_t i = 0; i < sizeof(help)/sizeof(help[0]); ++i)
        if (ush_execute(help[i]) != USH_OK) return "hierarchical help failed";
    if (ush_execute("ultrium version") != USH_OK || ush_execute("ultrium system info") != USH_OK ||
        ush_execute("ultrium system interrupts") != USH_OK || ush_execute("ultrium system memory") != USH_OK ||
        ush_execute("ultrium test nonsense") != USH_UNKNOWN || ush_execute("file new") != USH_USAGE)
        return "OS routing or argument validation failed";
    if (ush_execute("folder new /ush-structured") != USH_OK ||
        ush_execute("file write /ush-structured/a text") != USH_OK ||
        ush_execute("file info /ush-structured/a") != USH_OK ||
        ush_execute("folder list /ush-structured") != USH_OK ||
        ush_execute("file remove /ush-structured/a") != USH_OK ||
        ush_execute("folder remove /ush-structured") != USH_OK) return "structured operations failed";
    return 0;
}
#define FS_REQUIRE(condition, message) do { if (!(condition)) { reason = message; goto cleanup; } } while (0)
static const char *filesystem_transfer(void) {
    uint64_t frames = pmm_free_pages(), allocations = heap_live_allocations();
    const char *reason = 0;
    struct vfs_file file = {0}; struct vfs_info info;
    struct vfs_remove_report report;
    char payload[VFS_FILE_MAX], received[VFS_FILE_MAX];
    for (size_t i = 0; i < sizeof(payload); ++i) payload[i] = (char)(i * 17);
    size_t count = 0;
    FS_REQUIRE(vfs_create_directory("/fs-transfer") == VFS_OK, "fixture failed");
    FS_REQUIRE(vfs_create_file("/fs-transfer/source") == VFS_OK &&
        vfs_file_open("/fs-transfer/source", VFS_OPEN_WRITE, &file) == VFS_OK &&
        vfs_file_write(&file, payload, sizeof(payload), &count) == VFS_OK && count == sizeof(payload) &&
        vfs_file_close(&file) == VFS_OK, "source write failed");
    for (int fail = 0; fail < 2; ++fail) {
        uint64_t before = heap_live_allocations();
        vfs_test_fail_alloc_after(fail);
        enum vfs_error failed = vfs_copy("/fs-transfer/source", "/fs-transfer/copy");
        vfs_test_fail_alloc_after(-1);
        FS_REQUIRE(failed == VFS_NO_SPACE && heap_live_allocations() == before &&
            vfs_stat("/fs-transfer/copy", &info) == VFS_NOT_FOUND, "copy allocation rollback failed");
    }
    FS_REQUIRE(vfs_copy("/fs-transfer/source", "/fs-transfer/copy") == VFS_OK &&
        vfs_file_open("/fs-transfer/copy", VFS_OPEN_READ, &file) == VFS_OK &&
        vfs_file_read(&file, received, sizeof(received), &count) == VFS_OK && count == sizeof(payload) &&
        !memcmp(payload, received, count) && vfs_file_close(&file) == VFS_OK, "copied bytes differ");
    FS_REQUIRE(vfs_copy("/fs-transfer/source", "/fs-transfer/copy") == VFS_EXISTS &&
        vfs_rename("/fs-transfer/source", "/fs-transfer/copy") == VFS_EXISTS &&
        vfs_copy("/fs-transfer/missing", "/fs-transfer/new") == VFS_NOT_FOUND &&
        vfs_copy("/fs-transfer", "/fs-transfer/new") == VFS_IS_DIRECTORY &&
        vfs_copy("/fs-transfer/source", "/fs-transfer/source/child") == VFS_NOT_DIRECTORY,
        "transfer conflicts or types accepted");
    FS_REQUIRE(vfs_create_directory("/fs-transfer/dir") == VFS_OK &&
        vfs_create_file("/fs-transfer/dir/child") == VFS_OK &&
        vfs_rename("/fs-transfer/dir", "/fs-transfer/dir/descendant") == VFS_INVALID_PATH &&
        vfs_rename("/fs-transfer/dir", "/fs-transfer/dir") == VFS_EXISTS &&
        vfs_rename("/", "/fs-transfer/root") == VFS_INVALID_PATH, "directory cycle accepted");
    vfs_test_fail_alloc_after(0);
    enum vfs_error renamed = vfs_rename("/fs-transfer/dir", "/fs-transfer/moved");
    vfs_test_fail_alloc_after(-1);
    FS_REQUIRE(renamed == VFS_OK && vfs_stat("/fs-transfer/moved/child", &info) == VFS_OK &&
        vfs_stat("/fs-transfer/dir", &info) == VFS_NOT_FOUND, "atomic allocation-free rename failed");
cleanup:
    vfs_test_fail_alloc_after(-1);
    if (file.generation) vfs_file_close(&file);
    if (vfs_remove_tree("/fs-transfer", &report) != VFS_OK && !reason) reason = "transfer cleanup failed";
    if (heap_live_allocations() != allocations || pmm_free_pages() != frames) reason = "transfer leaked memory";
    return reason;
}
static const char *filesystem_safe_paths(void) {
    const char *reason = 0;
    struct vfs_info info; struct vfs_remove_report report;
    char out[VFS_PATH_MAX + 1U];
    FS_REQUIRE(vfs_create_directory("/fs-paths") == VFS_OK &&
        vfs_create_file("/fs-paths/x") == VFS_OK && vfs_create_file("/fs-paths/abc") == VFS_OK,
        "path fixture failed");
    /* A failed same-length comparison followed by a longer prefix must not match. */
    FS_REQUIRE(vfs_stat("/fs-paths/a", &info) == VFS_NOT_FOUND &&
        vfs_create_file("/fs-paths/..") == VFS_INVALID_PATH &&
        vfs_create_file("/fs-paths/.") == VFS_INVALID_PATH &&
        vfs_create_file("/fs-paths//bad") == VFS_INVALID_PATH &&
        vfs_unlink("/fs-paths/x/") == VFS_INVALID_PATH &&
        vfs_rename("/fs-paths/x", "/fs-paths/../redirect") == VFS_INVALID_PATH,
        "noncanonical path or prefix collision accepted");
    FS_REQUIRE(vfs_resolve_path("missing/../x", "/fs-paths", out, sizeof(out)) == VFS_NOT_FOUND &&
        vfs_resolve_path("x/../abc", "/fs-paths", out, sizeof(out)) == VFS_NOT_DIRECTORY &&
        vfs_resolve_path("./abc", "/fs-paths", out, sizeof(out)) == VFS_OK &&
        !memcmp(out, "/fs-paths/abc", sizeof("/fs-paths/abc")), "invalid traversal redirected action");
    FS_REQUIRE(vfs_create_directory("/fs-paths/a") == VFS_OK &&
        vfs_create_directory("/fs-paths/a/b") == VFS_OK &&
        vfs_create_file("/fs-paths/a/b/0123456789012345678901234567890") == VFS_OK,
        "long path fixture failed");
    FS_REQUIRE(vfs_create_directory("/fs-paths/0123456789012345678901234567890") == VFS_OK &&
        vfs_create_directory("/fs-paths/0123456789012345678901234567890/0123456789012345678901234567890") == VFS_OK &&
        vfs_rename("/fs-paths/a", "/fs-paths/0123456789012345678901234567890/0123456789012345678901234567890/0123456789012345678901234567890") == VFS_INVALID_PATH &&
        vfs_is_directory("/fs-paths/a/b"), "rename made descendants unreachable");
cleanup:
    if (vfs_remove_tree("/fs-paths", &report) != VFS_OK && !reason) reason = "path cleanup failed";
    return reason;
}
static const char *filesystem_tree_removal(void) {
    const char *reason = 0;
    uint64_t allocations = heap_live_allocations(), frames = pmm_free_pages();
    struct vfs_file held = {0}; struct vfs_remove_report report; struct vfs_info info;
    FS_REQUIRE(vfs_create_directory("/fs-tree") == VFS_OK &&
        vfs_create_directory("/fs-tree/sub") == VFS_OK &&
        vfs_create_file("/fs-tree/sub/first") == VFS_OK &&
        vfs_create_file("/fs-tree/sub/held") == VFS_OK &&
        vfs_file_open("/fs-tree/sub/held", VFS_OPEN_READ, &held) == VFS_OK,
        "tree fixture failed");
    FS_REQUIRE(vfs_unlink("/fs-tree") == VFS_NOT_EMPTY &&
        vfs_remove_tree("/", &report) == VFS_INVALID_PATH && !report.removed,
        "nonrecursive or root removal accepted");
    FS_REQUIRE(vfs_remove_tree("/fs-tree", &report) == VFS_UNSUPPORTED && report.removed == 1 &&
        !memcmp(report.remaining, "/fs-tree/sub/held", sizeof("/fs-tree/sub/held")) &&
        vfs_stat("/fs-tree/sub/first", &info) == VFS_NOT_FOUND &&
        vfs_stat("/fs-tree/sub/held", &info) == VFS_OK, "partial removal report incorrect");
    FS_REQUIRE(vfs_file_close(&held) == VFS_OK && vfs_remove_tree("/fs-tree", &report) == VFS_OK &&
        report.removed == 3, "tree removal recovery failed");
cleanup:
    if (held.generation) vfs_file_close(&held);
    if (vfs_is_directory("/fs-tree")) vfs_remove_tree("/fs-tree", &report);
    if (heap_live_allocations() != allocations || pmm_free_pages() != frames) reason = "tree removal leaked memory";
    return reason;
}
static const char *filesystem_capacity(void) {
    const char *reason = 0;
    uint64_t allocations = heap_live_allocations(), frames = pmm_free_pages();
    struct vfs_remove_report report; struct vfs_file file = {0};
    char payload[VFS_FILE_MAX]; memset(payload, 0x39, sizeof(payload)); size_t count;
    FS_REQUIRE(vfs_create_directory("/fs-limits") == VFS_OK &&
        vfs_create_file("/fs-limits/data") == VFS_OK &&
        vfs_file_open("/fs-limits/data", VFS_OPEN_WRITE, &file) == VFS_OK &&
        vfs_file_write(&file, payload, sizeof(payload), &count) == VFS_OK &&
        vfs_file_close(&file) == VFS_OK, "capacity fixture failed");
    const char *hex = "0123456789abcdef";
    unsigned copies = 0;
    for (; copies < VFS_NODE_MAX; ++copies) {
        char path[] = "/fs-limits/n00";
        path[12] = hex[(copies >> 4) & 15]; path[13] = hex[copies & 15];
        enum vfs_error result = vfs_copy("/fs-limits/data", path);
        if (result == VFS_NO_SPACE) break;
        FS_REQUIRE(result == VFS_OK, "capacity copy returned unexpected error");
    }
    FS_REQUIRE(copies == VFS_DATA_MAX / VFS_FILE_MAX - 1, "data capacity not enforced");
    FS_REQUIRE(vfs_remove_tree("/fs-limits", &report) == VFS_OK &&
        vfs_create_directory("/fs-limits") == VFS_OK, "capacity reset failed");
    unsigned nodes = 0;
    for (; nodes < VFS_NODE_MAX; ++nodes) {
        char path[] = "/fs-limits/n00";
        path[12] = hex[(nodes >> 4) & 15]; path[13] = hex[nodes & 15];
        enum vfs_error result = vfs_create_file(path);
        if (result == VFS_NO_SPACE) break;
        FS_REQUIRE(result == VFS_OK, "node capacity unexpected error");
    }
    FS_REQUIRE(nodes == VFS_NODE_MAX - 2, "node capacity not enforced");
cleanup:
    if (file.generation) vfs_file_close(&file);
    if (vfs_remove_tree("/fs-limits", &report) != VFS_OK && !reason) reason = "capacity cleanup failed";
    if (heap_live_allocations() != allocations || pmm_free_pages() != frames) reason = "capacity tests leaked";
    return reason;
}
static const char *shell_safe_operations(void) {
    const char *reason = 0;
    struct vfs_remove_report report; struct vfs_info info;
    FS_REQUIRE(ush_execute("new folder /ush-safe") == USH_OK &&
        ush_execute("new folder /ush-safe/sub") == USH_OK &&
        ush_execute("write /ush-safe/sub/a bytes") == USH_OK &&
        ush_execute("copy /ush-safe/sub/a /ush-safe/sub/b") == USH_OK &&
        ush_execute("file rename /ush-safe/sub/b /ush-safe/sub/c") == USH_OK &&
        ush_execute("move /ush-safe/sub/c /ush-safe/c") == USH_OK, "shell transfer failed");
    FS_REQUIRE(ush_execute("remove /ush-safe -y") == USH_FS_ERROR &&
        ush_execute("remove /ush-safe --unknown") == USH_USAGE &&
        ush_execute("remove / -r -y") == USH_FS_ERROR &&
        ush_execute("go /ush-safe/sub") == USH_OK &&
        ush_execute("remove /ush-safe -r -y") == USH_FS_ERROR &&
        ush_execute("remove . -r -y") == USH_FS_ERROR &&
        ush_execute("move /ush-safe /moved") == USH_FS_ERROR &&
        ush_execute("go /") == USH_OK, "removal flags or cwd protection failed");
    FS_REQUIRE(ush_execute("remove /ush-safe -r") == USH_PENDING && ush_confirmation_pending() &&
        ush_answer_confirmation("") == USH_OK && vfs_is_directory("/ush-safe"), "default confirmation not conservative");
    FS_REQUIRE(ush_execute("remove /ush-safe -r") == USH_PENDING &&
        ush_answer_confirmation("no") == USH_OK && vfs_is_directory("/ush-safe"), "confirmation rejection failed");
    FS_REQUIRE(ush_execute("remove /ush-safe --recursive") == USH_PENDING &&
        ush_answer_confirmation("yes") == USH_OK && vfs_stat("/ush-safe", &info) == VFS_NOT_FOUND,
        "affirmative recursive removal failed");
    FS_REQUIRE(ush_execute("new folder /ush-safe") == USH_OK &&
        ush_execute("new file /ush-safe/a") == USH_OK &&
        ush_execute("remove /ush-safe --recursive --yes") == USH_OK && !ush_confirmation_pending(),
        "noninteractive recursive removal failed");
cleanup:
    if (ush_confirmation_pending()) ush_answer_confirmation("");
    ush_execute("go /");
    if (vfs_is_directory("/ush-safe")) vfs_remove_tree("/ush-safe", &report);
    return reason;
}
#undef FS_REQUIRE

static const char *shell_quoting(void) {
    const char *reason = 0;
    struct ush_args args; struct vfs_info info; struct vfs_remove_report report;
    if (ush_tokenize("write \"a b\" 'c d' \"\" x\\ y \\\"quote\\\"", &args) != USH_OK || args.count != 6 ||
        memcmp(args.values[1], "a b", 4) || memcmp(args.values[2], "c d", 4) ||
        args.values[3][0] || memcmp(args.values[4], "x y", 4) || memcmp(args.values[5], "\"quote\"", 8))
        return "quoted/escaped/empty tokens incorrect";
    if (ush_tokenize("new file \"unfinished", &args) != USH_USAGE || !args.error ||
        ush_tokenize("write a trailing\\", &args) != USH_USAGE ||
        ush_tokenize("a b c d e f g h i j k l m n o p q", &args) != USH_USAGE)
        return "invalid token grammar accepted";
    char line[USH_INPUT_MAX+2];
    memset(line, 'a', USH_INPUT_MAX+1); line[USH_INPUT_MAX+1] = 0;
    if (ush_tokenize(line, &args) != USH_USAGE) return "overlong input silently truncated";
    line[USH_INPUT_MAX] = 0;
    if (ush_tokenize(line, &args) != USH_OK) return "exact input boundary rejected";
    if (ush_execute("new folder \"/ush quote\"") != USH_OK ||
        ush_execute("go \"/ush quote\"") != USH_OK ||
        ush_execute("write \"project notes.txt\" \"Hello  Ultrium\"") != USH_OK ||
        vfs_stat("/ush quote/project notes.txt", &info) != VFS_OK || info.size != 14 ||
        ush_execute("write empty \"\"") != USH_OK || vfs_stat("/ush quote/empty", &info) != VFS_OK || info.size ||
        ush_execute("new file \"\"") != USH_FS_ERROR ||
        ush_execute("new file \"unfinished") != USH_USAGE) reason = "quoted filesystem operations failed";
    ush_execute("go /");
    if (vfs_remove_tree("/ush quote", &report) != VFS_OK && !reason) reason = "quote cleanup failed";
    return reason;
}
static const char *shell_completion(void) {
    const char *reason = 0;
    struct vfs_remove_report report;
    if (vfs_create_directory("/ush-complete") != VFS_OK ||
        vfs_create_directory("/ush-complete/My Folder") != VFS_OK ||
        vfs_create_file("/ush-complete/apple") != VFS_OK ||
        vfs_create_file("/ush-complete/apricot") != VFS_OK ||
        vfs_create_file("/ush-complete/My Folder/notes.txt") != VFS_OK) return "completion fixture failed";
    struct completion_case { const char *input, *expected; enum ush_completion result; };
    const struct completion_case cases[] = {
        {"wher", "where ", USH_COMPLETE_UNIQUE},
        {"ultrium sh", "ultrium shell ", USH_COMPLETE_UNIQUE},
        {"file cop", "file copy ", USH_COMPLETE_UNIQUE},
        {"go /ush-complete/My", "go \"/ush-complete/My Folder/\"", USH_COMPLETE_UNIQUE},
        {"view \"/ush-complete/My Folder/no", "view \"/ush-complete/My Folder/notes.txt\" ", USH_COMPLETE_UNIQUE},
        {"view /ush-complete/a", "view /ush-complete/ap", USH_COMPLETE_MULTIPLE},
        {"view /ush-complete/no-such", "view /ush-complete/no-such", USH_COMPLETE_NONE}
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        char line[USH_INPUT_MAX+1]; size_t n = 0;
        do { line[n] = cases[i].input[n]; } while (cases[i].input[n++]);
        size_t caret = n-1;
        enum ush_completion result = ush_complete(line, &caret);
        size_t expected_length = 0; while (cases[i].expected[expected_length]) ++expected_length;
        if (result != cases[i].result || memcmp(line, cases[i].expected, expected_length+1)) {
            reason = "command/namespace/path completion incorrect"; break;
        }
    }
    char middle[USH_INPUT_MAX+1] = "file copx source target"; size_t caret = 8;
    if (ush_complete(middle, &caret) != USH_COMPLETE_UNIQUE ||
        memcmp(middle, "file copy source target", sizeof("file copy source target"))) reason = "middle completion damaged suffix";
    char boundary[USH_INPUT_MAX+1], preserved[USH_INPUT_MAX+1];
    const char *partial = "view /ush-complete/appl"; size_t used = 0;
    while (partial[used]) { boundary[used] = partial[used]; ++used; }
    for (size_t i = used; i < USH_INPUT_MAX; ++i) boundary[i] = ' ';
    boundary[USH_INPUT_MAX] = 0; memcpy(preserved, boundary, sizeof(boundary));
    if (ush_complete(boundary, &used) != USH_COMPLETE_LIMIT || memcmp(boundary, preserved, sizeof(boundary)))
        reason = "overlong completion silently truncated or changed input";
    if (!ush_suggest("", "lst") || memcmp(ush_suggest("", "lst"), "list", 5) ||
        !ush_suggest("ultrium", "sysem") || ush_suggest("", "zzzzzzzz")) reason = "bounded suggestions incorrect";
    if (vfs_remove_tree("/ush-complete", &report) != VFS_OK && !reason) reason = "completion cleanup failed";
    return reason;
}
static const char *shell_help_output(void) {
    const char *reason = 0;
    terminal_test_capture(1);
    ush_execute("ultrium help"); terminal_test_capture(0);
    if (!terminal_test_contains("system") || !terminal_test_contains("shell") ||
        terminal_test_contains("mapping") || terminal_test_contains("compatibility")) reason = "parent help flattened descendants";
    terminal_test_capture(1);
    ush_execute("lst"); terminal_test_capture(0);
    if (!terminal_test_contains("Unknown command: lst") || !terminal_test_contains("Did you mean: list?"))
        reason = "unknown-command suggestion missing";
    terminal_test_capture(1);
    ush_execute("new file \"unterminated"); terminal_test_capture(0);
    if (!terminal_test_contains("Unterminated quote.")) reason = "quote error not useful";
    return reason;
}

static bool interactive_active, interactive_initial;
static uint64_t interactive_allocations, interactive_frames;
void kernel_tests_shell_interactive_begin(void) {
    interactive_active = true;
    interactive_initial = !ush_compatibility() && !memcmp(ush_cwd(), "/", 2);
    interactive_allocations = heap_live_allocations(); interactive_frames = pmm_free_pages();
    terminal_test_capture(1);
    terminal_write("USH interactive capture started.\n");
}
static const char *shell_interactive_session(void) {
    terminal_test_capture(0);
    if (!interactive_active) return "start with ultrium test shell begin";
    const char *reason = 0;
    struct vfs_info info; struct vfs_file file = {0}; struct vfs_remove_report report;
    char text[32] = {0}; size_t count = 0;
    if (!interactive_initial || ush_compatibility() || !vfs_is_directory("/keyboard/My Projects") ||
        vfs_stat("/keyboard/My Projects/project notes.txt", &info) != VFS_OK || info.size != 13 ||
        vfs_file_open("/keyboard/My Projects/project notes.txt", VFS_OPEN_READ, &file) != VFS_OK ||
        vfs_file_read(&file, text, sizeof(text), &count) != VFS_OK || count != 13 || memcmp(text, "Hello Ultrium", 13))
        reason = "keyboard quoting, initial native mode, or file content failed";
    if (file.generation) vfs_file_close(&file);
    if (vfs_stat("/keyboard/My Projects/mid", &info) != VFS_OK ||
        !vfs_is_directory("/keyboard/My Projects/history") ||
        vfs_stat("/keyboard/boundary", &info) != VFS_NOT_FOUND ||
        vfs_stat("/keyboard/My Projects/unfinished", &info) != VFS_NOT_FOUND ||
        vfs_stat("/keyboard/remove", &info) != VFS_NOT_FOUND)
        reason = "middle editing, history, input limit, parse rejection, or confirmed removal failed";
    const char *expected[] = {"Use 'list' instead.", "Hello Ultrium", "Compatibility aliases: disabled",
        "/keyboard/My Projects", "apple", "apricot", "Unterminated quote.", "Did you mean: list?",
        "Input exceeds 70 characters; command rejected.", "Removal cancelled.", "Removed items: 2",
        "Removed: /keyboard/remove/a"};
    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i)
        if (!terminal_test_contains(expected[i])) reason = "keyboard output contract missing or capture overflowed";
    ush_execute("go /");
    if (vfs_remove_tree("/keyboard", &report) != VFS_OK && !reason) reason = "interactive cleanup failed";
    if (heap_live_allocations() != interactive_allocations || pmm_free_pages() != interactive_frames)
        reason = "interactive commands leaked memory";
    interactive_active = false;
    return reason;
}
static const struct test_case shell_cases[] = {
    {"USH quoting and limits", shell_quoting},
    {"USH completion and suggestions", shell_completion},
    {"USH help and error output", shell_help_output},
    {"USH safe operations", shell_safe_operations},
    {"USH registry and help", shell_registry},
    {"USH native commands", shell_native}, {"USH compatibility and errors", shell_aliases}
};

static const struct test_case memory_cases[] = {
    {"exception TSS/IST", exception_descriptors},
    {"physical self", physical_self}, {"physical ownership", physical_ownership},
    {"physical exhaustion", physical_exhaustion}, {"physical edges", physical_edges},
    {"heap self", heap_self}, {"heap edges", heap_edges},
    {"slab stress", slab_stress}, {"page tables", table_self},
    {"scratch mapping", mapping_self}, {"managed pages", managed_self},
    {"managed edges", managed_edges}, {"virtual regions", region_allocations},
    {"VM permissions", page_permissions}
};
static const struct test_case interrupt_cases[] = {
    {"periodic timer IRQ", timer_interrupt},
    {"PS/2 keyboard IRQ", keyboard_interrupt}
};
static const struct test_case filesystem_cases[] = {
    {"filesystem paths", filesystem_paths},
    {"filesystem file I/O", filesystem_file_io},
    {"filesystem directory iteration", filesystem_directory_iteration},
    {"filesystem allocation rollback", filesystem_rollback},
    {"filesystem copy/rename rollback", filesystem_transfer},
    {"filesystem strict paths", filesystem_safe_paths},
    {"filesystem bounded tree removal", filesystem_tree_removal},
    {"filesystem capacity limits", filesystem_capacity}
};
static void run_groups(const struct test_case *const *groups,
                       const size_t *counts, size_t group_count) {
    serial_init();
    unsigned passed = 0, failed = 0;
    report("TEST BEGIN\n");
    for (size_t group = 0; group < group_count; ++group) {
        for (size_t i = 0; i < counts[group]; ++i) {
            const struct test_case *test = &groups[group][i];
            const char *reason = test->run();
            report(reason ? "FAIL " : "PASS "); report(test->name);
            if (reason) {
                ++failed; report(": "); report(reason);
                if (test->run == mapping_self || test->run == managed_self) {
                    report(" "); report_number(vmm_test_error());
                }
            } else ++passed;
            report("\n");
        }
    }
    report("TEST SUMMARY passed="); report_number(passed);
    report(" failed="); report_number(failed); report("\n");
}
static void run(const struct test_case *cases, size_t count) {
    const struct test_case *groups[] = {cases};
    const size_t counts[] = {count};
    run_groups(groups, counts, 1);
}
void kernel_tests_memory(void) { run(memory_cases, sizeof(memory_cases)/sizeof(memory_cases[0])); }
void kernel_tests_interrupts(void) { run(interrupt_cases, sizeof(interrupt_cases)/sizeof(interrupt_cases[0])); }
void kernel_tests_filesystem(void) { run(filesystem_cases, sizeof(filesystem_cases)/sizeof(filesystem_cases[0])); }
void kernel_tests_storage(void) { run(storage_cases, storage_case_count); }
void kernel_tests_ultriumfs(void) { run(ultriumfs_cases, ultriumfs_case_count); }
void kernel_tests_persist_blank(void) { run(persist_blank_cases, persist_blank_count); }
void kernel_tests_persist_populate(void) { run(persist_populate_cases, persist_populate_count); }
void kernel_tests_persist_stage1(void) { run(persist_stage1_cases, persist_stage1_count); }
void kernel_tests_persist_modify(void) { run(persist_modify_cases, persist_modify_count); }
void kernel_tests_persist_stage3(void) { run(persist_stage3_cases, persist_stage3_count); }
void kernel_tests_persist_clean(void) { run(persist_clean_cases, persist_clean_count); }
void kernel_tests_persist_v1fixture(void) { run(persist_v1fixture_cases, persist_v1fixture_count); }
void kernel_tests_persist_v1verify(void) { run(persist_v1verify_cases, persist_v1verify_count); }
void kernel_tests_persist_unclean(void) { run(persist_unclean_cases, persist_unclean_count); }
void kernel_tests_persist_v2_big(void) { run(persist_v2_big_cases, 1); }
void kernel_tests_persist_v2_bigverify(void) { run(persist_v2_bigverify_cases, 1); }
void kernel_tests_persist_v2_cut_dataflush(void) { run(persist_v2_cut_dataflush_cases, 1); }
void kernel_tests_persist_v2_cut_journal(void) { run(persist_v2_cut_journal_cases, 1); }
void kernel_tests_persist_v2_cut_commit(void) { run(persist_v2_cut_commit_cases, 1); }
void kernel_tests_persist_v2_cut_home(void) { run(persist_v2_cut_home_cases, 1); }
void kernel_tests_persist_v2_cut_checkpoint(void) { run(persist_v2_cut_checkpoint_cases, 1); }
void kernel_tests_persist_v2_recovered(void) { run(persist_v2_recovered_cases, 1); }
void kernel_tests_persist_v2_stream(void) { run(persist_v2_stream_cases, 1); }
void kernel_tests_persist_v2_streamverify(void) { run(persist_v2_streamverify_cases, 1); }
void kernel_tests_scheduler(void) { run(sched_cases, sched_case_count); }
void kernel_tests_tasks(void) { run(task_cases, task_case_count); }
void kernel_tests_ufs2(void) { run(ufs2_journal_cases, ufs2_journal_case_count); }
void kernel_tests_ufs2fs(void) { run(ufs2_fs_cases, ufs2_fs_case_count); }
void kernel_tests_ufs2crash(void) {
    run(ufs2_crash_cases, ufs2_crash_case_count);
    report("CRASH MATRIX RUNS "); report_number(ufs2_crash_total_runs()); report("\n");
}
void kernel_tests_storage_disk(void) { run(storage_disk_cases, storage_disk_case_count); }
void kernel_tests_shell(void) { run(shell_cases, sizeof(shell_cases)/sizeof(shell_cases[0])); }
void kernel_tests_all(void) {
    const struct test_case *groups[] = {memory_cases, interrupt_cases, filesystem_cases, shell_cases, storage_cases, ultriumfs_cases, ufs2_journal_cases, ufs2_fs_cases, task_cases, sched_cases};
    const size_t counts[] = {
        sizeof(memory_cases)/sizeof(memory_cases[0]),
        sizeof(interrupt_cases)/sizeof(interrupt_cases[0]),
        sizeof(filesystem_cases)/sizeof(filesystem_cases[0]),
        sizeof(shell_cases)/sizeof(shell_cases[0]),
        storage_case_count,
        ultriumfs_case_count,
        ufs2_journal_case_count,
        ufs2_fs_case_count,
        task_case_count,
        sched_case_count
    };
    run_groups(groups, counts, sizeof(groups)/sizeof(groups[0]));
}
void kernel_test_slab_stress(void) {
    const struct test_case legacy_slab_case = {"slab stress", slab_stress};
    run(&legacy_slab_case, 1);
}

void kernel_tests_shell_interactive(void) {
    const struct test_case test = {"USH QMP interactive session", shell_interactive_session};
    run(&test, 1);
}
void tests_serial_text(const char *text) { report(text); }
void tests_serial_number(uint64_t value) { report_number(value); }
