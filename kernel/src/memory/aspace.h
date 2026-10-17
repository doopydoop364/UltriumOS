#ifndef ULTRIUM_ASPACE_H
#define ULTRIUM_ASPACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-process user address spaces (docs/process-model.md, "Address spaces").
   A user address space is a private PML4 whose lower half (slots 0-255) holds user
   mappings and whose upper half is a copy of the kernel's top-level entries. Every
   frame it uses (PML4, page tables, data pages, the tracking frames themselves) is
   owned through PMM handles held by the address space and released only by
   aspace_destroy. Single CPU, not reentrant. */

#define ASPACE_USER_MIN 0x0000000000400000ULL        /* nothing below 4 MiB is ever mapped */
#define ASPACE_USER_MAX 0x00007fffffff0000ULL        /* exclusive */
#define ASPACE_MAX_FRAMES 1000U                      /* data pages + page tables per address space */
#define ASPACE_MAP_MAX_PAGES 256U                    /* per aspace_map call */

/* Page permissions use enum vmm_page_permissions (READ/WRITE/EXECUTE). A user page is
   readable and either writable or executable, never both (W^X). */
struct aspace;

/* Capture the kernel's top-level table and enable SMEP/SMAP where the CPU has them. */
bool aspace_system_init(void);
bool aspace_system_ready(void);
bool aspace_smep_enabled(void);
bool aspace_smap_enabled(void);

struct aspace *aspace_create(void);
/* Frees every frame the address space owns. If it is the active address space the
   kernel root is loaded first. */
void aspace_destroy(struct aspace *space);

/* Map `pages` fresh, zeroed frames at the page-aligned user address `va`. All or
   nothing: on any failure nothing is mapped and no frame stays allocated. */
bool aspace_map(struct aspace *space, uint64_t va, size_t pages, uint32_t permissions);
bool aspace_unmap(struct aspace *space, uint64_t va, size_t pages);
/* Change the permissions of already-mapped pages (RO, RW or RX only; never W+X). */
bool aspace_protect(struct aspace *space, uint64_t va, size_t pages, uint32_t permissions);
/* Software walk: physical address of a user address, requiring the User bit and
   (optionally) write permission. Never touches user memory. */
bool aspace_translate(const struct aspace *space, uint64_t va, bool write, uint64_t *physical);
bool aspace_is_executable(const struct aspace *space, uint64_t va);

/* Validated copies. The whole range is checked before anything is copied; a range
   that is not entirely mapped with the right permissions copies nothing and fails. */
bool aspace_copy_to_user(struct aspace *space, uint64_t dst, const void *src, size_t length);
/* Loader-only: copy into mapped user pages whatever their W permission (used to fill code segments). */
/* Is [va, va+length) entirely mapped for user access (and writable when `write`)? Touches nothing. */
bool aspace_range_ok(const struct aspace *space, uint64_t va, size_t length, bool write);
bool aspace_poke(struct aspace *space, uint64_t dst, const void *src, size_t length);
bool aspace_copy_from_user(const struct aspace *space, void *dst, uint64_t src, size_t length);
/* Copies a NUL-terminated string of at most max-1 characters; returns its length or -1. */
long aspace_copy_string_from_user(const struct aspace *space, char *dst, uint64_t src, size_t max);

void aspace_activate(struct aspace *space);          /* load its root into CR3 */
void aspace_activate_kernel(void);
uint64_t aspace_root_physical(const struct aspace *space);
uint64_t aspace_kernel_root_physical(void);
struct aspace *aspace_active(void);
uint32_t aspace_frames_used(const struct aspace *space);
uint32_t aspace_data_pages(const struct aspace *space);
/* The upper half of this address space's PML4 equals the kernel's current top level and
   carries no User bit. */
bool aspace_kernel_half_intact(const struct aspace *space);
bool aspace_user_bounds_ok(uint64_t va, size_t length);
/* Test hook: pretend the kernel's top-level slot `index` (0-255 = slots 256-511) appeared or vanished since start-up. */
void aspace_test_flip_kernel_slot(unsigned index);
#endif
