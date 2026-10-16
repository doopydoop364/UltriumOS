#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "processes/elf.h"
#include "processes/process.h"
#include "processes/programs.h"
#include "scheduler/scheduler.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "system/interrupts.h"
#include "abi/ultrium_syscall.h"
#include "memory.h"

/* The sample programs in os/programs, built by the normal kernel build, loaded from the VFS and run in ring 3. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)

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
static bool contains(const char *text, const char *needle) {
    size_t n = 0; while (needle[n]) ++n;
    for (const char *p = text; *p; ++p) { size_t i = 0; while (i < n && p[i] == needle[i]) ++i; if (i == n) return true; }
    return false;
}
static unsigned count_lines(const char *text) { unsigned n = 0; for (; *text; ++text) if (*text == '\n') ++n; return n; }
/* first and last number following "tick=" */
static bool tick_range(const char *text, uint64_t *first, uint64_t *last) {
    bool any = false;
    for (const char *p = text; *p; ++p) {
        if (p[0] == 't' && p[1] == 'i' && p[2] == 'c' && p[3] == 'k' && p[4] == '=') {
            uint64_t v = 0; const char *q = p + 5;
            while (*q >= '0' && *q <= '9') v = v * 10 + (uint64_t)(*q++ - '0');
            if (!any) *first = v;
            *last = v; any = true;
        }
    }
    return any;
}

static const char *installed(void) {
    REQUIRE(programs_count() >= 4, "programs missing");
    for (unsigned i = 0; i < programs_count(); ++i) {
        const uint8_t *image; size_t size;
        if (programs_is_test_only(i)) {            /* test programs are valid executables but are not installed at boot */
            struct elf_plan p2;
            REQUIRE(programs_image(i, &image, &size) && elf_parse(image, size, &p2) == ELF_OK && size <= 4096, "a test program is not a valid small executable");
            char tp[40] = "/bin/"; size_t k = 5; for (const char *q = programs_name(i); *q; ++q) tp[k++] = *q; tp[k] = 0;
            struct vfs_info ti;
            REQUIRE(vfs_stat(tp, &ti) == VFS_NOT_FOUND, "a test program is installed at boot");
            continue;
        }
        REQUIRE(programs_image(i, &image, &size), "embedded image missing");
        struct elf_plan plan;
        REQUIRE(elf_parse(image, size, &plan) == ELF_OK, "an embedded program is not a valid executable");
        char path[40] = "/bin/";
        const char *name = programs_name(i);
        size_t n = 5; for (const char *p = name; *p; ++p) path[n++] = *p; path[n] = 0;
        struct vfs_info info;
        REQUIRE(vfs_stat(path, &info) == VFS_OK && info.type == VFS_NODE_FILE && info.size == size, "program not installed in /bin");
        struct vfs_file f = {0};
        static uint8_t back[4096];
        size_t got = 0;
        REQUIRE(vfs_file_open(path, VFS_OPEN_READ, &f) == VFS_OK && vfs_file_read(&f, back, sizeof(back), &got) == VFS_OK && got == size, "cannot read the installed program");
        (void)vfs_file_close(&f);
        REQUIRE(!memcmp(back, image, size), "installed program differs from the embedded image");
    }
    return 0;
}

static const char *hello(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    enum elf_error e;
    uint32_t pid = elf_spawn_file("/bin/hello", 0, &e);
    REQUIRE(pid, elf_error_name(e));
    int64_t st = -1;
    static char out[PROCESS_CONSOLE_MAX];
    REQUIRE(process_wait_ex(pid, &st, out, sizeof(out)), "wait failed");
    REQUIRE(st == 0, "hello did not exit 0");
    REQUIRE(contains(out, "Hello from Ultrium userspace! pid=") && contains(out, "abi=0x0000000000010000"), "hello printed the wrong text");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "leaked");
    return 0;
}

static const char *counters_interleave(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    struct sched_stats s0, s1;
    sched_stats_get(&s0);
    enum elf_error e;
    uint32_t a = elf_spawn_file("/bin/counter", 0, &e), b = elf_spawn_file("/bin/counter", 0, &e);
    REQUIRE(a && b && a != b, "spawn failed");
    int64_t sa = -1, sb = -1;
    static char oa[PROCESS_CONSOLE_MAX], ob[PROCESS_CONSOLE_MAX];
    REQUIRE(process_wait_ex(a, &sa, oa, sizeof(oa)) && process_wait_ex(b, &sb, ob, sizeof(ob)), "wait failed");
    REQUIRE(sa == 0 && sb == 0, "a counter failed");
    REQUIRE(count_lines(oa) == 5 && count_lines(ob) == 5, "a counter printed the wrong number of lines");
    uint64_t fa, la, fb, lb;
    REQUIRE(tick_range(oa, &fa, &la) && tick_range(ob, &fb, &lb), "no tick values");
    REQUIRE(fa < lb && fb < la, "the two counters did not run at the same time (their tick ranges are disjoint)");
    sched_stats_get(&s1);
    REQUIRE(s1.preemptions >= s0.preemptions + 2, "the counters were not preempted");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "leaked");
    return 0;
}

static const char *filedemo(void) {
    clear_processes();
    (void)vfs_unlink("/filedemo.txt");
    uint64_t frames = pmm_free_pages();
    enum elf_error e;
    uint32_t pid = elf_spawn_file("/bin/filedemo", 0, &e);
    REQUIRE(pid, elf_error_name(e));
    int64_t st = -1;
    static char out[PROCESS_CONSOLE_MAX];
    REQUIRE(process_wait_ex(pid, &st, out, sizeof(out)), "wait failed");
    REQUIRE(st == 0, "filedemo failed");
    REQUIRE(contains(out, ": OK"), "filedemo did not report success");
    /* The file is where the program says, with the content it wrote. */
    const char *path = contains(out, "/volumes/disk/filedemo.txt") ? "/volumes/disk/filedemo.txt" : "/filedemo.txt";
    struct vfs_file f = {0}; uint8_t back[80] = {0}; size_t got = 0;
    REQUIRE(vfs_file_open(path, VFS_OPEN_READ, &f) == VFS_OK && vfs_file_read(&f, back, sizeof(back), &got) == VFS_OK, "the file written by the program is missing");
    (void)vfs_file_close(&f);
    REQUIRE(got == 42 && !memcmp(back, "Written by filedemo through system calls.\n", 42), "the file content is wrong");
    REQUIRE(vfs_unlink(path) == VFS_OK, "cleanup failed (a descriptor stayed open?)");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "leaked");
    return 0;
}

static const char *fpu_isolation(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    struct sched_stats s0, s1;
    sched_stats_get(&s0);
    enum elf_error e;
    uint32_t p[3];
    for (unsigned i = 0; i < 3; ++i) { p[i] = elf_spawn_file("/bin/fpudemo", 0, &e); REQUIRE(p[i], "spawn failed"); }
    for (unsigned i = 0; i < 3; ++i) {
        int64_t st = -1;
        static char out[PROCESS_CONSOLE_MAX];
        REQUIRE(process_wait_ex(p[i], &st, out, sizeof(out)) && st == 0 && contains(out, "fpudemo: ok"), "floating-point state was corrupted or leaked between processes");
    }
    sched_stats_get(&s1);
    REQUIRE(s1.switches > s0.switches + 6, "the processes never overlapped");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "leaked");
    return 0;
}

static const char *copied_and_missing(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    /* A program copied to another path runs the same: the loader depends on the VFS, not on /bin. */
    REQUIRE(vfs_create_directory("/usr-test") == VFS_OK || vfs_is_directory("/usr-test"), "setup failed");
    (void)vfs_unlink("/usr-test/hello2");
    REQUIRE(vfs_copy("/bin/hello", "/usr-test/hello2") == VFS_OK, "copy failed");
    enum elf_error e;
    uint32_t pid = elf_spawn_file("/usr-test/hello2", 0, &e);
    int64_t st = -1;
    REQUIRE(pid && process_wait(pid, &st) && st == 0, "the copied program did not run");
    REQUIRE(vfs_unlink("/usr-test/hello2") == VFS_OK && vfs_unlink("/usr-test") == VFS_OK, "cleanup failed");
    REQUIRE(elf_spawn_file("/bin/nope", 0, &e) == 0 && e == ELF_E_NOT_FOUND, "missing program not reported");
    for (unsigned i = 0; i < 30; ++i) {
        pid = elf_spawn_file("/bin/hello", 0, &e);
        REQUIRE(pid && process_wait(pid, &st) && st == 0, "repeated launch failed");
    }
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "leaked");
    return 0;
}

const struct test_case program_cases[] = {
    {"programs are embedded, valid and installed in /bin", installed},
    {"program hello runs in ring 3 and prints through the console syscall", hello},
    {"two counter programs run concurrently and are preempted", counters_interleave},
    {"program filedemo writes and reads back a file through system calls", filedemo},
    {"three fpudemo programs keep their own floating-point state", fpu_isolation},
    {"a copied program runs, a missing one is reported, 30 launches do not leak", copied_and_missing},
};
const size_t program_case_count = sizeof(program_cases) / sizeof(program_cases[0]);
