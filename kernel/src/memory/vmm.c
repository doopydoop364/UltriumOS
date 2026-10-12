#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
static uint64_t offset;
void vmm_init(uint64_t direct_map_offset) { offset = direct_map_offset; }
void *vmm_direct_map(uint64_t physical) {
    if (!offset || physical > UINT64_MAX - offset) return (void *)0;
    return (void *)(uintptr_t)(offset + physical);
}
uint64_t vmm_direct_offset(void) { return offset; }

/* Inspect Limine's active x86-64 page tables without modifying them.
   Supports both 4-level and 5-level paging plus 1 GiB and 2 MiB leaves. */
#define PRESENT 1ULL
#define LARGE_PAGE (1ULL << 7)
#define PHYS_MASK 0x000ffffffffff000ULL
bool vmm_translate(uint64_t address, uint64_t *physical_address) {
#if defined(__x86_64__)
    if (!offset || !physical_address) return false;
    uint64_t cr3, cr4;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    int levels = (cr4 & (1ULL << 12)) ? 5 : 4;
    uint64_t sign_bit = (uint64_t)1 << (levels == 5 ? 56 : 47);
    uint64_t upper = address & ~(sign_bit - 1);
    if (upper != 0 && upper != ~(sign_bit - 1)) return false;
    uint64_t table_phys = cr3 & PHYS_MASK;
    for (int level = levels; level >= 1; --level) {
        uint64_t *table = (uint64_t *)vmm_direct_map(table_phys);
        if (!table) return false;
        uint64_t index = (address >> (12 + 9 * (level - 1))) & 511;
        uint64_t entry = table[index];
        if (!(entry & PRESENT)) return false;
        if (level == 1 || ((level == 2 || level == 3) && (entry & LARGE_PAGE))) {
            unsigned shift = (unsigned)(12 + 9 * (level - 1));
            uint64_t within = (1ULL << shift) - 1;
            *physical_address = (entry & PHYS_MASK & ~within) | (address & within);
            return true;
        }
        table_phys = entry & PHYS_MASK;
    }
#else
    (void)address; (void)physical_address;
#endif
    return false;
}
bool vmm_self_test(void) {
    /* Use a PMM allocation: the HHDM must resolve back to exactly its
       physical address, including a nonzero offset inside the page. */
    uint64_t before = pmm_free_pages();
    uint64_t physical = pmm_alloc_page();
    if (!physical) return false;
    uint64_t mapped = 0;
    void *ptr = vmm_direct_map(physical);
    bool ok = ptr && vmm_translate((uint64_t)(uintptr_t)ptr + 17, &mapped) &&
              mapped == physical + 17;
    if (!pmm_free_page(physical)) ok = false;
    return ok && pmm_free_pages() == before;
}

/* v0.1.1: isolated, temporary mapping exercise.
   Never overwrite an existing PML4 entry or expose arbitrary mapping writes. */
static uint64_t scratch_va, scratch_pdpt, scratch_pt;
static uint16_t scratch_slot;
static bool scratch_ready;

static void zero_table(uint64_t physical) {
    uint64_t *table = (uint64_t *)vmm_direct_map(physical);
    for (unsigned i = 0; i < 512; ++i) table[i] = 0;
}
static void invalidate(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}
bool vmm_mapping_init(void) {
    uint64_t cr3, cr4;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1ULL << 12)) return false; /* 5-level not yet writable */
    uint64_t *root = (uint64_t *)vmm_direct_map(cr3 & PHYS_MASK);
    if (!root || scratch_ready) return false;
    unsigned index = 256;
    while (index < 511 && (root[index] & PRESENT)) ++index;
    if (index == 511) return false;
    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    uint64_t c = pmm_alloc_page();
    if (!a || !b || !c) {
        if (a) pmm_free_page(a);
        if (b) pmm_free_page(b);
        if (c) pmm_free_page(c);
        return false;
    }
    zero_table(a); zero_table(b); zero_table(c);
    uint64_t *pdpt = (uint64_t *)vmm_direct_map(a);
    uint64_t *pd = (uint64_t *)vmm_direct_map(b);
    pdpt[0] = b | 3;
    pd[0] = c | 3;
    /* PML4 upper entries sign extend to 64 bits. */
    uint64_t va = 0xffff000000000000ULL | ((uint64_t)index << 39);
    root[index] = a | 3;
    scratch_va = va;
    scratch_pdpt = a; scratch_pt = c;
    scratch_slot = (uint16_t)index;
    scratch_ready = true;
    return true;
}
bool vmm_map_test(void) {
    test_error = 0;
    if (!scratch_ready) { test_error = 1; return false; }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *root = (uint64_t *)vmm_direct_map(cr3 & PHYS_MASK);
    uint64_t *pt = (uint64_t *)vmm_direct_map(scratch_pt);
    if (!root || !pt || root[scratch_slot] != (scratch_pdpt | 3) ||
        pt[0] != 0) { test_error = 2; return false; }
    uint64_t before = pmm_free_pages();
    uint64_t page = pmm_alloc_page();
    if (!page) { test_error = 3; return false; }
    bool ok = false;
    pt[0] = page | 3; /* writable kernel-only scratch mapping */
    invalidate(scratch_va);
    uint64_t translated = 0;
    if (vmm_translate(scratch_va + 29, &translated) &&
        translated == page + 29) {
        volatile uint8_t *alias = (volatile uint8_t *)(uintptr_t)scratch_va;
        volatile uint8_t *direct = (volatile uint8_t *)vmm_direct_map(page);
        alias[29] = 0x6b;
        ok = direct[29] == 0x6b;
    }
    if (!ok) test_error = 4;
    pt[0] = 0;
    invalidate(scratch_va);
    if (vmm_translate(scratch_va, &translated)) { test_error = 5; ok = false; }
    if (!pmm_free_page(page)) { test_error = 6; ok = false; }
    if (pmm_free_pages() != before) { test_error = 7; ok = false; }
    return ok;
}

/* Managed pages occupy scratch PT slots 1..32; slot 0 remains reserved
   for the earlier temporary mapping test. Single-core only. */
#define MANAGED_SLOTS 32
static uint64_t managed_physical[MANAGED_SLOTS];
static uint64_t managed_live_count;
static unsigned test_error;
unsigned vmm_test_error(void) { return test_error; }
void *vmm_managed_alloc(void) {
    if (!scratch_ready) return (void *)0;
    uint64_t *pt = (uint64_t *)vmm_direct_map(scratch_pt);
    if (!pt) return (void *)0;
    for (unsigned i = 0; i < MANAGED_SLOTS; ++i) {
        if (managed_physical[i] || pt[i + 1] != 0) continue;
        uint64_t physical = pmm_alloc_page();
        if (!physical) return (void *)0;
        uint64_t virtual_address = scratch_va + (uint64_t)(i + 1) * 4096;
        pt[i + 1] = physical | 3;
        invalidate(virtual_address);
        managed_physical[i] = physical;
        ++managed_live_count;
        uint8_t *data = (uint8_t *)(uintptr_t)virtual_address;
        for (size_t byte = 0; byte < 4096; ++byte) data[byte] = 0;
        return data;
    }
    return (void *)0;
}
bool vmm_managed_free(void *address) {
    if (!scratch_ready || !address) return false;
    uint64_t va = (uint64_t)(uintptr_t)address;
    if (va <= scratch_va || va >= scratch_va + 33ULL * 4096 ||
        ((va - scratch_va) & 4095)) return false;
    unsigned slot = (unsigned)((va - scratch_va) / 4096 - 1);
    uint64_t physical = managed_physical[slot];
    if (!physical) return false;
    uint64_t *pt = (uint64_t *)vmm_direct_map(scratch_pt);
    if (!pt || pt[slot + 1] != (physical | 3)) return false;
    /* Drop the alias and invalidate before returning the frame to the PMM. */
    pt[slot + 1] = 0;
    invalidate(va);
    if (!pmm_free_page(physical)) return false;
    managed_physical[slot] = 0;
    --managed_live_count;
    return true;
}
uint64_t vmm_managed_live(void) { return managed_live_count; }
bool vmm_managed_test(void) {
    test_error = 0;
    if (!scratch_ready || managed_live_count) { test_error = 11; return false; }
    uint64_t before = pmm_free_pages();
    void *pages[8] = {0};
    bool ok = true;
    for (unsigned i = 0; i < 8; ++i) {
        pages[i] = vmm_managed_alloc();
        if (!pages[i]) { test_error = 12; ok = false; break; }
        uint8_t *bytes = (uint8_t *)pages[i];
        if (bytes[0] != 0 || bytes[4095] != 0) { test_error = 13; ok = false; }
        bytes[0] = (uint8_t)(0x30 + i);
        bytes[4095] = (uint8_t)(0x80 + i);
    }
    for (unsigned i = 0; i < 8; ++i) {
        if (!pages[i]) continue;
        uint8_t *bytes = (uint8_t *)pages[i];
        uint64_t physical;
        if (bytes[0] != (uint8_t)(0x30 + i) ||
            bytes[4095] != (uint8_t)(0x80 + i) ||
            !vmm_translate((uint64_t)(uintptr_t)pages[i], &physical) ||
            physical != managed_physical[i]) { test_error = 14; ok = false; }
    }
    for (unsigned i = 0; i < 8; ++i)
        if (pages[i] && !vmm_managed_free(pages[i])) { test_error = 15; ok = false; }
    if (managed_live_count != 0 || pmm_free_pages() != before) { test_error = 16; ok = false; }
    uint64_t ignored;
    for (unsigned i = 0; i < 8; ++i)
        if (pages[i] && vmm_translate((uint64_t)(uintptr_t)pages[i], &ignored)) { test_error = 17; ok = false; }
    return ok;
}
