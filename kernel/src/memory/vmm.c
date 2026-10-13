#include <stdint.h>
#include <stddef.h>
#include "vmm.h"
#include "pmm.h"
#include "vmm_test.h"
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
        volatile uint64_t *table = (volatile uint64_t *)vmm_direct_map(table_phys);
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
static uint64_t scratch_va, scratch_root, scratch_pdpt, scratch_pd, scratch_pt;
static uint16_t scratch_slot;
static bool scratch_ready;

static void zero_table(uint64_t physical) {
    volatile uint64_t *table = (volatile uint64_t *)vmm_direct_map(physical);
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
    volatile uint64_t *root = (volatile uint64_t *)vmm_direct_map(cr3 & PHYS_MASK);
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
    volatile uint64_t *pdpt = (volatile uint64_t *)vmm_direct_map(a);
    volatile uint64_t *pd = (volatile uint64_t *)vmm_direct_map(b);
    pdpt[0] = b | 3;
    pd[0] = c | 3;
    /* PML4 upper entries sign extend to 64 bits. */
    uint64_t va = 0xffff000000000000ULL | ((uint64_t)index << 39);
    root[index] = a | 3;
    scratch_va = va;
    scratch_root = cr3 & PHYS_MASK;
    scratch_pdpt = a; scratch_pd = b; scratch_pt = c;
    scratch_slot = (uint16_t)index;
    scratch_ready = true;
    return true;
}
static unsigned test_error;
bool vmm_map_test(void) {
    test_error = 0;
    if (!scratch_ready) { test_error = 1; return false; }
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    volatile uint64_t *root = (volatile uint64_t *)vmm_direct_map(cr3 & PHYS_MASK);
    volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
    if (!root || !pt) { test_error = 2; return false; }
    /* The CPU sets Accessed on upper-level entries during address walks. */
    if ((root[scratch_slot] & (PHYS_MASK | 3ULL)) != (scratch_pdpt | 3)) {
        test_error = 8; return false;
    }
    if (pt[0] != 0) { test_error = 9; return false; }
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
unsigned vmm_test_error(void) { return test_error; }
void *vmm_managed_alloc(void) {
    if (!scratch_ready) return (void *)0;
    volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
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
    volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
    /* The CPU may set Accessed (bit 5) and Dirty (bit 6) on a leaf. */
    if (!pt || (pt[slot + 1] & (PHYS_MASK | 3ULL)) != (physical | 3))
        return false;
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
    for (unsigned i = 0; i < 8; ++i) {
        if (!pages[i]) continue;
        volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
        if (!pt || pt[i + 1] != 0) { test_error = 18; ok = false; }
        if (vmm_translate((uint64_t)(uintptr_t)pages[i], &ignored)) { test_error = 17; ok = false; }
    }
    return ok;
}

/* A bounded kernel-only region following the legacy scratch pages. */
#define REGION_FIRST_PAGE 33U
#define REGION_MAX_ALLOCS 64U
#define REGION_PT_COUNT ((REGION_FIRST_PAGE + VMM_REGION_MAX_PAGES + 511U) / 512U)
#define PAGE_SIZE 4096ULL
#define ACCESSED (1ULL << 5)
#define DIRTY (1ULL << 6)
enum region_page_state { REGION_FREE, REGION_RESERVED, REGION_MAPPED, REGION_ORPHAN };
enum region_allocation_state { ALLOCATION_FREE, ALLOCATION_BUILDING, ALLOCATION_LIVE, ALLOCATION_ORPHAN };
struct region_allocation { uint16_t first, count; uint8_t state; };
static struct region_allocation region_allocations[REGION_MAX_ALLOCS];
static uint64_t region_frames[VMM_REGION_MAX_PAGES];
static uint8_t region_page_states[VMM_REGION_MAX_PAGES];
static uint64_t region_pt_frames[REGION_PT_COUNT];
/* Static transaction storage avoids depending on the kernel stack size. */
static uint64_t transaction_frames[VMM_REGION_MAX_PAGES];
static bool region_busy;
static int32_t injected_fail_after = -1;
static uint32_t injected_allocations;

void vmm_test_fail_alloc_after(int32_t successful_allocations) {
    injected_fail_after = successful_allocations;
    injected_allocations = 0;
}

static uint64_t region_va(unsigned page) {
    return scratch_va + ((uint64_t)REGION_FIRST_PAGE + page) * PAGE_SIZE;
}

static uint64_t allocate_backing_page(void) {
    if (injected_fail_after >= 0) {
        if (injected_allocations >= (uint32_t)injected_fail_after) {
            injected_fail_after = -1;
            return 0;
        }
        ++injected_allocations;
    }
    return pmm_alloc_page();
}

static bool region_tables_ready(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PHYS_MASK) != scratch_root || !scratch_root || !scratch_pdpt ||
        !scratch_pd || !scratch_pt) return false;
    volatile uint64_t *root = (volatile uint64_t *)vmm_direct_map(scratch_root);
    volatile uint64_t *pdpt = (volatile uint64_t *)vmm_direct_map(scratch_pdpt);
    volatile uint64_t *pd = (volatile uint64_t *)vmm_direct_map(scratch_pd);
    if (!root || !pdpt || !pd || !scratch_ready) return false;
    if ((root[scratch_slot] & (PHYS_MASK | 3ULL)) != (scratch_pdpt | 3ULL) ||
        (pdpt[0] & (PHYS_MASK | 3ULL)) != (scratch_pd | 3ULL) ||
        (pd[0] & (PHYS_MASK | 3ULL)) != (scratch_pt | 3ULL)) return false;
    return true;
}

/* Return a tracked leaf table. Only directory indexes reached by the bounded
   window are accepted; an untracked present entry is treated as foreign. */
static volatile uint64_t *region_pt(unsigned index, bool create, bool *created) {
    if (created) *created = false;
    if (index >= REGION_PT_COUNT || !region_tables_ready()) return (void *)0;
    volatile uint64_t *pd = (volatile uint64_t *)vmm_direct_map(scratch_pd);
    if (!pd) return (void *)0;
    uint64_t entry = pd[index];
    if (index == 0) {
        if ((entry & (PHYS_MASK | 3ULL)) != (scratch_pt | 3ULL) || (entry & LARGE_PAGE))
            return (void *)0;
        return (volatile uint64_t *)vmm_direct_map(scratch_pt);
    }
    uint64_t tracked = region_pt_frames[index];
    if (tracked) {
        if ((entry & (PHYS_MASK | 3ULL)) != (tracked | 3ULL) || (entry & LARGE_PAGE))
            return (void *)0;
        return (volatile uint64_t *)vmm_direct_map(tracked);
    }
    if (entry != 0 || !create) return (void *)0;
    uint64_t physical = pmm_alloc_page();
    if (!physical) return (void *)0;
    volatile uint64_t *table = (volatile uint64_t *)vmm_direct_map(physical);
    if (!table) { pmm_free_page(physical); return (void *)0; }
    for (unsigned i = 0; i < 512; ++i) table[i] = 0;
    pd[index] = physical | 3ULL;
    region_pt_frames[index] = physical;
    if (created) *created = true;
    return table;
}

static volatile uint64_t *region_leaf(unsigned page, bool create, bool *created) {
    uint64_t va = region_va(page);
    unsigned pd_index = (unsigned)((va >> 21) & 511U);
    unsigned pt_index = (unsigned)((va >> 12) & 511U);
    volatile uint64_t *pt = region_pt(pd_index, create, created);
    return pt ? &pt[pt_index] : (volatile uint64_t *)0;
}

static bool region_pt_empty(unsigned index) {
    volatile uint64_t *pt = region_pt(index, false, 0);
    if (!pt) return index == 0;
    for (unsigned i = 0; i < 512; ++i) if (pt[i] & 1ULL) return false;
    return true;
}

static bool reclaim_region_pt(unsigned index) {
    if (index == 0 || index >= REGION_PT_COUNT || !region_pt_frames[index]) return true;
    if (!region_pt_empty(index)) return false;
    volatile uint64_t *pd = (volatile uint64_t *)vmm_direct_map(scratch_pd);
    if (!pd) return false;
    uint64_t physical = region_pt_frames[index];
    uint64_t old_entry = pd[index];
    if ((old_entry & (PHYS_MASK | 3ULL)) != (physical | 3ULL)) return false;
    pd[index] = 0;
    if (!pmm_free_page(physical)) {
        pd[index] = old_entry;
        return false;
    }
    region_pt_frames[index] = 0;
    return true;
}

static void rollback_region(unsigned record, unsigned first, unsigned count,
                            unsigned mapped, const uint8_t *created_tables) {
    for (unsigned i = 0; i < mapped; ++i) {
        volatile uint64_t *leaf = region_leaf(first + i, false, 0);
        if (leaf) *leaf = 0;
        invalidate(region_va(first + i));
    }
    bool orphaned = false;
    for (unsigned i = 0; i < count; ++i) {
        uint64_t frame = transaction_frames[i];
        if (!frame) { region_page_states[first + i] = REGION_FREE; continue; }
        if (pmm_free_page(frame)) {
            transaction_frames[i] = 0;
            region_frames[first + i] = 0;
            region_page_states[first + i] = REGION_FREE;
        } else {
            region_frames[first + i] = frame;
            region_page_states[first + i] = REGION_ORPHAN;
            orphaned = true;
        }
    }
    for (unsigned i = REGION_PT_COUNT; i > 1; --i)
        if (created_tables[i - 1]) reclaim_region_pt(i - 1);
    if (orphaned) region_allocations[record].state = ALLOCATION_ORPHAN;
    else region_allocations[record] = (struct region_allocation){0};
}

void *vmm_alloc_pages(size_t count) {
    if (!count || count > VMM_REGION_MAX_PAGES || !scratch_ready ||
        region_busy) return (void *)0;
    unsigned record = REGION_MAX_ALLOCS;
    for (unsigned i = 0; i < REGION_MAX_ALLOCS; ++i)
        if (region_allocations[i].state == ALLOCATION_FREE) { record = i; break; }
    if (record == REGION_MAX_ALLOCS) return (void *)0;
    unsigned first = VMM_REGION_MAX_PAGES;
    for (unsigned start = 0; start + count <= VMM_REGION_MAX_PAGES; ++start) {
        size_t i = 0;
        while (i < count && region_page_states[start + i] == REGION_FREE) ++i;
        if (i == count) { first = start; break; }
        start += (unsigned)i;
    }
    if (first == VMM_REGION_MAX_PAGES) return (void *)0;

    region_busy = true;
    region_allocations[record] = (struct region_allocation){
        .first = (uint16_t)first, .count = (uint16_t)count,
        .state = ALLOCATION_BUILDING
    };
    for (unsigned i = 0; i < count; ++i) {
        region_page_states[first + i] = REGION_RESERVED;
        transaction_frames[i] = 0;
    }
    uint8_t created_tables[REGION_PT_COUNT] = {0};
    bool ok = true;

    /* Build all required leaf tables before acquiring or publishing data pages. */
    unsigned previous_pd = REGION_PT_COUNT;
    for (unsigned i = 0; i < count; ++i) {
        unsigned pd_index = (unsigned)((region_va(first + i) >> 21) & 511U);
        if (pd_index == previous_pd) continue;
        bool created = false;
        if (!region_pt(pd_index, true, &created)) { ok = false; break; }
        if (created) created_tables[pd_index] = 1;
        previous_pd = pd_index;
    }
    for (unsigned i = 0; ok && i < count; ++i) {
        transaction_frames[i] = allocate_backing_page();
        if (!transaction_frames[i]) { ok = false; break; }
    }
    for (unsigned i = 0; ok && i < count; ++i) {
        volatile uint64_t *leaf = region_leaf(first + i, false, 0);
        if (!leaf || *leaf != 0) ok = false;
    }

    unsigned mapped = 0;
    for (unsigned i = 0; ok && i < count; ++i) {
        volatile uint64_t *leaf = region_leaf(first + i, false, 0);
        if (!leaf || *leaf != 0) { ok = false; break; }
        *leaf = transaction_frames[i] | 3ULL;
        invalidate(region_va(first + i));
        ++mapped;
    }
    if (!ok) {
        rollback_region(record, first, (unsigned)count, mapped, created_tables);
        region_busy = false;
        return (void *)0;
    }

    for (unsigned i = 0; i < count; ++i) {
        volatile uint8_t *data = (volatile uint8_t *)(uintptr_t)region_va(first + i);
        for (unsigned byte = 0; byte < PAGE_SIZE; ++byte) data[byte] = 0;
        region_frames[first + i] = transaction_frames[i];
        transaction_frames[i] = 0;
        region_page_states[first + i] = REGION_MAPPED;
    }
    region_allocations[record].state = ALLOCATION_LIVE;
    region_busy = false;
    return (void *)(uintptr_t)region_va(first);
}

static bool region_leaf_matches(volatile uint64_t *leaf, uint64_t physical) {
    return leaf && ((*leaf & ~(ACCESSED | DIRTY)) == (physical | 3ULL));
}

bool vmm_free_pages(void *address) {
    if (!address || !scratch_ready || region_busy) return false;
    uint64_t va = (uint64_t)(uintptr_t)address;
    unsigned record = REGION_MAX_ALLOCS;
    for (unsigned i = 0; i < REGION_MAX_ALLOCS; ++i) {
        struct region_allocation *allocation = &region_allocations[i];
        if (allocation->state == ALLOCATION_LIVE &&
            va == region_va(allocation->first)) { record = i; break; }
    }
    if (record == REGION_MAX_ALLOCS) return false;
    struct region_allocation allocation = region_allocations[record];

    /* Validate the entire allocation before the first page-table mutation. */
    for (unsigned i = 0; i < allocation.count; ++i) {
        unsigned page = allocation.first + i;
        volatile uint64_t *leaf = region_leaf(page, false, 0);
        if (region_page_states[page] != REGION_MAPPED || !region_frames[page] ||
            !region_leaf_matches(leaf, region_frames[page])) return false;
    }
    region_busy = true;
    for (unsigned i = 0; i < allocation.count; ++i) {
        unsigned page = allocation.first + i;
        volatile uint64_t *leaf = region_leaf(page, false, 0);
        *leaf = 0;
        invalidate(region_va(page));
        region_page_states[page] = REGION_RESERVED;
    }
    bool freed_all = true;
    for (unsigned i = 0; i < allocation.count; ++i) {
        unsigned page = allocation.first + i;
        if (pmm_free_page(region_frames[page])) {
            region_frames[page] = 0;
            region_page_states[page] = REGION_FREE;
        } else {
            region_page_states[page] = REGION_ORPHAN;
            freed_all = false;
        }
    }
    if (freed_all) region_allocations[record] = (struct region_allocation){0};
    else region_allocations[record].state = ALLOCATION_ORPHAN;

    unsigned first_pd = (unsigned)((region_va(allocation.first) >> 21) & 511U);
    unsigned last_pd = (unsigned)((region_va(allocation.first + allocation.count - 1) >> 21) & 511U);
    for (unsigned pd = first_pd; pd <= last_pd; ++pd) reclaim_region_pt(pd);
    region_busy = false;
    return freed_all;
}

bool vmm_region_stats_get(struct vmm_region_stats *stats) {
    if (!stats) return false;
    *stats = (struct vmm_region_stats){0};
    for (unsigned i = 0; i < VMM_REGION_MAX_PAGES; ++i) {
        if (region_page_states[i] != REGION_FREE) ++stats->reserved_virtual_pages;
        if (region_page_states[i] == REGION_MAPPED) ++stats->mapped_virtual_pages;
        if (region_frames[i]) ++stats->owned_backing_pages;
    }
    stats->free_virtual_pages = VMM_REGION_MAX_PAGES - stats->reserved_virtual_pages;
    for (unsigned i = 1; i < REGION_PT_COUNT; ++i)
        if (region_pt_frames[i]) ++stats->page_table_frames;
    return true;
}
