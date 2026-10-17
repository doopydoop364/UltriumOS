#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "processes/elf.h"
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "memory/aspace.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "fs/vfs.h"
#include "memory.h"

/* ELF64 loader tests: a well-formed synthetic executable must run; every malformed variation must be
   rejected with the right error and leave no process, frame or file handle behind. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define IMG 4096
#define PH0 64                       /* program header table */
#define CODE_OFF 0x200
#define DATA_OFF 0x300

static uint8_t good[IMG], work[IMG];
static size_t good_size;
static const uint8_t program[] = {
    0x48,0xb8,0x00,0x02,0x50,0x00,0,0,0,0,      /* movabs rax, 0x500200 */
    0x48,0x8b,0x38,                              /* mov rdi, [rax]        (initialised data = 0x2a) */
    0x48,0x03,0x78,0x08,                         /* add rdi, [rax + 8]    (bss = 0) */
    0xb8,0,0,0,0, 0xcd,0x80, 0xeb,0xfe};

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static void phdr(uint8_t *img, unsigned i, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t filesz, uint64_t memsz, uint64_t align) {
    uint8_t *p = img + PH0 + i * 56;
    w32(p, type); w32(p + 4, flags); w64(p + 8, off); w64(p + 16, va); w64(p + 24, va); w64(p + 32, filesz); w64(p + 40, memsz); w64(p + 48, align);
}
static void build(void) {
    memset(good, 0, sizeof(good));
    good[0] = 0x7f; good[1] = 'E'; good[2] = 'L'; good[3] = 'F'; good[4] = 2; good[5] = 1; good[6] = 1;
    w16(good + 16, 2); w16(good + 18, 62); w32(good + 20, 1);
    w64(good + 24, 0x400200); w64(good + 32, PH0);
    w16(good + 52, 64); w16(good + 54, 56); w16(good + 56, 3);
    phdr(good, 0, 1, 5, CODE_OFF, 0x400200, sizeof(program), sizeof(program), 0x1000);       /* R+X code */
    phdr(good, 1, 1, 6, DATA_OFF, 0x500200, 8, 0x1800, 0x1000);                              /* R+W data + bss */
    phdr(good, 2, 0x6474e551, 6, 0, 0, 0, 0, 16);                                            /* GNU_STACK, non-executable */
    memcpy(good + CODE_OFF, program, sizeof(program));
    w64(good + DATA_OFF, 42);
    good_size = DATA_OFF + 8;
}
static void clear_processes(void) {
    struct process_info list[PROCESS_MAX];
    unsigned n = process_list(list, PROCESS_MAX);
    for (unsigned i = 0; i < n; ++i) {
        int64_t st;
        if (list[i].state == PROC_ACTIVE) (void)process_kill(list[i].pid);
        if (list[i].state == PROC_CREATED) (void)process_discard(list[i].pid);
        else (void)process_wait(list[i].pid, &st);
    }
}

static const char *loads_and_runs(void) {
    clear_processes();
    build();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    struct elf_plan plan;
    REQUIRE(elf_parse(good, good_size, &plan) == ELF_OK, "the valid image was rejected");
    REQUIRE(plan.count == 2 && plan.entry == 0x400200 && plan.pages == 1 + 2, "plan is wrong");
    REQUIRE((plan.segment[0].permissions & VMM_PAGE_EXECUTE) && !(plan.segment[0].permissions & VMM_PAGE_WRITE), "code segment permissions wrong");
    REQUIRE((plan.segment[1].permissions & VMM_PAGE_WRITE) && !(plan.segment[1].permissions & VMM_PAGE_EXECUTE), "data segment permissions wrong");
    enum elf_error e;
    uint32_t pid = elf_spawn_image("elf-ok", 0, good, good_size, &e);
    REQUIRE(pid && e == ELF_OK, "spawn failed");
    int64_t st = -1;
    REQUIRE(process_wait(pid, &st), "wait failed");
    REQUIRE(st == 42, "the program saw the wrong initialised data or bss");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "leaked");
    return 0;
}

struct bad { const char *name; unsigned where; uint64_t value; unsigned width; enum elf_error expect; };
static const struct bad mutations[] = {
    {"magic[0]", 0, 0x7e, 1, ELF_E_MAGIC}, {"magic[1]", 1, 'X', 1, ELF_E_MAGIC}, {"magic[3]", 3, 0, 1, ELF_E_MAGIC},
    {"32-bit class", 4, 1, 1, ELF_E_CLASS}, {"class 0", 4, 0, 1, ELF_E_CLASS},
    {"big endian", 5, 2, 1, ELF_E_ENDIAN}, {"ident version", 6, 2, 1, ELF_E_VERSION},
    {"ET_DYN (PIE)", 16, 3, 2, ELF_E_TYPE}, {"ET_REL", 16, 1, 2, ELF_E_TYPE}, {"ET_CORE", 16, 4, 2, ELF_E_TYPE},
    {"ARM", 18, 40, 2, ELF_E_MACHINE}, {"i386", 18, 3, 2, ELF_E_MACHINE}, {"machine 0", 18, 0, 2, ELF_E_MACHINE},
    {"e_version", 20, 2, 4, ELF_E_VERSION},
    {"ehsize", 52, 63, 2, ELF_E_HEADER}, {"phentsize", 54, 32, 2, ELF_E_HEADER}, {"phnum 0", 56, 0, 2, ELF_E_HEADER},
    {"phnum 17", 56, 17, 2, ELF_E_HEADER}, {"phnum 0xffff", 56, 0xffff, 2, ELF_E_HEADER},
    {"phoff 0", 32, 0, 8, ELF_E_PHDR_RANGE}, {"phoff 63", 32, 63, 8, ELF_E_PHDR_RANGE},
    {"phoff past the file", 32, 0x100000, 8, ELF_E_PHDR_RANGE}, {"phoff = UINT64_MAX", 32, ~0ULL, 8, ELF_E_PHDR_RANGE},
    {"phoff + table wraps", 32, ~0ULL - 100, 8, ELF_E_PHDR_RANGE},
    {"table runs past the end", 32, 0x3f0, 8, ELF_E_PHDR_RANGE},
    {"entry 0", 24, 0, 8, ELF_E_ENTRY}, {"entry in data (NX)", 24, 0x500200, 8, ELF_E_ENTRY},
    {"entry in the kernel", 24, 0xffffffff80000000ULL, 8, ELF_E_ENTRY}, {"entry past the segment", 24, 0x400200 + 0x1000, 8, ELF_E_ENTRY},
    {"entry just before the segment", 24, 0x4001ff, 8, ELF_E_ENTRY},
    /* segment 0 fields live at PH0 + 0: type +0, flags +4, offset +8, vaddr +16, filesz +32, memsz +40, align +48 */
    {"PT_INTERP", PH0 + 0, 3, 4, ELF_E_SEGMENT_TYPE}, {"PT_DYNAMIC", PH0 + 56, 2, 4, ELF_E_SEGMENT_TYPE},
    {"PT_TLS", PH0 + 56, 7, 4, ELF_E_SEGMENT_TYPE}, {"PT_SHLIB", PH0 + 56, 5, 4, ELF_E_SEGMENT_TYPE}, {"unknown type", PH0 + 56, 0x12345678, 4, ELF_E_SEGMENT_TYPE},
    {"executable stack", PH0 + 112 + 4, 7, 4, ELF_E_SEGMENT_TYPE},
    {"code W+X", PH0 + 4, 7, 4, ELF_E_SEGMENT_PERM}, {"data W+X", PH0 + 56 + 4, 7, 4, ELF_E_SEGMENT_PERM}, {"bogus flag bits", PH0 + 4, 0x100f5, 4, ELF_E_SEGMENT_PERM},
    {"offset past the file", PH0 + 8, 0x10000, 8, ELF_E_SEGMENT_BOUNDS}, {"offset = UINT64_MAX", PH0 + 8, ~0ULL, 8, ELF_E_SEGMENT_BOUNDS},
    {"filesz past the file", PH0 + 32, 0x10000, 8, ELF_E_SEGMENT_BOUNDS}, {"filesz > memsz", PH0 + 32, 0x100, 8, ELF_E_SEGMENT_BOUNDS},
    {"filesz wraps with offset", PH0 + 32, ~0ULL - 8, 8, ELF_E_SEGMENT_BOUNDS},
    {"vaddr 0", PH0 + 16, 0, 8, ELF_E_SEGMENT_ADDR}, {"vaddr below 4 MiB", PH0 + 16, 0x1000, 8, ELF_E_SEGMENT_ADDR},
    {"vaddr in the kernel half", PH0 + 16, 0xffffffff80000000ULL, 8, ELF_E_SEGMENT_ADDR}, {"vaddr at the user limit", PH0 + 16, 0x00007fffffff0000ULL, 8, ELF_E_SEGMENT_ADDR},
    {"vaddr non-canonical", PH0 + 16, 0x0000800000000000ULL, 8, ELF_E_SEGMENT_ADDR}, {"vaddr + memsz wraps", PH0 + 16, ~0ULL - 0x80, 8, ELF_E_SEGMENT_ADDR},
    {"memsz crosses the limit", PH0 + 56 + 16, 0x00007fffffffe000ULL, 8, ELF_E_SEGMENT_ADDR},
    {"memsz 2 MiB", PH0 + 40, 0x200000, 8, ELF_E_TOO_BIG}, {"align not a power of two", PH0 + 48, 3, 8, ELF_E_SEGMENT_ADDR},
    {"segments overlap", PH0 + 56 + 16, 0x400000, 8, ELF_E_OVERLAP}, {"segments share a page", PH0 + 56 + 16, 0x400800, 8, ELF_E_OVERLAP},
};
static uint64_t read_field(const uint8_t *p, unsigned width) { uint64_t v = 0; for (unsigned i = 0; i < width; ++i) v |= (uint64_t)p[i] << (8 * i); return v; }
static const char *rejects_malformed(void) {
    clear_processes();
    build();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    for (unsigned i = 0; i < sizeof(mutations) / sizeof(mutations[0]); ++i) {
        const struct bad *m = &mutations[i];
        memcpy(work, good, sizeof(work));
        for (unsigned b = 0; b < m->width; ++b) work[m->where + b] = (uint8_t)(m->value >> (8 * b));
        struct elf_plan plan;
        enum elf_error e = elf_parse(work, good_size, &plan);
        if (e != m->expect) { (void)read_field; return m->name; }
        enum elf_error se;
        if (elf_spawn_image("bad", 0, work, good_size, &se) != 0 || se != m->expect) return m->name;
    }
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "rejected images leaked");
    return 0;
}

static const char *rejects_truncated(void) {
    build();
    struct elf_plan plan;
    static const size_t cuts[] = {0, 1, 3, 4, 16, 63, 64, 65, 100, 64 + 55, 64 + 167, 64 + 168, 0x1ff, 0x200, 0x200 + 10, 0x2ff, 0x300, 0x307};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); ++i) {
        enum elf_error e = elf_parse(good, cuts[i], &plan);
        if (e == ELF_OK) return "a truncated image was accepted";      /* every cut below the full size removes needed bytes */
    }
    REQUIRE(elf_parse(good, good_size + 1, &plan) == ELF_OK, "trailing bytes must be tolerated");
    REQUIRE(elf_parse(NULL, 100, &plan) != ELF_OK && elf_parse(good, 100, NULL) != ELF_OK, "null arguments accepted");
    REQUIRE(elf_parse(good, ELF_MAX_FILE_BYTES + 1, &plan) == ELF_E_TOO_BIG, "oversized image accepted");
    /* No load segment at all. */
    memcpy(work, good, sizeof(work));
    work[PH0] = 0; work[PH0 + 56] = 0;
    REQUIRE(elf_parse(work, good_size, &plan) == ELF_E_NO_LOAD, "an image with no PT_LOAD was accepted");
    /* Too many loadable segments. */
    memcpy(work, good, sizeof(work));
    w16(work + 56, 16);
    for (unsigned i = 0; i < 16; ++i) phdr(work, i, 1, 4, DATA_OFF, 0x600000 + i * 0x2000, 8, 0x1000, 0x1000);
    REQUIRE(elf_parse(work, 1200, &plan) == ELF_E_HEADER, "more than the allowed number of segments accepted");
    return 0;
}

static const char *fuzz(void) {
    build();
    uint32_t state = 0x1badc0de;
    unsigned accepted = 0;
    for (unsigned n = 0; n < 4000; ++n) {
        memcpy(work, good, sizeof(work));
        unsigned flips = 1 + n % 4;
        for (unsigned f = 0; f < flips; ++f) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            unsigned at = (state >> 3) % good_size;
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            work[at] ^= (uint8_t)(1U << (state & 7));
        }
        struct elf_plan plan;
        if (elf_parse(work, good_size, &plan) != ELF_OK) continue;
        ++accepted;
        /* Whatever was accepted must satisfy every loader invariant. */
        bool entry_exec = false;
        uint32_t pages = 0;
        if (!plan.count || plan.count > ELF_MAX_SEGMENTS || plan.pages > ELF_MAX_PAGES) return "fuzz: accepted plan has bad counts";
        for (unsigned i = 0; i < plan.count; ++i) {
            const struct elf_segment *s = &plan.segment[i];
            if (!aspace_user_bounds_ok(s->vaddr, (size_t)s->memsz) || s->filesz > s->memsz || s->offset > good_size || s->filesz > good_size - s->offset) return "fuzz: accepted segment out of bounds";
            if ((s->permissions & VMM_PAGE_WRITE) && (s->permissions & VMM_PAGE_EXECUTE)) return "fuzz: accepted W+X";
            if ((s->permissions & VMM_PAGE_EXECUTE) && plan.entry >= s->vaddr && plan.entry - s->vaddr < s->memsz) entry_exec = true;
            pages += (uint32_t)((((s->vaddr + s->memsz + 4095) & ~4095ULL) - (s->vaddr & ~4095ULL)) / 4096);
            for (unsigned k = 0; k < i; ++k) {
                const struct elf_segment *t = &plan.segment[k];
                uint64_t a0 = s->vaddr & ~4095ULL, a1 = (s->vaddr + s->memsz + 4095) & ~4095ULL, b0 = t->vaddr & ~4095ULL, b1 = (t->vaddr + t->memsz + 4095) & ~4095ULL;
                if (a0 < b1 && b0 < a1) return "fuzz: accepted overlapping segments";
            }
        }
        if (!entry_exec || pages != plan.pages) return "fuzz: accepted entry or page count wrong";
    }
    REQUIRE(accepted > 0 && accepted < 4000, "fuzz did not exercise both outcomes");
    return 0;
}

static const char *from_filesystem(void) {
    clear_processes();
    build();
    uint64_t frames = pmm_free_pages();
    /* warm the RAMFS so the frame comparison measures the loader */
    (void)vfs_unlink("/elf-test");
    struct vfs_file f = {0}; size_t n;
    REQUIRE(vfs_create_file("/elf-test") == VFS_OK && vfs_file_open("/elf-test", VFS_OPEN_WRITE, &f) == VFS_OK, "setup failed");
    REQUIRE(vfs_file_write(&f, good, good_size > 4096 ? 4096 : good_size, &n) == VFS_OK && n == good_size, "setup write failed");
    (void)vfs_file_close(&f);
    uint64_t after_file = pmm_free_pages();
    enum elf_error e;
    uint32_t pid = elf_spawn_file("/elf-test", 0, &e);
    REQUIRE(pid && e == ELF_OK, "spawning from the filesystem failed");
    int64_t st;
    REQUIRE(process_wait(pid, &st) && st == 42, "program from the filesystem misbehaved");
    REQUIRE(pmm_free_pages() == after_file, "loading leaked memory");
    REQUIRE(elf_spawn_file("/nope", 0, &e) == 0 && e == ELF_E_NOT_FOUND, "missing file not reported");
    REQUIRE(elf_spawn_file("/", 0, &e) == 0 && e == ELF_E_NOT_FILE, "directory accepted");
    REQUIRE(elf_spawn_file("relative", 0, &e) == 0 && e != ELF_OK, "relative path accepted");
    REQUIRE(vfs_create_file("/elf-empty") == VFS_OK, "setup failed");
    REQUIRE(elf_spawn_file("/elf-empty", 0, &e) == 0 && e == ELF_E_TOO_SMALL, "empty file accepted");
    REQUIRE(vfs_file_open("/elf-test", VFS_OPEN_WRITE, &f) == VFS_OK, "setup failed");
    (void)vfs_file_write(&f, "this is plain text, not an executable program at all, certainly not ELF....", 70, &n);
    (void)vfs_file_close(&f);
    REQUIRE(elf_spawn_file("/elf-test", 0, &e) == 0 && e == ELF_E_MAGIC, "text file accepted");
    REQUIRE(vfs_unlink("/elf-test") == VFS_OK && vfs_unlink("/elf-empty") == VFS_OK, "cleanup failed (a handle stayed open?)");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "leaked");
    return 0;
}

static const char *resources(void) {
    clear_processes();
    build();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    enum elf_error e;
    /* process table full */
    uint32_t fill[PROCESS_MAX + 1]; unsigned made = 0;
    while (made < PROCESS_MAX + 1 && (fill[made] = process_create("fill", 0))) ++made;
    REQUIRE(elf_spawn_image("x", 0, good, good_size, &e) == 0 && e == ELF_E_PROCESS, "spawn with a full table did not fail cleanly");
    for (unsigned i = 0; i < made; ++i) (void)process_discard(fill[i]);
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "table exhaustion leaked");
    /* allocation failure at every point of loading */
    bool succeeded = false;
    for (int32_t k = 0; k < 80 && !succeeded; ++k) {
        pmm_test_fail_alloc_after(k);
        uint32_t pid = elf_spawn_image("inject", 0, good, good_size, &e);
        pmm_test_fail_alloc_after(-1);
        if (pid) {
            int64_t st;
            REQUIRE(process_wait(pid, &st) && st == 42, "program started under injection misbehaved");
            succeeded = true;
        } else {
            REQUIRE(e == ELF_E_PROCESS || e == ELF_E_NOMEM, "wrong error under allocation failure");
        }
        REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "an allocation failure during loading leaked");
    }
    REQUIRE(succeeded, "loading never succeeded");
    /* too many pages in total: 401 pages are fine, 601 are not */
    memcpy(work, good, sizeof(work));
    phdr(work, 1, 1, 6, DATA_OFF, 0x500000, 8, 200 * 4096, 0x1000);
    phdr(work, 2, 1, 6, DATA_OFF, 0x800000, 8, 200 * 4096, 0x1000);
    w16(work + 56, 3);
    struct elf_plan plan;
    REQUIRE(elf_parse(work, good_size, &plan) == ELF_OK && plan.pages == 401, "401 pages were refused");
    phdr(work, 3, 1, 6, DATA_OFF, 0xb00000, 8, 200 * 4096, 0x1000);
    w16(work + 56, 4);
    REQUIRE(elf_parse(work, good_size, &plan) == ELF_E_TOO_BIG, "segments totalling more than 512 pages were accepted");
    /* A 1 MiB segment that starts mid-page spans 257 pages: refused by the parser, not at map time. */
    memcpy(work, good, sizeof(work));
    phdr(work, 1, 1, 6, DATA_OFF, 0x500200, 8, 0x100000, 0x1000);
    REQUIRE(elf_parse(work, good_size, &plan) == ELF_E_TOO_BIG, "a segment spanning more pages than one mapping allows was accepted");
    phdr(work, 1, 1, 6, DATA_OFF, 0x500000, 8, 0x100000, 0x1000);
    REQUIRE(elf_parse(work, good_size, &plan) == ELF_OK, "a page-aligned 1 MiB segment was refused");
    return 0;
}

const struct test_case elf_cases[] = {
    {"elf loads a static executable with initialised data and bss", loads_and_runs},
    {"elf rejects 55 malformed headers and segments", rejects_malformed},
    {"elf rejects truncated images, empty and excessive segment tables", rejects_truncated},
    {"elf parser survives 4000 corrupted images and keeps every invariant", fuzz},
    {"elf spawns from the filesystem and rejects bad files", from_filesystem},
    {"elf loading is clean under table exhaustion and allocation failure", resources},
};
const size_t elf_case_count = sizeof(elf_cases) / sizeof(elf_cases[0]);
