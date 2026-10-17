#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "memory/aspace.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "memory/heap.h"
#include "memory.h"

/* Per-process address-space tests (docs/process-model.md). They exercise the validated, software-walk
   access paths and page-table switching; no user code runs yet. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define BASE 0x0000000000400000ULL
#define RO VMM_PAGE_READ
#define RW (VMM_PAGE_READ | VMM_PAGE_WRITE)
#define RX (VMM_PAGE_READ | VMM_PAGE_EXECUTE)

static const char *lifecycle(void) {
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    REQUIRE(aspace_system_ready(), "address-space support not initialised");
    {   /* Every protection the CPU offers must be on (QEMU's default CPU model offers neither; ULTRIUM_QEMU_CPU=max does). */
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0, max_leaf;
        __asm__ volatile("cpuid" : "=a"(max_leaf) : "a"(0U) : "ebx", "ecx", "edx");
        if (max_leaf >= 7) {
            eax = 7; ecx = 0;
            __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
            REQUIRE(!(ebx & (1U << 7)) || aspace_smep_enabled(), "the CPU has SMEP but it is not enabled");
            REQUIRE(!(ebx & (1U << 20)) || aspace_smap_enabled(), "the CPU has SMAP but it is not enabled");
            tests_serial_text("INFO SMEP "); tests_serial_text(aspace_smep_enabled() ? "enabled" : "not offered by this CPU model");
            tests_serial_text(", SMAP "); tests_serial_text(aspace_smap_enabled() ? "enabled" : "not offered by this CPU model"); tests_serial_text("\n");
        }
    }
    for (unsigned i = 0; i < 50; ++i) {
        struct aspace *s = aspace_create();
        REQUIRE(s, "create failed");
        REQUIRE(aspace_kernel_half_intact(s), "kernel half missing or carries User bits");
        REQUIRE(aspace_map(s, BASE, 1 + i % 20, RW) && aspace_map(s, BASE + 0x200000 + (i % 3) * 0x40000000ULL, 3, RX), "map failed");
        REQUIRE(aspace_data_pages(s) == 4 + i % 20, "page accounting wrong");
        aspace_destroy(s);
    }
    aspace_destroy(NULL);
    /* A kernel top-level slot that appeared after start-up would be missing from existing spaces: creation must refuse. */
    for (unsigned slot = 0; slot < 256; slot += 51) {
        aspace_test_flip_kernel_slot(slot);
        struct aspace *refused = aspace_create();
        aspace_test_flip_kernel_slot(slot);
        if (refused) { aspace_destroy(refused); return "an address space was created although the kernel half changed"; }
    }
    REQUIRE(pmm_free_pages() == frames && heap_live_allocations() == live, "address spaces leaked frames or heap blocks");
    return 0;
}

static const char *isolation(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *a = aspace_create(), *b = aspace_create();
    REQUIRE(a && b, "create failed");
    const char *err = 0;
    uint8_t buf[64], pattern_a[64], pattern_b[64];
    for (unsigned i = 0; i < 64; ++i) { pattern_a[i] = (uint8_t)(i + 1); pattern_b[i] = (uint8_t)(0xa0 + i); }
    if (!aspace_map(a, BASE, 4, RW) || !aspace_map(b, BASE, 4, RW)) err = "map failed";
    else if (!aspace_copy_to_user(a, BASE + 100, pattern_a, 64) || !aspace_copy_to_user(b, BASE + 100, pattern_b, 64)) err = "copy failed";
    else {
        uint64_t pa, pb;
        if (!aspace_translate(a, BASE + 100, false, &pa) || !aspace_translate(b, BASE + 100, false, &pb) || pa == pb) err = "two address spaces share a frame";
        else if (!aspace_copy_from_user(a, buf, BASE + 100, 64) || memcmp(buf, pattern_a, 64)) err = "A reads the wrong data";
        else if (!aspace_copy_from_user(b, buf, BASE + 100, 64) || memcmp(buf, pattern_b, 64)) err = "B reads the wrong data";
        else {
            for (unsigned page = 0; page < 4 && !err; ++page)
                for (unsigned other = 0; other < 4; ++other) {
                    uint64_t x, y;
                    if (aspace_translate(a, BASE + page * 4096ULL, false, &x) && aspace_translate(b, BASE + other * 4096ULL, false, &y) && x == y) { err = "frames shared between processes"; break; }
                }
        }
    }
    if (!err && aspace_map(b, BASE + 0x100000, 1, RW) && aspace_translate(a, BASE + 0x100000, false, 0)) err = "a mapping in B appeared in A";
    aspace_destroy(a);
    if (!err) {                                                   /* B survives A's destruction intact */
        if (!aspace_copy_from_user(b, buf, BASE + 100, 64) || memcmp(buf, pattern_b, 64)) err = "B's data changed when A was destroyed";
    }
    aspace_destroy(b);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *permissions(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    uint8_t byte = 7, in = 0;
    REQUIRE(aspace_map(s, BASE, 1, RO) && aspace_map(s, BASE + 0x1000, 1, RW) && aspace_map(s, BASE + 0x2000, 1, RX), "map failed");
    if (aspace_copy_to_user(s, BASE, &byte, 1)) err = "wrote to a read-only page";
    else if (!aspace_copy_from_user(s, &in, BASE, 1)) err = "cannot read a read-only page";
    else if (!aspace_copy_to_user(s, BASE + 0x1000, &byte, 1)) err = "cannot write a RW page";
    else if (aspace_copy_to_user(s, BASE + 0x2000, &byte, 1)) err = "wrote to an executable page (W^X)";
    else if (aspace_is_executable(s, BASE) || aspace_is_executable(s, BASE + 0x1000)) err = "a data page is executable";
    else if (!aspace_is_executable(s, BASE + 0x2000)) err = "the code page is not executable";
    else if (aspace_map(s, BASE + 0x3000, 1, VMM_PAGE_READ | VMM_PAGE_WRITE | VMM_PAGE_EXECUTE)) err = "a W+X mapping was accepted";
    else if (aspace_map(s, BASE + 0x3000, 1, VMM_PAGE_EXECUTE) || aspace_map(s, BASE + 0x3000, 1, VMM_PAGE_WRITE) || aspace_map(s, BASE + 0x3000, 1, 0)) err = "an invalid permission set was accepted";
    else if (aspace_translate(s, BASE + 0x3000, false, 0)) err = "a rejected mapping left a page behind";
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *bounds(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    if (aspace_map(s, 0, 1, RW) || aspace_map(s, 0x1000, 1, RW) || aspace_map(s, BASE - 0x1000, 1, RW)) err = "mapped below the user minimum";
    else if (aspace_map(s, ASPACE_USER_MAX, 1, RW) || aspace_map(s, 0xffff800000000000ULL, 1, RW) || aspace_map(s, 0xffffffff80000000ULL, 1, RW)) err = "mapped at or above the user maximum / in the kernel half";
    else if (aspace_map(s, ASPACE_USER_MAX - 0x1000, 2, RW)) err = "mapping crossed the user maximum";
    else if (aspace_map(s, 0x8000000000000000ULL, 1, RW) || aspace_map(s, 0x0000800000000000ULL, 1, RW)) err = "mapped a non-canonical address";
    else if (aspace_map(s, BASE + 1, 1, RW) || aspace_map(s, BASE, 0, RW) || aspace_map(s, BASE, ASPACE_MAP_MAX_PAGES + 1, RW)) err = "bad alignment or size accepted";
    else if (aspace_map(s, ~0ULL - 0xfff, 4, RW)) err = "overflowing range accepted";
    else if (!aspace_map(s, BASE, 2, RW)) err = "valid map failed";
    else if (aspace_map(s, BASE + 0x1000, 2, RW)) err = "overlapping map accepted";
    else if (aspace_translate(s, BASE + 0x2000, false, 0)) err = "overlap rejection left a page mapped";
    else if (aspace_user_bounds_ok(ASPACE_USER_MAX - 1, 2) || !aspace_user_bounds_ok(ASPACE_USER_MAX - 1, 1) || aspace_user_bounds_ok(BASE, SIZE_MAX)) err = "range check wrong";
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *copies(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    uint8_t src[6000], back[6000];
    for (unsigned i = 0; i < sizeof(src); ++i) src[i] = (uint8_t)(i * 13 + 5);
    if (!aspace_map(s, BASE, 2, RW)) err = "map failed";
    else if (!aspace_copy_to_user(s, BASE + 3000, src, 5000) || !aspace_copy_from_user(s, back, BASE + 3000, 5000) || memcmp(src, back, 5000)) err = "cross-page copy failed";
    else {
        /* A copy that runs off the end of the mapping must fail without writing anything. */
        uint8_t marker = 0;
        (void)aspace_copy_from_user(s, &marker, BASE + 2 * 4096 - 8, 1);
        uint8_t ones[64]; for (unsigned i = 0; i < 64; ++i) ones[i] = 0xee;
        uint8_t before[8], after[8];
        aspace_copy_from_user(s, before, BASE + 2 * 4096 - 8, 8);
        if (aspace_copy_to_user(s, BASE + 2 * 4096 - 8, ones, 64)) err = "copy beyond the mapping succeeded";
        else {
            aspace_copy_from_user(s, after, BASE + 2 * 4096 - 8, 8);
            if (memcmp(before, after, 8)) err = "a failed copy partly wrote";
            else if (aspace_copy_from_user(s, back, BASE + 2 * 4096 - 8, 64)) err = "read beyond the mapping succeeded";
            else if (aspace_copy_to_user(s, 0, ones, 4) || aspace_copy_from_user(s, back, 0xffffffff80000000ULL, 4) || aspace_copy_from_user(s, back, ASPACE_USER_MAX - 2, 8)) err = "kernel or null address accepted";
            else if (aspace_copy_to_user(s, BASE, NULL, 4) || aspace_copy_from_user(s, NULL, BASE, 4)) err = "null kernel buffer accepted";
            else if (!aspace_copy_to_user(s, BASE, ones, 0)) err = "zero-length copy rejected";
        }
        if (!err) {
            const char text[] = "hello/path";
            char out[32];
            aspace_copy_to_user(s, BASE + 0x1000 - 4, text, sizeof(text));
            if (aspace_copy_string_from_user(s, out, BASE + 0x1000 - 4, sizeof(out)) != 10 || memcmp(out, text, sizeof(text))) err = "string copy across a page failed";
            else if (aspace_copy_string_from_user(s, out, BASE + 0x1000 - 4, 5) != -1) err = "unterminated string accepted";
            else {
                uint8_t fill[16]; for (unsigned i = 0; i < 16; ++i) fill[i] = 'x';
                aspace_copy_to_user(s, BASE + 2 * 4096 - 16, fill, 16);          /* runs to the end of the mapping, no NUL */
                if (aspace_copy_string_from_user(s, out, BASE + 2 * 4096 - 16, sizeof(out)) != -1) err = "string running off the mapping accepted";
            }
        }
    }
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *rollback(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    uint32_t used = aspace_frames_used(s);
    for (int32_t fail_at = 0; fail_at < 40 && !err; ++fail_at) {
        pmm_test_fail_alloc_after(fail_at);
        bool ok = aspace_map(s, BASE + 0x3fe00000ULL, 30, RW);      /* crosses a page-table and a page-directory boundary */
        pmm_test_fail_alloc_after(-1);
        if (ok) { aspace_destroy(s); return fail_at >= 31 ? 0 : "an injected failure was ignored"; }
        if (aspace_frames_used(s) != used || aspace_data_pages(s) != 0) err = "a failed map kept frames";
        else if (aspace_translate(s, BASE + 0x3fe00000ULL, false, 0) || aspace_translate(s, BASE + 0x3fe00000ULL + 29 * 4096ULL, false, 0)) err = "a failed map left pages mapped";
        else if (pmm_free_pages() + 0 != frames - (aspace_frames_used(s) - 0) + 0 && pmm_free_pages() > frames) err = "free frame count inconsistent";
    }
    if (!err && !aspace_map(s, BASE + 0x3fe00000ULL, 30, RW)) err = "map failed after rollbacks";
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *limits(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    unsigned mapped = 0;
    for (unsigned chunk = 0; chunk < 8; ++chunk) {
        if (aspace_map(s, BASE + chunk * 0x200000ULL, 200, RW)) mapped += 200; else break;
    }
    if (mapped == 0 || mapped >= 1000) err = "frame limit not enforced";
    else if (aspace_frames_used(s) > ASPACE_MAX_FRAMES + 8) err = "exceeded the frame limit";
    else if (aspace_map(s, BASE + 0x10000000ULL, 200, RW)) err = "mapping beyond the limit accepted";
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *activation(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    uint64_t kroot = aspace_kernel_root_physical(), cr3;
    uint8_t v = 0x5a;
    if (!aspace_map(s, BASE, 2, RW) || !aspace_copy_to_user(s, BASE + 8, &v, 1)) err = "setup failed";
    else {
        uint64_t phys_expected, phys_seen = 0;
        aspace_translate(s, BASE + 8, false, &phys_expected);
        aspace_activate(s);                                     /* we keep running on the shared kernel half */
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        bool switched = (cr3 & ~0xfffULL) == aspace_root_physical(s);
        bool visible = vmm_translate(BASE + 8, &phys_seen) && phys_seen == phys_expected;
        bool kernel_ok = vmm_direct_map(phys_expected) && *(volatile uint8_t *)vmm_direct_map(phys_expected) == 0x5a;
        uint64_t unmapped_phys;
        bool hidden = !vmm_translate(BASE + 0x100000, &unmapped_phys);
        aspace_destroy(s);                                      /* destroyed while active: the kernel root comes back first */
        s = NULL;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        if (!switched) err = "CR3 was not switched";
        else if (!visible) err = "user mapping not visible through the active tables";
        else if (!kernel_ok) err = "kernel direct map unusable in the user address space";
        else if (!hidden) err = "an unmapped user address translated";
        else if ((cr3 & ~0xfffULL) != kroot || aspace_active()) err = "destroying the active address space left its root loaded";
    }
    if (s) aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *unmapping(void) {
    uint64_t frames = pmm_free_pages();
    struct aspace *s = aspace_create();
    REQUIRE(s, "create failed");
    const char *err = 0;
    uint8_t v = 1;
    uint32_t used0 = 0;
    if (!aspace_map(s, BASE, 8, RW)) err = "map failed";
    else {
        used0 = aspace_frames_used(s);
        if (!aspace_unmap(s, BASE + 0x2000, 3) || aspace_data_pages(s) != 5) err = "unmap failed";
        else if (aspace_frames_used(s) != used0 - 3) err = "unmap did not release frames";
        else if (aspace_copy_to_user(s, BASE + 0x2000, &v, 1) || !aspace_copy_to_user(s, BASE + 0x1000, &v, 1) || !aspace_copy_to_user(s, BASE + 0x5000, &v, 1)) err = "unmapped/neighbouring pages wrong";
        else if (aspace_unmap(s, BASE + 0x2000, 1) || aspace_unmap(s, BASE + 0x1000, 3)) err = "unmap of unmapped pages accepted";
        else if (!aspace_map(s, BASE + 0x2000, 3, RX) || !aspace_is_executable(s, BASE + 0x3000)) err = "remap failed";
    }
    aspace_destroy(s);
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

const struct test_case aspace_cases[] = {
    {"aspace create/destroy leaves no frames or heap blocks", lifecycle},
    {"aspace two processes cannot see each other's memory", isolation},
    {"aspace enforces R/W/X and rejects W+X", permissions},
    {"aspace rejects kernel, null, non-canonical and overlapping ranges", bounds},
    {"aspace validated copies are all-or-nothing", copies},
    {"aspace map rolls back on allocation failure", rollback},
    {"aspace frame limit is enforced and cleaned up", limits},
    {"aspace activation, kernel half and destroy-while-active", activation},
    {"aspace unmap releases frames", unmapping},
};
const size_t aspace_case_count = sizeof(aspace_cases) / sizeof(aspace_cases[0]);
