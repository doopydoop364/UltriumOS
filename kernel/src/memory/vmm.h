#ifndef ULTRIUM_VMM_H
#define ULTRIUM_VMM_H
#include <stdint.h>
#include <stdbool.h>
void vmm_init(uint64_t direct_map_offset);
void *vmm_direct_map(uint64_t physical);
uint64_t vmm_direct_offset(void);
bool vmm_translate(uint64_t virtual_address, uint64_t *physical_address);
bool vmm_self_test(void);
#endif
