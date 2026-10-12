#ifndef ULTRIUM_VMM_H
#define ULTRIUM_VMM_H
#include <stdint.h>
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
#endif
