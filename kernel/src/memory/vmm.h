#ifndef ULTRIUM_VMM_H
#define ULTRIUM_VMM_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
void vmm_init(uint64_t direct_map_offset);
void *vmm_direct_map(uint64_t physical);
uint64_t vmm_direct_offset(void);
bool vmm_translate(uint64_t virtual_address, uint64_t *physical_address);
bool vmm_self_test(void);
bool vmm_mapping_init(void);
bool vmm_map_test(void);
void *vmm_managed_alloc(void);
bool vmm_managed_free(void *address);
uint64_t vmm_managed_live(void);
bool vmm_managed_test(void);
unsigned vmm_test_error(void);

#define VMM_REGION_MAX_PAGES 4096U
/* Region allocations own their physical frames and return a zeroed contiguous
   virtual span. Free only the exact pointer returned by the allocator. */
struct vmm_region_stats {
    uint64_t reserved_virtual_pages;
    uint64_t mapped_virtual_pages;
    uint64_t owned_backing_pages;
    uint64_t free_virtual_pages;
    uint64_t page_table_frames;
};
/* Single-CPU kernel-only allocator. count must be 1..VMM_REGION_MAX_PAGES. */
void *vmm_alloc_pages(size_t count);
bool vmm_free_pages(void *address);
bool vmm_region_stats_get(struct vmm_region_stats *stats);
#endif
