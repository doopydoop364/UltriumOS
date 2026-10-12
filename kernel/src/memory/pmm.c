#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "pmm.h"

#define PAGE_SIZE 4096ULL
#define MAX_RANGES 128
struct range { uint64_t begin, end; };
static struct range ranges[MAX_RANGES];
static size_t range_count;
static uint64_t hhdm;
static uint64_t all_pages, available;
static uint64_t cursor_range, cursor_page;
static uint64_t released_head;
static bool ready;

/* Physical addresses in the free stack are stored inside freed pages,
   through Limine's higher-half direct map. No heap needed. */
static bool valid_page(uint64_t address) {
    if (address == 0 || (address & (PAGE_SIZE - 1))) return false;
    for (size_t i = 0; i < range_count; ++i)
        if (address >= ranges[i].begin && address < ranges[i].end) return true;
    return false;
}
bool pmm_init(struct limine_memmap_response *map, uint64_t hhdm_offset) {
    if (!map || !map->entries || !hhdm_offset) return false;
    range_count = 0; all_pages = 0; available = 0;
    cursor_range = 0; cursor_page = 0; released_head = 0; ready = false;
    hhdm = hhdm_offset;
    for (uint64_t i = 0; i < map->entry_count; ++i) {
        struct limine_memmap_entry *e = map->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE || !e->length) continue;
        if (e->base > UINT64_MAX - e->length) return false;
        uint64_t start = e->base;
        if (start < PAGE_SIZE) start = PAGE_SIZE;
        if (start > UINT64_MAX - (PAGE_SIZE - 1)) return false;
        start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = (e->base + e->length) & ~(PAGE_SIZE - 1);
        if (end <= start) continue;
        if (range_count >= MAX_RANGES) return false;
        ranges[range_count].begin = start;
        ranges[range_count].end = end;
        ++range_count;
        all_pages += (end - start) / PAGE_SIZE;
    }
    if (!range_count || !all_pages) return false;
    available = all_pages;
    cursor_page = ranges[0].begin;
    ready = true;
    return true;
}
uint64_t pmm_alloc_page(void) {
    if (!ready || !available) return 0;
    uint64_t page;
    if (released_head) {
        page = released_head;
        released_head = *(uint64_t *)(uintptr_t)(hhdm + page);
    } else {
        while (cursor_range < range_count && cursor_page >= ranges[cursor_range].end) {
            ++cursor_range;
            if (cursor_range < range_count) cursor_page = ranges[cursor_range].begin;
        }
        if (cursor_range >= range_count) return 0;
        page = cursor_page;
        cursor_page += PAGE_SIZE;
    }
    --available;
    return page;
}
bool pmm_free_page(uint64_t physical) {
    if (!ready || !valid_page(physical)) return false;
    /* A page that has never been allocated cannot be freed. */
    bool issued = false;
    for (size_t i = 0; i < range_count; ++i) {
        if (physical >= ranges[i].begin && physical < ranges[i].end) {
            issued = i < cursor_range || (i == cursor_range && physical < cursor_page);
            break;
        }
    }
    if (!issued) return false;
    /* Prototype safety guard: detect double frees in the released-page stack. */
    for (uint64_t p = released_head; p; p = *(uint64_t *)(uintptr_t)(hhdm + p))
        if (p == physical) return false;
    *(uint64_t *)(uintptr_t)(hhdm + physical) = released_head;
    released_head = physical;
    ++available;
    return true;
}
uint64_t pmm_total_bytes(void) { return all_pages * PAGE_SIZE; }
uint64_t pmm_free_pages(void) { return available; }
uint64_t pmm_used_pages(void) { return all_pages - available; }
bool pmm_self_test(void) {
    uint64_t before = available;
    uint64_t a = pmm_alloc_page(), b = pmm_alloc_page();
    if (!a || !b || a == b) return false;
    if (!pmm_free_page(a) || pmm_free_page(a)) return false;
    uint64_t c = pmm_alloc_page();
    bool ok = c == a && pmm_free_page(b) && pmm_free_page(c) && available == before;
    return ok;
}
