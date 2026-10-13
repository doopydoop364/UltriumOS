#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "heap.h"
#include "pmm.h"
#include "vmm.h"

/* One physical page per slab, with several small allocations per page.
   Static metadata avoids recursion before a general-purpose kernel heap exists. */
#define PAGE_BYTES 4096U
#define MAX_SLABS 512U
#define BITMAP_WORDS 4U
static const uint16_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
#define CLASS_COUNT (sizeof(sizes) / sizeof(sizes[0]))
struct slab {
    struct pmm_page_handle owner;
    uint8_t *base;
    uint64_t used[BITMAP_WORDS];
    uint16_t block_size;
    uint16_t blocks;
    uint16_t active;
};
static struct slab slabs[MAX_SLABS];
static uint64_t live;
static bool initialized;

bool heap_init(void) {
    if (initialized && live) return false;
    for (size_t i = 0; i < MAX_SLABS; ++i) {
        slabs[i] = (struct slab){0};
    }
    live = 0;
    initialized = vmm_direct_offset() != 0;
    return initialized;
}
static void *use_slot(struct slab *slab) {
    for (uint16_t i = 0; i < slab->blocks; ++i) {
        uint64_t mask = 1ULL << (i & 63);
        uint64_t *word = &slab->used[i / 64];
        if (!(*word & mask)) {
            *word |= mask;
            ++slab->active;
            ++live;
            uint8_t *ptr = slab->base + (size_t)i * slab->block_size;
            for (size_t k = 0; k < slab->block_size; ++k) ptr[k] = 0;
            return ptr;
        }
    }
    return (void *)0;
}
void *kmalloc(size_t bytes) {
    if (!initialized || !bytes || bytes > PAGE_BYTES) return (void *)0;
    uint16_t size = 0;
    for (size_t i = 0; i < CLASS_COUNT; ++i) {
        if (bytes <= sizes[i]) { size = sizes[i]; break; }
    }
    for (size_t i = 0; i < MAX_SLABS; ++i)
        if (slabs[i].base && slabs[i].block_size == size &&
            slabs[i].active < slabs[i].blocks &&
            pmm_page_handle_valid(&slabs[i].owner))
            return use_slot(&slabs[i]);
    for (size_t i = 0; i < MAX_SLABS; ++i) {
        if (slabs[i].base) continue;
        struct pmm_page_handle owner = {0};
        if (!pmm_page_alloc(&owner)) return (void *)0;
        uint8_t *base = (uint8_t *)vmm_direct_map(owner.physical);
        if (!base) { pmm_page_release(&owner); return (void *)0; }
        slabs[i] = (struct slab){
            .owner = owner, .base = base, .block_size = size,
            .blocks = PAGE_BYTES / size, .active = 0
        };
        owner = (struct pmm_page_handle){0};
        return use_slot(&slabs[i]);
    }
    return (void *)0;
}
void kfree(void *pointer) {
    if (!initialized || !pointer) return;
    uintptr_t target = (uintptr_t)pointer;
    for (size_t i = 0; i < MAX_SLABS; ++i) {
        struct slab *s = &slabs[i];
        if (!s->base) continue;
        uintptr_t begin = (uintptr_t)s->base;
        if (target < begin || target - begin >= PAGE_BYTES) continue;
        if (!pmm_page_handle_valid(&s->owner)) return;
        size_t offset = target - begin;
        if (offset % s->block_size) return;
        size_t index = offset / s->block_size;
        uint64_t mask = 1ULL << (index & 63);
        uint64_t *word = &s->used[index / 64];
        if (!(*word & mask)) return;
        *word &= ~mask;
        --s->active;
        --live;
        if (!s->active) {
            /* Keep the slab descriptor if releasing its owned frame fails. */
            if (pmm_page_release(&s->owner)) *s = (struct slab){0};
        }
        return;
    }
}
uint64_t heap_live_allocations(void) { return live; }
bool heap_self_test(void) {
    uint64_t pages_before = pmm_free_pages();
    uint64_t live_before = live;
    void *a = kmalloc(24), *b = kmalloc(24), *c = kmalloc(4096);
    if (!a || !b || !c || a == b || a == c || b == c) goto fail;
    if (((uint8_t *)a)[0] || ((uint8_t *)b)[0] || ((uint8_t *)c)[0]) goto fail;
    ((uint8_t *)a)[0] = 0x5a;
    ((uint8_t *)b)[0] = 0xa5;
    if (((uint8_t *)a)[0] != 0x5a || ((uint8_t *)b)[0] != 0xa5) goto fail;
    kfree(a); a = (void *)0;
    kfree(b); b = (void *)0;
    kfree(c); c = (void *)0;
    return pmm_free_pages() == pages_before && live == live_before &&
           kmalloc(4097) == (void *)0;
fail:
    kfree(a); kfree(b); kfree(c);
    return false;
}
