#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>
#include "pmm.h"

#define PAGE_SIZE 4096ULL
#define MAX_RANGES 128
#define ALLOCATED_BIT 1U
#define MAX_ALLOCATED_GENERATION 0xfffffffdU
struct range {
    uint64_t begin, end, alloc_begin;
    uint32_t *state;
};
/* Per-frame records: 0 is never issued, odd values are live generations,
   nonzero even values are released generations, and UINT32_MAX is retired. */
static struct range ranges[MAX_RANGES];
static size_t range_count;
static uint64_t hhdm;
static uint64_t all_pages, available;
static uint64_t cursor_range, cursor_page;
static uint64_t released_head;
static bool ready;
static int32_t fail_after = -1;
static uint32_t successful_since_injection;

void pmm_test_fail_alloc_after(int32_t successful_allocations) {
    fail_after = successful_allocations;
    successful_since_injection = 0;
}

static uint32_t *state_for(uint64_t physical) {
    if (!physical || (physical & (PAGE_SIZE - 1))) return (uint32_t *)0;
    if (!ready && !range_count) return (uint32_t *)0;
    for (size_t i = 0; i < range_count; ++i) {
        if (physical >= ranges[i].alloc_begin && physical < ranges[i].end) {
            uint64_t slot = (physical - ranges[i].begin) / PAGE_SIZE;
            return &ranges[i].state[slot];
        }
    }
    return (uint32_t *)0;
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
        uint64_t begin = e->base < PAGE_SIZE ? PAGE_SIZE : e->base;
        if (begin > UINT64_MAX - (PAGE_SIZE - 1)) return false;
        begin = (begin + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t end = (e->base + e->length) & ~(PAGE_SIZE - 1);
        if (end <= begin) continue;
        if (range_count >= MAX_RANGES) return false;
        for (size_t r = 0; r < range_count; ++r)
            if (begin < ranges[r].end && ranges[r].begin < end) return false;
        uint64_t pages = (end - begin) / PAGE_SIZE;
        if (all_pages > UINT64_MAX - pages) return false;
        if (pages > UINT64_MAX / sizeof(uint32_t)) return false;
        uint64_t metadata_bytes = pages * sizeof(uint32_t);
        if (metadata_bytes > UINT64_MAX - (PAGE_SIZE - 1)) return false;
        uint64_t metadata_pages = (metadata_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        if (metadata_pages > pages || hhdm_offset > UINT64_MAX - (end - 1)) return false;
        uint64_t alloc_begin = begin + metadata_pages * PAGE_SIZE;
        uint32_t *state = (uint32_t *)(uintptr_t)(hhdm_offset + begin);
        if (metadata_bytes) {
            for (uint64_t byte = 0; byte < metadata_bytes; ++byte)
                ((volatile uint8_t *)state)[byte] = 0;
        }
        ranges[range_count++] = (struct range){
            .begin = begin, .end = end, .alloc_begin = alloc_begin, .state = state
        };
        all_pages += (end - alloc_begin) / PAGE_SIZE;
    }
    if (!range_count || !all_pages) return false;
    available = all_pages;
    while (cursor_range < range_count && ranges[cursor_range].alloc_begin == ranges[cursor_range].end)
        ++cursor_range;
    if (cursor_range == range_count) return false;
    cursor_page = ranges[cursor_range].alloc_begin;
    ready = true;
    return true;
}

bool pmm_page_alloc(struct pmm_page_handle *out) {
    if (!ready || !out || !available || out->physical || out->generation) return false;
    if (fail_after >= 0 && successful_since_injection >= (uint32_t)fail_after) {
        fail_after = -1;
        return false;
    }
    uint64_t physical;
    bool from_released = released_head != 0;
    if (from_released) {
        physical = released_head;
        uint32_t *state = state_for(physical);
        if (!state || (*state & ALLOCATED_BIT) || !*state) return false;
        if (*state > MAX_ALLOCATED_GENERATION) return false;
        if (hhdm > UINT64_MAX - physical) return false;
        uint64_t next = *(volatile uint64_t *)(uintptr_t)(hhdm + physical);
        if (next) {
            uint32_t *next_state = state_for(next);
            if (!next_state || !*next_state || (*next_state & ALLOCATED_BIT)) return false;
        }
        released_head = next;
    } else {
        while (cursor_range < range_count && cursor_page >= ranges[cursor_range].end) {
            ++cursor_range;
            if (cursor_range < range_count) cursor_page = ranges[cursor_range].alloc_begin;
        }
        if (cursor_range >= range_count) return false;
        physical = cursor_page;
        cursor_page += PAGE_SIZE;
    }
    uint32_t *state = state_for(physical);
    if (!state || (*state & ALLOCATED_BIT) || *state > MAX_ALLOCATED_GENERATION)
        return false;
    if (!from_released && *state != 0) return false;
    uint32_t generation = *state + 1;
    *state = generation;
    --available;
    if (fail_after >= 0) ++successful_since_injection;
    *out = (struct pmm_page_handle){.physical = physical, .generation = generation};
    return true;
}

bool pmm_page_handle_valid(const struct pmm_page_handle *owner) {
    if (!ready || !owner || !owner->physical || !owner->generation ||
        !(owner->generation & ALLOCATED_BIT) || owner->generation == UINT32_MAX)
        return false;
    uint32_t *state = state_for(owner->physical);
    return state && *state == owner->generation;
}

bool pmm_page_handle_is_empty(const struct pmm_page_handle *owner) {
    return owner && !owner->physical && !owner->generation;
}

bool pmm_page_release(struct pmm_page_handle *owner) {
    if (!pmm_page_handle_valid(owner) || available >= all_pages) return false;
    uint32_t *state = state_for(owner->physical);
    if (!state) return false;
    /* Retire a frame rather than wrapping a generation and making an ancient
       stale handle valid again. Retired frames leave the available count. */
    if (owner->generation == MAX_ALLOCATED_GENERATION) {
        *state = UINT32_MAX;
        --available;
        *owner = (struct pmm_page_handle){0};
        return true;
    }
    if (hhdm > UINT64_MAX - owner->physical) return false;
    *(volatile uint64_t *)(uintptr_t)(hhdm + owner->physical) = released_head;
    *state = owner->generation + 1;
    released_head = owner->physical;
    ++available;
    *owner = (struct pmm_page_handle){0};
    return true;
}

uint64_t pmm_total_bytes(void) { return all_pages * PAGE_SIZE; }
uint64_t pmm_free_pages(void) { return available; }
uint64_t pmm_used_pages(void) { return all_pages - available; }

bool pmm_self_test(void) {
    uint64_t before = available;
    struct pmm_page_handle a = {0}, b = {0}, c = {0}, stale = {0};
    bool ok = pmm_page_alloc(&a) && pmm_page_alloc(&b) && a.physical != b.physical;
    if (ok) {
        stale = a;
        ok = pmm_page_release(&a) && pmm_page_handle_is_empty(&a) &&
             !pmm_page_release(&stale) && pmm_page_alloc(&c) &&
             c.physical == stale.physical && c.generation != stale.generation &&
             !pmm_page_release(&stale);
    }
    if (!pmm_page_handle_is_empty(&a) && !pmm_page_release(&a)) ok = false;
    if (!pmm_page_handle_is_empty(&b) && !pmm_page_release(&b)) ok = false;
    if (!pmm_page_handle_is_empty(&c) && !pmm_page_release(&c)) ok = false;
    return ok && available == before;
}
