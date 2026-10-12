#ifndef ULTRIUM_VMM_H
#define ULTRIUM_VMM_H
#include <stdint.h>
#include <stdbool.h>
void vmm_init(uint64_t direct_map_offset);
void *vmm_direct_map(uint64_t physical);
uint64_t vmm_direct_offset(void);
#endif
