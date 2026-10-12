#ifndef ULTRIUM_PMM_H
#define ULTRIUM_PMM_H
#include <stdint.h>
#include <stdbool.h>
struct limine_memmap_response;
bool pmm_init(struct limine_memmap_response *map, uint64_t hhdm_offset);
uint64_t pmm_alloc_page(void); /* physical address, 0 on failure */
bool pmm_free_page(uint64_t physical);
uint64_t pmm_total_bytes(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_used_pages(void);
bool pmm_self_test(void);
#endif
