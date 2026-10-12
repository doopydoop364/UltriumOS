#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "heap.h"
#include "pmm.h"
#include "vmm.h"

/* Bootstrap kernel heap: one 4 KiB physical page per allocation.
   Metadata lives in a small static table, not on allocated pages. */
#define HEAP_SLOTS 256
#define HEAP_PAGE_SIZE 4096
static struct { void *address; uint64_t physical; } slots[HEAP_SLOTS];
static uint64_t live;
static bool initialized;

bool heap_init(void) {
    for (size_t i = 0; i < HEAP_SLOTS; ++i) {
        slots[i].address = (void *)0;
        slots[i].physical = 0;
    }
    live = 0;
    initialized = vmm_direct_offset() != 0;
    return initialized;
}
void *kmalloc(size_t bytes) {
    if (!initialized || bytes == 0 || bytes > HEAP_PAGE_SIZE) return (void *)0;
    size_t slot = 0;
    while (slot < HEAP_SLOTS && slots[slot].address) ++slot;
    if (slot == HEAP_SLOTS) return (void *)0;
    uint64_t page = pmm_alloc_page();
    if (!page) return (void *)0;
    void *addr = vmm_direct_map(page);
    if (!addr) {
        pmm_free_page(page);
        return (void *)0;
    }
    /* Zero memory so new kernel consumers don't read stale contents. */
    uint8_t *out = (uint8_t *)addr;
    for (size_t i = 0; i < HEAP_PAGE_SIZE; ++i) out[i] = 0;
    slots[slot].address = addr;
    slots[slot].physical = page;
    ++live;
    return addr;
}
void kfree(void *pointer) {
    if (!pointer || !initialized) return;
    for (size_t i = 0; i < HEAP_SLOTS; ++i) {
        if (slots[i].address == pointer) {
            if (pmm_free_page(slots[i].physical)) {
                slots[i].address = (void *)0;
                slots[i].physical = 0;
                --live;
            }
            return;
        }
    }
}
uint64_t heap_live_allocations(void) { return live; }
bool heap_self_test(void) {
    uint64_t before_pages = pmm_free_pages();
    void *a = kmalloc(64);
    void *b = kmalloc(4096);
    if (!a || !b || a == b) {
        kfree(a); kfree(b);
        return false;
    }
    uint8_t *p = (uint8_t *)a;
    for (size_t i = 0; i < 64; ++i) if (p[i] != 0) return false;
    p[0] = 0x5a;
    if (((uint8_t *)b)[0] != 0 || p[0] != 0x5a) return false;
    kfree(a); kfree(b);
    return live == 0 && pmm_free_pages() == before_pages &&
           kmalloc(4097) == (void *)0;
}
