#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "memory/aspace.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/heap.h"

#define PTE_P 1ULL
#define PTE_W (1ULL << 1)
#define PTE_U (1ULL << 2)
#define PTE_PS (1ULL << 7)
#define PTE_NX (1ULL << 63)
#define PTE_ADDR 0x000ffffffffff000ULL
#define PAGE 4096ULL
#define TRACKERS 4U
#define HANDLES_PER_TRACKER (4096U / sizeof(struct pmm_page_handle))
#define MAGIC 0x41535041U

struct aspace {
    uint32_t magic;
    struct pmm_page_handle root;
    struct pmm_page_handle trackers[TRACKERS];     /* frames holding the handle list */
    uint32_t frames;                               /* handles stored (page tables + data pages) */
    uint32_t data_pages;
};

static uint64_t kernel_root;
static bool kernel_slot_present[256];                 /* which kernel top-level slots (256-511) existed when processes became possible */
static bool ready, smep, smap;
static struct aspace *active;

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }

bool aspace_system_init(void) {
    if (ready || !vmm_direct_offset()) return false;
    kernel_root = read_cr3() & PTE_ADDR;
    {
        volatile uint64_t *kroot = (volatile uint64_t *)vmm_direct_map(kernel_root);
        if (!kroot) return false;
        for (unsigned i = 0; i < 256; ++i) kernel_slot_present[i] = (kroot[256 + i] & PTE_P) != 0;
    }
    uint32_t eax = 7, ebx = 0, ecx = 0, edx = 0;
    uint32_t max_leaf;
    __asm__ volatile("cpuid" : "=a"(max_leaf) : "a"(0U) : "ebx", "ecx", "edx");
    if (max_leaf >= 7) {
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        uint64_t cr4;
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        if (ebx & (1U << 7)) { cr4 |= 1ULL << 20; smep = true; }
        if (ebx & (1U << 20)) { cr4 |= 1ULL << 21; smap = true; }
        __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
        __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
        smep = smep && (cr4 & (1ULL << 20));
        smap = smap && (cr4 & (1ULL << 21));
    }
    ready = true;
    return true;
}
bool aspace_system_ready(void) { return ready; }
bool aspace_smep_enabled(void) { return smep; }
bool aspace_smap_enabled(void) { return smap; }
uint64_t aspace_kernel_root_physical(void) { return kernel_root; }
struct aspace *aspace_active(void) { return active; }

static volatile uint64_t *table_at(uint64_t physical) { return (volatile uint64_t *)vmm_direct_map(physical); }
static void zero_frame(uint64_t physical) {
    volatile uint64_t *t = table_at(physical);
    for (unsigned i = 0; i < 512; ++i) t[i] = 0;
}
static struct pmm_page_handle *handle_slot(struct aspace *s, uint32_t index) {
    struct pmm_page_handle *list = (struct pmm_page_handle *)table_at(s->trackers[index / HANDLES_PER_TRACKER].physical);
    return &list[index % HANDLES_PER_TRACKER];
}

struct aspace *aspace_create(void) {
    if (!ready) return NULL;
    struct aspace *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    if (!pmm_page_alloc(&s->root)) { kfree(s); return NULL; }
    if (!pmm_page_alloc(&s->trackers[0])) { pmm_page_release(&s->root); kfree(s); return NULL; }
    zero_frame(s->root.physical);
    zero_frame(s->trackers[0].physical);
    volatile uint64_t *root = table_at(s->root.physical), *kroot = table_at(kernel_root);
    /* A top-level slot the kernel gained after start-up would be missing from every existing address space. */
    for (unsigned i = 0; i < 256; ++i)
        if (((kroot[256 + i] & PTE_P) != 0) != kernel_slot_present[i]) { pmm_page_release(&s->trackers[0]); pmm_page_release(&s->root); kfree(s); return NULL; }
    /* Shared kernel half. The bootloader's own top-level entries can carry the User bit (the direct map does):
       clearing it in this private copy makes everything beneath unreachable from ring 3, because x86
       user access needs the User bit at every level. */
    for (unsigned i = 256; i < 512; ++i) root[i] = kroot[i] & ~PTE_U;
    s->magic = MAGIC;
    return s;
}

/* Returns the address-space frame list slot for a new frame, allocating a tracker frame when needed. */
static bool reserve_slot(struct aspace *s, struct pmm_page_handle **slot) {
    if (s->frames >= ASPACE_MAX_FRAMES) return false;
    unsigned tracker = s->frames / HANDLES_PER_TRACKER;
    if (tracker >= TRACKERS) return false;
    if (!s->trackers[tracker].physical) {
        if (!pmm_page_alloc(&s->trackers[tracker])) return false;
        zero_frame(s->trackers[tracker].physical);
    }
    *slot = handle_slot(s, s->frames);
    return true;
}

void aspace_destroy(struct aspace *s) {
    if (!s || s->magic != MAGIC) return;
    if (active == s || (read_cr3() & PTE_ADDR) == s->root.physical) aspace_activate_kernel();
    for (uint32_t i = 0; i < s->frames; ++i) {
        struct pmm_page_handle *h = handle_slot(s, i);
        (void)pmm_page_release(h);
    }
    for (unsigned i = 0; i < TRACKERS; ++i) if (s->trackers[i].physical) (void)pmm_page_release(&s->trackers[i]);
    (void)pmm_page_release(&s->root);
    s->magic = 0;
    kfree(s);
}

void aspace_test_flip_kernel_slot(unsigned index) { if (index < 256) kernel_slot_present[index] = !kernel_slot_present[index]; }

bool aspace_user_bounds_ok(uint64_t va, size_t length) {
    if (va < ASPACE_USER_MIN || va >= ASPACE_USER_MAX) return false;
    if (length > ASPACE_USER_MAX - va) return false;                 /* also rejects overflow */
    return true;
}

/* The leaf entry for `va`, or NULL when a table on the path is missing. */
static volatile uint64_t *leaf_entry(const struct aspace *s, uint64_t va) {
    uint64_t table = s->root.physical;
    for (int level = 3; level >= 1; --level) {
        volatile uint64_t *t = table_at(table);
        uint64_t e = t[(va >> (12 + 9 * level)) & 511];
        if (!(e & PTE_P) || (e & PTE_PS)) return NULL;
        table = e & PTE_ADDR;
    }
    return &table_at(table)[(va >> 12) & 511];
}

bool aspace_translate(const struct aspace *s, uint64_t va, bool write, uint64_t *physical) {
    if (!s || s->magic != MAGIC || !aspace_user_bounds_ok(va, 1)) return false;
    volatile uint64_t *leaf = leaf_entry(s, va);
    if (!leaf) return false;
    uint64_t e = *leaf;
    if (!(e & PTE_P) || !(e & PTE_U) || (write && !(e & PTE_W))) return false;
    if (physical) *physical = (e & PTE_ADDR) | (va & (PAGE - 1));
    return true;
}
bool aspace_is_executable(const struct aspace *s, uint64_t va) {
    if (!s || s->magic != MAGIC || !aspace_user_bounds_ok(va, 1)) return false;
    volatile uint64_t *leaf = leaf_entry(s, va);
    return leaf && (*leaf & PTE_P) && (*leaf & PTE_U) && !(*leaf & PTE_NX);
}

static bool new_frame(struct aspace *s, uint64_t *physical) {
    struct pmm_page_handle *slot;
    if (!reserve_slot(s, &slot)) return false;
    struct pmm_page_handle h = {0};
    if (!pmm_page_alloc(&h)) return false;
    zero_frame(h.physical);
    *slot = h;
    ++s->frames;
    *physical = slot->physical;
    return true;
}

#define MAX_CREATED_TABLES 16U
struct map_undo {
    volatile uint64_t *created[MAX_CREATED_TABLES];
    unsigned created_count;
    uint32_t start_frames;
    size_t mapped_pages;
};

/* Next-level table for `index`, creating it from a fresh frame when absent. */
static bool descend(struct aspace *s, uint64_t table, unsigned index, uint64_t *next, struct map_undo *undo) {
    volatile uint64_t *t = table_at(table);
    uint64_t e = t[index];
    if (e & PTE_P) {
        if (e & PTE_PS) return false;
        *next = e & PTE_ADDR;
        return true;
    }
    if (undo->created_count >= MAX_CREATED_TABLES) return false;
    uint64_t frame;
    if (!new_frame(s, &frame)) return false;
    t[index] = frame | PTE_P | PTE_W | PTE_U;          /* permissions are enforced at the leaf */
    undo->created[undo->created_count++] = &t[index];
    *next = frame;
    return true;
}

static void rollback(struct aspace *s, uint64_t va, struct map_undo *undo) {
    for (size_t i = 0; i < undo->mapped_pages; ++i) {
        volatile uint64_t *leaf = leaf_entry(s, va + i * PAGE);
        if (leaf) *leaf = 0;
    }
    s->data_pages -= (uint32_t)undo->mapped_pages;
    while (undo->created_count) *undo->created[--undo->created_count] = 0;
    while (s->frames > undo->start_frames) {
        struct pmm_page_handle *h = handle_slot(s, --s->frames);
        (void)pmm_page_release(h);
    }
}

static bool permissions_ok(uint32_t p) {
    return p == VMM_PAGE_READ || p == (VMM_PAGE_READ | VMM_PAGE_WRITE) || p == (VMM_PAGE_READ | VMM_PAGE_EXECUTE);
}

bool aspace_map(struct aspace *s, uint64_t va, size_t pages, uint32_t permissions) {
    if (!s || s->magic != MAGIC || !pages || pages > ASPACE_MAP_MAX_PAGES || (va & (PAGE - 1))) return false;
    if (!permissions_ok(permissions) || !aspace_user_bounds_ok(va, pages * PAGE)) return false;
    for (size_t i = 0; i < pages; ++i) {
        volatile uint64_t *leaf = leaf_entry(s, va + i * PAGE);
        if (leaf && (*leaf & PTE_P)) return false;                      /* never replace a mapping */
    }
    struct map_undo undo = {.start_frames = s->frames};
    uint64_t flags = PTE_P | PTE_U;
    if (permissions & VMM_PAGE_WRITE) flags |= PTE_W;
    if (!(permissions & VMM_PAGE_EXECUTE)) flags |= PTE_NX;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t a = va + i * PAGE, t = s->root.physical, data;
        if (!descend(s, t, (a >> 39) & 511, &t, &undo) || !descend(s, t, (a >> 30) & 511, &t, &undo) ||
            !descend(s, t, (a >> 21) & 511, &t, &undo) || !new_frame(s, &data)) { rollback(s, va, &undo); return false; }
        volatile uint64_t *leaf = &table_at(t)[(a >> 12) & 511];
        if (*leaf & PTE_P) { rollback(s, va, &undo); return false; }
        *leaf = data | flags;
        ++undo.mapped_pages;
        ++s->data_pages;
    }
    if (active == s) write_cr3(s->root.physical);                         /* flush: no stale not-present entries */
    return true;
}

bool aspace_unmap(struct aspace *s, uint64_t va, size_t pages) {
    if (!s || s->magic != MAGIC || !pages || pages > ASPACE_MAP_MAX_PAGES || (va & (PAGE - 1)) || !aspace_user_bounds_ok(va, pages * PAGE)) return false;
    for (size_t i = 0; i < pages; ++i) {
        volatile uint64_t *leaf = leaf_entry(s, va + i * PAGE);
        if (!leaf || !(*leaf & PTE_P) || !(*leaf & PTE_U)) return false;     /* every page must be mapped */
    }
    for (size_t i = 0; i < pages; ++i) {
        volatile uint64_t *leaf = leaf_entry(s, va + i * PAGE);
        uint64_t physical = *leaf & PTE_ADDR;
        *leaf = 0;
        for (uint32_t k = 0; k < s->frames; ++k) {
            struct pmm_page_handle *h = handle_slot(s, k);
            if (h->physical != physical) continue;
            (void)pmm_page_release(h);
            *h = *handle_slot(s, s->frames - 1);                             /* keep the list dense */
            *handle_slot(s, s->frames - 1) = (struct pmm_page_handle){0};
            --s->frames;
            break;
        }
        --s->data_pages;
    }
    if (active == s) write_cr3(s->root.physical);
    return true;
}

static bool range_ok(const struct aspace *s, uint64_t va, size_t length, bool write) {
    if (!s || s->magic != MAGIC || !aspace_user_bounds_ok(va, length)) return false;
    if (!length) return true;
    for (uint64_t a = va & ~(PAGE - 1); a < va + length; a += PAGE)
        if (!aspace_translate(s, a, write, NULL)) return false;
    return true;
}
bool aspace_copy_to_user(struct aspace *s, uint64_t dst, const void *src, size_t length) {
    if ((length && !src) || !range_ok(s, dst, length, true)) return false;
    const uint8_t *from = src;
    for (size_t done = 0; done < length;) {
        uint64_t physical;
        if (!aspace_translate(s, dst + done, true, &physical)) return false;
        size_t chunk = PAGE - ((dst + done) & (PAGE - 1));
        if (chunk > length - done) chunk = length - done;
        volatile uint8_t *to = (volatile uint8_t *)vmm_direct_map(physical);
        for (size_t i = 0; i < chunk; ++i) to[i] = from[done + i];
        done += chunk;
    }
    return true;
}
bool aspace_copy_from_user(const struct aspace *s, void *dst, uint64_t src, size_t length) {
    if ((length && !dst) || !range_ok(s, src, length, false)) return false;
    uint8_t *to = dst;
    for (size_t done = 0; done < length;) {
        uint64_t physical;
        if (!aspace_translate(s, src + done, false, &physical)) return false;
        size_t chunk = PAGE - ((src + done) & (PAGE - 1));
        if (chunk > length - done) chunk = length - done;
        const volatile uint8_t *from = (const volatile uint8_t *)vmm_direct_map(physical);
        for (size_t i = 0; i < chunk; ++i) to[done + i] = from[i];
        done += chunk;
    }
    return true;
}
long aspace_copy_string_from_user(const struct aspace *s, char *dst, uint64_t src, size_t max) {
    if (!s || !dst || !max) return -1;
    for (size_t i = 0; i < max; ++i) {
        uint64_t physical;
        if (!aspace_user_bounds_ok(src + i, 1) || !aspace_translate(s, src + i, false, &physical)) return -1;
        char c = (char)*(const volatile uint8_t *)vmm_direct_map(physical);
        dst[i] = c;
        if (!c) return (long)i;
    }
    dst[max - 1] = 0;
    return -1;                                                            /* not terminated within the limit */
}

void aspace_activate(struct aspace *s) {
    if (!s || s->magic != MAGIC) return;
    active = s;
    write_cr3(s->root.physical);
}
void aspace_activate_kernel(void) {
    active = NULL;
    write_cr3(kernel_root);
}
uint64_t aspace_root_physical(const struct aspace *s) { return s && s->magic == MAGIC ? s->root.physical : 0; }
uint32_t aspace_frames_used(const struct aspace *s) {
    if (!s) return 0;
    uint32_t n = s->frames + 1;
    for (unsigned i = 0; i < TRACKERS; ++i) if (s->trackers[i].physical) ++n;
    return n;
}
uint32_t aspace_data_pages(const struct aspace *s) { return s ? s->data_pages : 0; }
bool aspace_kernel_half_intact(const struct aspace *s) {
    if (!s || s->magic != MAGIC) return false;
    volatile uint64_t *root = table_at(s->root.physical), *kroot = table_at(kernel_root);
    for (unsigned i = 256; i < 512; ++i) {
        if ((root[i] & ~0x64ULL) != (kroot[i] & ~0x64ULL)) return false;      /* ignoring CPU-maintained Accessed/Dirty and the deliberately cleared User bit */
        if ((root[i] & PTE_P) && (root[i] & PTE_U)) return false;
    }
    for (unsigned i = 0; i < 256; ++i) if ((root[i] & PTE_P) && !(root[i] & PTE_U)) return false;
    return true;
}
