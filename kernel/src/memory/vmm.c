#include <stdint.h>
#include "vmm.h"
static uint64_t offset;
void vmm_init(uint64_t direct_map_offset) { offset = direct_map_offset; }
void *vmm_direct_map(uint64_t physical) {
    if (!offset || physical > UINT64_MAX - offset) return (void *)0;
    return (void *)(uintptr_t)(offset + physical);
}
uint64_t vmm_direct_offset(void) { return offset; }
