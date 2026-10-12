#include <stdint.h>
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
