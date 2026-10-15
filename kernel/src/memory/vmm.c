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
#define WRITABLE (1ULL << 1)
#define NX (1ULL << 63)
#define EFER_MSR 0xc0000080U
#define EFER_NXE (1ULL << 11)
#define CR0_WP (1ULL << 16)
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
    struct pmm_page_handle owner = {0};
    if (!pmm_page_alloc(&owner)) return false;
    uint64_t physical = owner.physical;
    uint64_t mapped = 0;
    void *ptr = vmm_direct_map(physical);
    bool ok = ptr && vmm_translate((uint64_t)(uintptr_t)ptr + 17, &mapped) &&
              mapped == physical + 17;
    if (!pmm_page_release(&owner)) ok = false;
    return ok && pmm_free_pages() == before;
}

/* v0.1.1: isolated, temporary mapping exercise.
   Never overwrite an existing PML4 entry or expose arbitrary mapping writes. */
static uint64_t scratch_va, scratch_root, scratch_pdpt, scratch_pd, scratch_pt;
static struct pmm_page_handle scratch_owners[3];
static uint16_t scratch_slot;
static bool scratch_ready;
static bool page_protection_ready;

static bool enable_page_protection(void) {
#if defined(__x86_64__)
    uint32_t eax = 0x80000000U, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    uint32_t extended_max = eax;
    if (extended_max < 0x80000001U) return false;
    eax = 0x80000001U;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1U << 20))) return false; /* NX is optional in x86-64. */

    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(EFER_MSR));
    uint64_t efer = ((uint64_t)high << 32) | low;
    if (!(efer & EFER_NXE)) {
        efer |= EFER_NXE;
        __asm__ volatile("wrmsr" : : "a"((uint32_t)efer),
                         "d"((uint32_t)(efer >> 32)), "c"(EFER_MSR) : "memory");
    }
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    if (!(cr0 & CR0_WP)) {
        cr0 |= CR0_WP;
        __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    }
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(EFER_MSR));
    efer = ((uint64_t)high << 32) | low;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    page_protection_ready = (efer & EFER_NXE) && (cr0 & CR0_WP);
    return page_protection_ready;
#else
    return false;
#endif
}

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
    if (!enable_page_protection()) return false;
    volatile uint64_t *root = (volatile uint64_t *)vmm_direct_map(cr3 & PHYS_MASK);
    if (!root || scratch_ready) return false;
    unsigned index = 256;
    while (index < 511 && (root[index] & PRESENT)) ++index;
    if (index == 511) return false;
    struct pmm_page_handle owners[3] = {{0}};
    bool allocated_a = pmm_page_alloc(&owners[0]);
    bool allocated_b = allocated_a && pmm_page_alloc(&owners[1]);
    bool allocated_c = allocated_b && pmm_page_alloc(&owners[2]);
    if (!allocated_c) {
        if (owners[0].physical) pmm_page_release(&owners[0]);
        if (owners[1].physical) pmm_page_release(&owners[1]);
        if (owners[2].physical) pmm_page_release(&owners[2]);
        return false;
    }
    uint64_t a = owners[0].physical, b = owners[1].physical, c = owners[2].physical;
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
    for (unsigned i = 0; i < 3; ++i) {
        scratch_owners[i] = owners[i];
        owners[i] = (struct pmm_page_handle){0};
    }
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
    struct pmm_page_handle owner = {0};
    if (!pmm_page_alloc(&owner)) { test_error = 3; return false; }
    uint64_t page = owner.physical;
    bool ok = false;
    pt[0] = page | 3 | NX; /* writable kernel-only data mapping */
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
    if (!pmm_page_release(&owner)) { test_error = 6; ok = false; }
    if (pmm_free_pages() != before) { test_error = 7; ok = false; }
    return ok;
}

/* Managed pages occupy scratch PT slots 1..32; slot 0 remains reserved
   for the earlier temporary mapping test. Single-core only. */
#define MANAGED_SLOTS 32
static struct pmm_page_handle managed_owners[MANAGED_SLOTS];
static uint64_t managed_live_count;
unsigned vmm_test_error(void) { return test_error; }
void *vmm_managed_alloc(void) {
    if (!scratch_ready) return (void *)0;
    volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
    if (!pt) return (void *)0;
    for (unsigned i = 0; i < MANAGED_SLOTS; ++i) {
        if (managed_owners[i].physical || pt[i + 1] != 0) continue;
        struct pmm_page_handle owner = {0};
        if (!pmm_page_alloc(&owner)) return (void *)0;
        uint64_t physical = owner.physical;
        uint64_t virtual_address = scratch_va + (uint64_t)(i + 1) * 4096;
        pt[i + 1] = physical | 3 | NX;
        invalidate(virtual_address);
        managed_owners[i] = owner;
        owner = (struct pmm_page_handle){0};
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
    struct pmm_page_handle *owner = &managed_owners[slot];
    uint64_t physical = owner->physical;
    if (!physical || !pmm_page_handle_valid(owner)) return false;
    volatile uint64_t *pt = (volatile uint64_t *)vmm_direct_map(scratch_pt);
    /* The CPU may set Accessed (bit 5) and Dirty (bit 6) on a leaf. */
    if (!pt || (pt[slot + 1] & (PHYS_MASK | 3ULL)) != (physical | 3))
        return false;
    uint64_t old_entry = pt[slot + 1];
    /* Drop the alias and invalidate before returning the frame to the PMM. */
    pt[slot + 1] = 0;
    invalidate(va);
    if (!pmm_page_release(owner)) {
        pt[slot + 1] = old_entry;
        invalidate(va);
        return false;
    }
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
            physical != managed_owners[i].physical) { test_error = 14; ok = false; }
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
struct region_allocation { uint16_t first, count; uint8_t state; uint32_t permissions; };
static struct region_allocation region_allocations[REGION_MAX_ALLOCS];
static struct pmm_page_handle region_owners[VMM_REGION_MAX_PAGES];
static uint8_t region_page_states[VMM_REGION_MAX_PAGES];
static struct pmm_page_handle region_pt_owners[REGION_PT_COUNT];
/* Static transaction storage avoids depending on the kernel stack size. */
static struct pmm_page_handle transaction_owners[VMM_REGION_MAX_PAGES];
static bool region_busy;
static uint64_t local_apic_va;
static uint64_t ioapic_va;
static int32_t injected_fail_after = -1;
static uint32_t injected_allocations;

void vmm_test_fail_alloc_after(int32_t successful_allocations) {
    injected_fail_after = successful_allocations;
    injected_allocations = 0;
}

static uint64_t region_va(unsigned page) {
    return scratch_va + ((uint64_t)REGION_FIRST_PAGE + page) * PAGE_SIZE;
}

static bool allocate_backing_page(struct pmm_page_handle *owner) {
    if (injected_fail_after >= 0) {
        if (injected_allocations >= (uint32_t)injected_fail_after) {
            injected_fail_after = -1;
            return false;
        }
        ++injected_allocations;
    }
    return pmm_page_alloc(owner);
}

static bool region_tables_ready(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if ((cr3 & PHYS_MASK) != scratch_root || !scratch_root || !scratch_pdpt ||
        !scratch_pd || !scratch_pt) return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!pmm_page_handle_valid(&scratch_owners[i])) return false;
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
    uint64_t tracked = region_pt_owners[index].physical;
    if (tracked) {
        if (!pmm_page_handle_valid(&region_pt_owners[index]) ||
            (entry & (PHYS_MASK | 3ULL)) != (tracked | 3ULL) || (entry & LARGE_PAGE))
            return (void *)0;
        return (volatile uint64_t *)vmm_direct_map(tracked);
    }
    if (entry != 0 || !create) return (void *)0;
    struct pmm_page_handle owner = {0};
    if (!pmm_page_alloc(&owner)) return (void *)0;
    uint64_t physical = owner.physical;
    volatile uint64_t *table = (volatile uint64_t *)vmm_direct_map(physical);
    if (!table) { pmm_page_release(&owner); return (void *)0; }
    for (unsigned i = 0; i < 512; ++i) table[i] = 0;
    pd[index] = physical | 3ULL;
    region_pt_owners[index] = owner;
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
    if (index == 0 || index >= REGION_PT_COUNT || !region_pt_owners[index].physical) return true;
    if (!region_pt_empty(index)) return false;
    volatile uint64_t *pd = (volatile uint64_t *)vmm_direct_map(scratch_pd);
    if (!pd) return false;
    struct pmm_page_handle *owner = &region_pt_owners[index];
    uint64_t physical = owner->physical;
    uint64_t old_entry = pd[index];
    if ((old_entry & (PHYS_MASK | 3ULL)) != (physical | 3ULL)) return false;
    pd[index] = 0;
    if (!pmm_page_release(owner)) {
        pd[index] = old_entry;
        return false;
    }
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
        struct pmm_page_handle *owner = &transaction_owners[i];
        if (!owner->physical) { region_page_states[first + i] = REGION_FREE; continue; }
        if (pmm_page_release(owner)) {
            region_owners[first + i] = (struct pmm_page_handle){0};
            region_page_states[first + i] = REGION_FREE;
        } else {
            region_owners[first + i] = *owner;
            *owner = (struct pmm_page_handle){0};
            region_page_states[first + i] = REGION_ORPHAN;
            orphaned = true;
        }
    }
    for (unsigned i = REGION_PT_COUNT; i > 1; --i)
        if (created_tables[i - 1]) reclaim_region_pt(i - 1);
    if (orphaned) region_allocations[record].state = ALLOCATION_ORPHAN;
    else region_allocations[record] = (struct region_allocation){0};
}

static bool valid_region_permissions(uint32_t permissions) {
    const uint32_t allowed = VMM_PAGE_READ | VMM_PAGE_WRITE | VMM_PAGE_EXECUTE;
    return (permissions & VMM_PAGE_READ) && !(permissions & ~allowed) &&
           (permissions & (VMM_PAGE_WRITE | VMM_PAGE_EXECUTE)) !=
               (VMM_PAGE_WRITE | VMM_PAGE_EXECUTE);
}

static uint64_t region_leaf_flags(uint32_t permissions) {
    uint64_t flags = PRESENT;
    if (permissions & VMM_PAGE_WRITE) flags |= WRITABLE;
    if (!(permissions & VMM_PAGE_EXECUTE)) flags |= NX;
    return flags; /* USER is intentionally never set. */
}

void *vmm_alloc_pages_with_permissions(size_t count, uint32_t permissions) {
    if (!count || count > VMM_REGION_MAX_PAGES || !scratch_ready ||
        !page_protection_ready || !valid_region_permissions(permissions) ||
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
        .state = ALLOCATION_BUILDING, .permissions = permissions
    };
    for (unsigned i = 0; i < count; ++i) {
        region_page_states[first + i] = REGION_RESERVED;
        transaction_owners[i] = (struct pmm_page_handle){0};
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
        if (!allocate_backing_page(&transaction_owners[i])) { ok = false; break; }
    }
    /* Initialize through Limine's writable direct map before publishing the
       requested leaf permissions (which may intentionally be read-only). */
    for (unsigned i = 0; ok && i < count; ++i) {
        volatile uint8_t *data = (volatile uint8_t *)vmm_direct_map(transaction_owners[i].physical);
        if (!data) { ok = false; break; }
        for (unsigned byte = 0; byte < PAGE_SIZE; ++byte) data[byte] = 0;
    }
    for (unsigned i = 0; ok && i < count; ++i) {
        volatile uint64_t *leaf = region_leaf(first + i, false, 0);
        if (!leaf || *leaf != 0) ok = false;
    }

    unsigned mapped = 0;
    for (unsigned i = 0; ok && i < count; ++i) {
        volatile uint64_t *leaf = region_leaf(first + i, false, 0);
        if (!leaf || *leaf != 0) { ok = false; break; }
        *leaf = transaction_owners[i].physical | region_leaf_flags(permissions);
        invalidate(region_va(first + i));
        ++mapped;
    }
    if (!ok) {
        rollback_region(record, first, (unsigned)count, mapped, created_tables);
        region_busy = false;
        return (void *)0;
    }

    for (unsigned i = 0; i < count; ++i) {
        region_owners[first + i] = transaction_owners[i];
        transaction_owners[i] = (struct pmm_page_handle){0};
        region_page_states[first + i] = REGION_MAPPED;
    }
    region_allocations[record].state = ALLOCATION_LIVE;
    region_busy = false;
    return (void *)(uintptr_t)region_va(first);
}

void *vmm_alloc_pages(size_t count) {
    return vmm_alloc_pages_with_permissions(count, VMM_PAGE_READ | VMM_PAGE_WRITE);
}

static bool region_leaf_matches(volatile uint64_t *leaf, uint64_t physical,
                                uint32_t permissions) {
    return leaf && ((*leaf & ~(ACCESSED | DIRTY)) ==
                    (physical | region_leaf_flags(permissions)));
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
        if (region_page_states[page] != REGION_MAPPED || !region_owners[page].physical ||
            !pmm_page_handle_valid(&region_owners[page]) ||
            !region_leaf_matches(leaf, region_owners[page].physical, allocation.permissions)) return false;
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
        if (pmm_page_release(&region_owners[page])) {
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

bool vmm_set_pages_permissions(void *address, uint32_t permissions) {
    if (!address || !scratch_ready || !page_protection_ready || region_busy ||
        !valid_region_permissions(permissions)) return false;
    uint64_t va = (uint64_t)(uintptr_t)address;
    unsigned record = REGION_MAX_ALLOCS;
    for (unsigned i = 0; i < REGION_MAX_ALLOCS; ++i)
        if (region_allocations[i].state == ALLOCATION_LIVE &&
            va == region_va(region_allocations[i].first)) { record = i; break; }
    if (record == REGION_MAX_ALLOCS) return false;
    struct region_allocation *allocation = &region_allocations[record];
    for (unsigned i = 0; i < allocation->count; ++i) {
        unsigned page = allocation->first + i;
        volatile uint64_t *leaf = region_leaf(page, false, 0);
        if (region_page_states[page] != REGION_MAPPED || !region_owners[page].physical ||
            !pmm_page_handle_valid(&region_owners[page]) ||
            !region_leaf_matches(leaf, region_owners[page].physical, allocation->permissions))
            return false;
    }
    region_busy = true;
    uint64_t clear = WRITABLE | NX;
    uint64_t set = region_leaf_flags(permissions);
    for (unsigned i = 0; i < allocation->count; ++i) {
        unsigned page = allocation->first + i;
        volatile uint64_t *leaf = region_leaf(page, false, 0);
        *leaf = (*leaf & ~clear) | set;
        invalidate(region_va(page));
    }
    allocation->permissions = permissions;
    region_busy = false;
    return true;
}

bool vmm_get_page_permissions(const void *address, uint32_t *permissions) {
    if (!address || !permissions || !scratch_ready || !page_protection_ready ||
        region_busy) return false;
    uint64_t va = (uint64_t)(uintptr_t)address;
    if (va & (PAGE_SIZE - 1)) return false;
    for (unsigned i = 0; i < REGION_MAX_ALLOCS; ++i) {
        struct region_allocation *allocation = &region_allocations[i];
        if (allocation->state != ALLOCATION_LIVE) continue;
        uint64_t base = region_va(allocation->first);
        uint64_t bytes = (uint64_t)allocation->count * PAGE_SIZE;
        if (va < base || va - base >= bytes) continue;
        unsigned page = allocation->first + (unsigned)((va - base) / PAGE_SIZE);
        volatile uint64_t *leaf = region_leaf(page, false, 0);
        if (region_page_states[page] != REGION_MAPPED || !region_owners[page].physical ||
            !pmm_page_handle_valid(&region_owners[page]) ||
            !region_leaf_matches(leaf, region_owners[page].physical, allocation->permissions))
            return false;
        *permissions = allocation->permissions;
        return true;
    }
    return false;
}

bool vmm_page_protection_ready(void) { return page_protection_ready; }

bool vmm_region_stats_get(struct vmm_region_stats *stats) {
    if (!stats) return false;
    *stats = (struct vmm_region_stats){0};
    for (unsigned i = 0; i < VMM_REGION_MAX_PAGES; ++i) {
        if (region_page_states[i] != REGION_FREE) ++stats->reserved_virtual_pages;
        if (region_page_states[i] == REGION_MAPPED) ++stats->mapped_virtual_pages;
        if (region_owners[i].physical) ++stats->owned_backing_pages;
    }
    stats->free_virtual_pages = VMM_REGION_MAX_PAGES - stats->reserved_virtual_pages;
    for (unsigned i = 1; i < REGION_PT_COUNT; ++i)
        if (region_pt_owners[i].physical) ++stats->page_table_frames;
    return true;
}

static void *map_fixed_mmio_page(uint64_t physical_address, unsigned page,
                                 uint64_t *stored_va) {
    if (!scratch_ready || region_busy || !stored_va) return (void *)0;
    if (*stored_va) return (void *)(uintptr_t)*stored_va;
    unsigned index = (unsigned)(((region_va(page) >> 21) & 511U));
    unsigned slot = (unsigned)((region_va(page) >> 12) & 511U);
    bool created = false;
    volatile uint64_t *pt = region_pt(index, true, &created);
    if (!pt) return (void *)0;
    if (pt[slot]) {
        if (created) reclaim_region_pt(index);
        return (void *)0;
    }
    uint64_t mapping = physical_address | 3ULL | (1ULL << 3) |
                       (1ULL << 4) | NX;
    pt[slot] = mapping;
    uint64_t va = region_va(page);
    invalidate(va);
    uint64_t translated;
    if (!vmm_translate(va, &translated) || (translated & ~0xfffULL) != physical_address) {
        pt[slot] = 0;
        invalidate(va);
        if (created) reclaim_region_pt(index);
        return (void *)0;
    }
    *stored_va = va;
    return (void *)(uintptr_t)va;
}

void *vmm_map_local_apic(uint64_t physical_address) {
    if (physical_address != 0xfee00000ULL) return (void *)0;
    return map_fixed_mmio_page(physical_address, VMM_REGION_MAX_PAGES, &local_apic_va);
}

void *vmm_map_ioapic(uint64_t physical_address) {
    if (physical_address != 0xfec00000ULL) return (void *)0;
    return map_fixed_mmio_page(physical_address, VMM_REGION_MAX_PAGES + 1,
                               &ioapic_va);
}

/* Borrowed uncached device MMIO (for example PCI BARs). Pages come from a
   fixed bump pool after the APIC pages and are never PMM-owned or unmapped;
   a driver maps its BAR once for the lifetime of the kernel. */
#define MMIO_POOL_FIRST (VMM_REGION_MAX_PAGES + 2U)
#define MMIO_POOL_PAGES 16U
static uint64_t mmio_va[MMIO_POOL_PAGES];
static unsigned mmio_used;
void *vmm_map_mmio(uint64_t physical_address, size_t length) {
    uint64_t offset = physical_address & 0xfffULL, base = physical_address - offset;
    if (!length || length > MMIO_POOL_PAGES * PAGE_SIZE || physical_address >= (1ULL << 47) ||
        length > (size_t)(~0ULL >> 17) || offset + length < offset) return (void *)0;
    unsigned pages = (unsigned)((offset + length + PAGE_SIZE - 1) / PAGE_SIZE);
    if (pages > MMIO_POOL_PAGES - mmio_used) return (void *)0;
    unsigned first = mmio_used;
    for (unsigned i = 0; i < pages; ++i) {
        if (!map_fixed_mmio_page(base + (uint64_t)i * PAGE_SIZE, MMIO_POOL_FIRST + first + i,
                                 &mmio_va[first + i])) {
            mmio_used = first + i;  /* Already-mapped pages stay mapped, but unused. */
            return (void *)0;
        }
    }
    mmio_used = first + pages;
    return (void *)(uintptr_t)(mmio_va[first] + offset);
}
