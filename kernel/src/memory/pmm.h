#ifndef ULTRIUM_PMM_H
#define ULTRIUM_PMM_H
#include <stdint.h>
#include <stdbool.h>
struct limine_memmap_response;
/* Owning handle for one physical-frame allocation lifetime. Pass an empty
   handle to pmm_page_alloc. Treat returned handles as move-only: there must
   be one releasing owner. A physical address alone is only a borrowed mapping
   address and cannot be returned to the PMM. */
struct pmm_page_handle {
    uint64_t physical;
    uint32_t generation;
};
bool pmm_init(struct limine_memmap_response *map, uint64_t hhdm_offset);
bool pmm_page_alloc(struct pmm_page_handle *out);
bool pmm_page_release(struct pmm_page_handle *owner);
bool pmm_page_handle_valid(const struct pmm_page_handle *owner);
bool pmm_page_handle_is_empty(const struct pmm_page_handle *owner);
/* Deterministic failure injection used by the in-kernel test suite. */
void pmm_test_fail_alloc_after(int32_t successful_allocations);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_used_pages(void);
bool pmm_self_test(void);
#endif
