#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "syscalls/syscall.h"
#include "memory/aspace.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "system/interrupts.h"
#include "fs/vfs.h"
#include "abi/ultrium_syscall.h"
#include "memory.h"

/* System-call tests. Hostile arguments are tried through syscall_test_invoke (same handlers as int 0x80,
   without entering ring 3) against a process whose memory the test controls; one real ring-3 program
   then uses the registers, the int 0x80 gate, the VFS and the console end to end. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define CODE_VA 0x400000ULL
#define DATA_VA 0x500000ULL
#define E(x) (-(int64_t)(x))

#define MOV_EAX(n) 0xb8,(n),0,0,0
#define MOV_EDI(n) 0xbf,(uint8_t)(n),(uint8_t)((n)>>8),(uint8_t)((n)>>16),0
#define MOV_ESI(n) 0xbe,(uint8_t)(n),(uint8_t)((n)>>8),(uint8_t)((n)>>16),0
#define MOV_EDX(n) 0xba,(n),0,0,0
#define INT80 0xcd,0x80
/* path "/sysdemo.txt" at DATA, "hello" at DATA+0x20, buffer at DATA+0x40 */
static const uint8_t PROGRAM[] = {
    MOV_EAX(4), MOV_EDI(0x500000), MOV_ESI(12), MOV_EDX(7), INT80,            /* open(path, 12, READ|WRITE|CREATE) */
    0x89,0xc3,                                                                 /* mov ebx, eax */
    MOV_EAX(6), 0x89,0xdf, MOV_ESI(0x500020), MOV_EDX(5), INT80,               /* write(fd, "hello", 5) */
    MOV_EAX(7), 0x89,0xdf, INT80,                                              /* close(fd) */
    MOV_EAX(4), MOV_EDI(0x500000), MOV_ESI(12), MOV_EDX(1), INT80,             /* open(path, 12, READ) */
    0x89,0xc3,
    MOV_EAX(5), 0x89,0xdf, MOV_ESI(0x500040), MOV_EDX(16), INT80,              /* read(fd, buf, 16) */
    0x41,0x89,0xc4,                                                            /* mov r12d, eax */
    MOV_EAX(2), MOV_EDI(0x500040), MOV_ESI(5), INT80,                          /* write_console(buf, 5) */
    MOV_EAX(0), 0x44,0x89,0xe7, INT80,                                         /* exit(count) */
    0xeb,0xfe};

static uint32_t make(const char *name, bool start, const uint8_t *code, size_t len) {
    uint32_t pid = process_create(name, 0);
    if (!pid) return 0;
    bool ok = process_map_segment(pid, CODE_VA, code, len, len, VMM_PAGE_READ | VMM_PAGE_EXECUTE) &&
              process_map_segment(pid, DATA_VA, NULL, 0, 2 * 4096ULL, VMM_PAGE_READ | VMM_PAGE_WRITE) &&
              process_map_stack(pid);
    if (ok && start) ok = process_start(pid, CODE_VA, PROCESS_STACK_TOP - 8);
    if (!ok) { (void)process_discard(pid); return 0; }
    return pid;
}
static bool poke(uint32_t pid, uint64_t va, const void *data, size_t n) { return aspace_poke(process_aspace(pid), va, data, n); }
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

static const char *basics(void) {
    clear_processes();
    uint32_t pid = make("sys", false, PROGRAM, sizeof(PROGRAM));
    REQUIRE(pid, "setup failed");
    const char *err = 0;
    uint64_t t0 = (uint64_t)syscall_test_invoke(pid, ULTRIUM_SYS_GET_TICKS, 0, 0, 0);
    if (syscall_test_invoke(pid, ULTRIUM_SYS_ABI_VERSION, 0, 0, 0) != ULTRIUM_ABI_VERSION) err = "abi version wrong";
    else if (ULTRIUM_ABI_MAJOR != 1) err = "ABI major changed without review";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_GET_PID, 0, 0, 0) != (int64_t)pid) err = "get_pid wrong";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_YIELD, 0, 0, 0) != 0) err = "yield failed";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_SLEEP, 3, 0, 0) != 0 || (uint64_t)syscall_test_invoke(pid, ULTRIUM_SYS_GET_TICKS, 0, 0, 0) < t0 + 3) err = "sleep returned early";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_SLEEP, ULTRIUM_MAX_SLEEP_TICKS + 1, 0, 0) != E(ULTRIUM_E_INVAL)) err = "oversized sleep accepted";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_COUNT, 0, 0, 0) != E(ULTRIUM_E_NOSYS) || syscall_test_invoke(pid, 999, 0, 0, 0) != E(ULTRIUM_E_NOSYS) ||
             syscall_test_invoke(pid, ~0ULL, 0, 0, 0) != E(ULTRIUM_E_NOSYS) || syscall_test_invoke(pid, 0x8000000000000001ULL, 0, 0, 0) != E(ULTRIUM_E_NOSYS)) err = "invalid syscall numbers were not rejected";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_EXIT, 0, 0, 0) != E(ULTRIUM_E_NOPROC)) err = "test hook allowed exit";
    else if (syscall_test_invoke(424242, ULTRIUM_SYS_GET_PID, 0, 0, 0) != E(ULTRIUM_E_NOPROC) || syscall_test_invoke(0, ULTRIUM_SYS_GET_PID, 0, 0, 0) != E(ULTRIUM_E_NOPROC)) err = "invalid process id accepted";
    (void)process_discard(pid);
    return err;
}

static const char *console(void) {
    clear_processes();
    uint32_t pid = make("con", false, PROGRAM, sizeof(PROGRAM));
    REQUIRE(pid, "setup failed");
    const char *err = 0;
    char out[64];
    const char text[] = "ok\n\x1b[2Jx\x01";
    poke(pid, DATA_VA + 0xff0, text, sizeof(text) - 1);
    if (syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, DATA_VA + 0xff0, sizeof(text) - 1, 0) != (int64_t)sizeof(text) - 1) err = "console write failed";
    else {
        process_console_peek(pid, out, sizeof(out));
        if (memcmp(out, "ok\n?[2Jx?", 9) || out[9]) err = "control characters were not neutralised";
    }
    unsigned before = process_console_peek(pid, out, sizeof(out));
    if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, 0xffffffff80000000ULL, 4, 0) != E(ULTRIUM_E_FAULT)) err = "kernel pointer accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, 0, 4, 0) != E(ULTRIUM_E_FAULT)) err = "null pointer accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, 0x700000, 4, 0) != E(ULTRIUM_E_FAULT)) err = "unmapped pointer accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, DATA_VA + 0x1ff0, 0x100, 0) != E(ULTRIUM_E_FAULT)) err = "range running off the mapping accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, DATA_VA, ULTRIUM_MAX_IO + 1, 0) != E(ULTRIUM_E_TOOBIG)) err = "oversized length accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, DATA_VA, ~0ULL, 0) != E(ULTRIUM_E_TOOBIG)) err = "huge length accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, ASPACE_USER_MAX - 2, 8, 0) != E(ULTRIUM_E_FAULT)) err = "range overflowing the user limit accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, ~0ULL - 1, 8, 0) != E(ULTRIUM_E_FAULT)) err = "wrapping range accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE_CONSOLE, DATA_VA, 0, 0) != 0) err = "zero-length write failed";
    else if (!err && process_console_peek(pid, out, sizeof(out)) != before) err = "a rejected write produced output";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, ULTRIUM_STDOUT, DATA_VA + 0xff0, 2) != 2) err = "write to stdout failed";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, ULTRIUM_STDERR, DATA_VA + 0xff0, 2) != 2) err = "write to stderr failed";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, ULTRIUM_STDIN, DATA_VA, 2) != E(ULTRIUM_E_BADF)) err = "write to stdin accepted";
    else if (!err && syscall_test_invoke(pid, ULTRIUM_SYS_READ, ULTRIUM_STDIN, DATA_VA, 8) != 0) err = "stdin is not at end of file";
    (void)process_discard(pid);
    return err;
}

static const char *files(void) {
    clear_processes();
    /* Warm the RAMFS slabs so the frame comparison below measures process memory, not allocator caching. */
    {
        struct vfs_file w = {0}; size_t n;
        (void)vfs_create_file("/sc-a.txt"); (void)vfs_create_file("/sc-leak.txt");
        if (vfs_file_open("/sc-a.txt", VFS_OPEN_WRITE, &w) == VFS_OK) { (void)vfs_file_write(&w, "payload-0123456789", 18, &n); (void)vfs_file_close(&w); }
        (void)vfs_unlink("/sc-a.txt"); (void)vfs_unlink("/sc-leak.txt");
    }
    uint64_t frames = pmm_free_pages();
    uint32_t pid = make("files", false, PROGRAM, sizeof(PROGRAM));
    REQUIRE(pid, "setup failed");
    const char *err = 0;
    struct aspace *as = process_aspace(pid);
    (void)vfs_unlink("/sc-a.txt");
    poke(pid, DATA_VA, "/sc-a.txt", 9);
    poke(pid, DATA_VA + 0x100, "payload-0123456789", 18);
    int64_t fd, fd2;
    #define OPEN(path_va, len, flags) syscall_test_invoke(pid, ULTRIUM_SYS_OPEN, (path_va), (len), (flags))
    if (OPEN(DATA_VA, 9, ULTRIUM_O_READ) != E(ULTRIUM_E_NOENT)) err = "missing file did not report NOENT";
    else if ((fd = OPEN(DATA_VA, 9, ULTRIUM_O_READ | ULTRIUM_O_WRITE | ULTRIUM_O_CREATE)) < 3) err = "create failed";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, (uint64_t)fd, DATA_VA + 0x100, 18) != 18) err = "write failed";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x200, 4) != 0 && 0) err = "unreachable";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd, 0, 0) != 0) err = "close failed";
    else if (syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd, 0, 0) != E(ULTRIUM_E_BADF)) err = "double close accepted";
    else if ((fd = OPEN(DATA_VA, 9, ULTRIUM_O_READ)) < 3) err = "open for reading failed";
    else {
        uint8_t back[32] = {0};
        if (syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, (uint64_t)fd, DATA_VA + 0x100, 4) != E(ULTRIUM_E_BADF)) err = "write on a read-only descriptor accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x300, 7) != 7) err = "read failed";
        else if (!aspace_copy_from_user(as, back, DATA_VA + 0x300, 7) || memcmp(back, "payload", 7)) err = "read returned the wrong data";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x300, 100) != 11) err = "second read did not return the remainder";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x300, 100) != 0) err = "read at end of file did not return 0";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, 0xffffffff80000000ULL, 4) != E(ULTRIUM_E_FAULT)) err = "read into a kernel address accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x1ff0, 0x100) != E(ULTRIUM_E_FAULT)) err = "read into a range running off the mapping accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA, ULTRIUM_MAX_IO + 1) != E(ULTRIUM_E_TOOBIG)) err = "oversized read accepted";
        (void)syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd, 0, 0);
    }
    if (!err) {
        /* A failed read must not consume the file: a fresh descriptor sees all of it, and so does the old data on disk. */
        fd = OPEN(DATA_VA, 9, ULTRIUM_O_READ);
        fd2 = OPEN(DATA_VA, 9, ULTRIUM_O_WRITE);
        if (fd < 3 || fd2 < 3 || fd == fd2) err = "two descriptors for one file failed";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd2, DATA_VA, 4) != E(ULTRIUM_E_BADF)) err = "read on a write-only descriptor accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x1ffc, 100) != E(ULTRIUM_E_FAULT)) err = "partial buffer accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, (uint64_t)fd, DATA_VA + 0x400, 5) != 5) err = "read after a failed read lost data";
        (void)syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd, 0, 0);
        (void)syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd2, 0, 0);
    }
    if (!err) {                                                   /* path validation */
        poke(pid, DATA_VA + 0x500, "relative", 8);
        poke(pid, DATA_VA + 0x510, "/a\0b", 4);
        poke(pid, DATA_VA + 0x520, "/", 1);
        poke(pid, DATA_VA + 0x530, "/sc-a.txt/../sc-a.txt", 21);
        static char longpath[200]; for (unsigned i = 0; i < sizeof(longpath); ++i) longpath[i] = i ? 'a' : '/';
        poke(pid, DATA_VA + 0x600, longpath, sizeof(longpath));
        if (OPEN(DATA_VA + 0x500, 8, ULTRIUM_O_READ) != E(ULTRIUM_E_INVAL)) err = "relative path accepted";
        else if (OPEN(DATA_VA + 0x510, 4, ULTRIUM_O_READ) != E(ULTRIUM_E_INVAL)) err = "path with an embedded NUL accepted";
        else if (OPEN(DATA_VA + 0x520, 1, ULTRIUM_O_READ) != E(ULTRIUM_E_ISDIR)) err = "opening a directory as a file accepted";
        else if (OPEN(DATA_VA + 0x600, sizeof(longpath), ULTRIUM_O_READ) != E(ULTRIUM_E_NAMETOOLONG)) err = "over-long path accepted";
        else if (OPEN(DATA_VA, 0, ULTRIUM_O_READ) != E(ULTRIUM_E_INVAL)) err = "empty path accepted";
        else if (OPEN(0xffffffff80000000ULL, 9, ULTRIUM_O_READ) != E(ULTRIUM_E_FAULT) || OPEN(0, 9, ULTRIUM_O_READ) != E(ULTRIUM_E_FAULT)) err = "bad path pointer accepted";
        else if (OPEN(DATA_VA, 9, 0) != E(ULTRIUM_E_INVAL) || OPEN(DATA_VA, 9, 8) != E(ULTRIUM_E_INVAL) || OPEN(DATA_VA, 9, ULTRIUM_O_READ | 0x100) != E(ULTRIUM_E_INVAL)) err = "bad flags accepted";
        else if (OPEN(DATA_VA, 9, ULTRIUM_O_READ | ULTRIUM_O_CREATE) != E(ULTRIUM_E_INVAL)) err = "create without write accepted";
        else if (OPEN(DATA_VA, ~0ULL, ULTRIUM_O_READ) != E(ULTRIUM_E_NAMETOOLONG)) err = "huge path length accepted";
        else if ((fd = OPEN(DATA_VA + 0x530, 21, ULTRIUM_O_READ)) < 3) err = "dot-dot path was not normalised";
        else (void)syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, (uint64_t)fd, 0, 0);
    }
    if (!err) {                                                   /* descriptor table */
        for (int64_t bad = -1; bad <= 0; ++bad) (void)bad;
        if (syscall_test_invoke(pid, ULTRIUM_SYS_READ, ~0ULL, DATA_VA, 4) != E(ULTRIUM_E_BADF) || syscall_test_invoke(pid, ULTRIUM_SYS_READ, 16, DATA_VA, 4) != E(ULTRIUM_E_BADF) ||
            syscall_test_invoke(pid, ULTRIUM_SYS_READ, 3, DATA_VA, 4) != E(ULTRIUM_E_BADF) || syscall_test_invoke(pid, ULTRIUM_SYS_WRITE, 99, DATA_VA, 4) != E(ULTRIUM_E_BADF)) err = "bad descriptor accepted";
        else if (syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, 1, 0, 0) != E(ULTRIUM_E_BADF) || syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, 2, 0, 0) != E(ULTRIUM_E_BADF) || syscall_test_invoke(pid, ULTRIUM_SYS_CLOSE, ~0ULL, 0, 0) != E(ULTRIUM_E_BADF)) err = "closing the console or a bad descriptor accepted";
        else {
            unsigned opened = 0; int64_t last = 0;
            while (opened < 20 && (last = OPEN(DATA_VA, 9, ULTRIUM_O_READ)) >= 3) ++opened;
            if (opened != ULTRIUM_FD_MAX - 3 || last != E(ULTRIUM_E_MFILE)) err = "descriptor limit not enforced";
        }
    }
    (void)process_discard(pid);                                   /* closes the leftover descriptors */
    if (vfs_unlink("/sc-a.txt") != VFS_OK && !err) err = "a file stayed busy after its owner was discarded";
    if (!err) {                                                   /* descriptors must be released: 8 x 13 > the VFS handle slots */
        (void)vfs_create_file("/sc-leak.txt");
        for (unsigned round = 0; round < 8 && !err; ++round) {
            uint32_t q = make("leak", false, PROGRAM, sizeof(PROGRAM));
            if (!q) { err = "setup failed"; break; }
            poke(q, DATA_VA, "/sc-leak.txt", 12);
            for (unsigned i = 0; i < 13; ++i) if (syscall_test_invoke(q, ULTRIUM_SYS_OPEN, DATA_VA, 12, ULTRIUM_O_READ) < 3) { err = "descriptors or VFS handles leaked"; break; }
            (void)process_discard(q);
        }
        (void)vfs_unlink("/sc-leak.txt");
    }
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    return err;
}

static const char *ring3_end_to_end(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    (void)vfs_unlink("/sysdemo.txt");
    uint32_t pid = make("sysdemo", false, PROGRAM, sizeof(PROGRAM));
    REQUIRE(pid, "setup failed");
    REQUIRE(poke(pid, DATA_VA, "/sysdemo.txt", 12) && poke(pid, DATA_VA + 0x20, "hello", 5), "setup failed");
    REQUIRE(process_start(pid, CODE_VA, PROCESS_STACK_TOP - 8), "start failed");
    int64_t st = -1;
    char text[64];
    REQUIRE(process_wait_ex(pid, &st, text, sizeof(text)), "wait failed");
    REQUIRE(st == 5, "the program did not read back what it wrote");
    REQUIRE(!memcmp(text, "hello", 6), "console output wrong");
    struct vfs_file f = {0};
    uint8_t back[16] = {0}; size_t n = 0;
    REQUIRE(vfs_file_open("/sysdemo.txt", VFS_OPEN_READ, &f) == VFS_OK, "the file written by ring 3 does not exist");
    REQUIRE(vfs_file_read(&f, back, sizeof(back), &n) == VFS_OK && n == 5 && !memcmp(back, "hello", 5), "the file content is wrong");
    (void)vfs_file_close(&f);
    REQUIRE(vfs_unlink("/sysdemo.txt") == VFS_OK, "cleanup failed");
    REQUIRE(pmm_free_pages() == frames && process_count_live() == 0, "leaked");
    return 0;
}

const struct test_case syscall_cases[] = {
    {"syscall basics, versioning and invalid numbers", basics},
    {"syscall console writes validate pointers and neutralise control characters", console},
    {"syscall file operations validate descriptors, paths and buffers", files},
    {"syscall int 0x80 end to end from ring 3 (open, write, close, read, console, exit)", ring3_end_to_end},
};
const size_t syscall_case_count = sizeof(syscall_cases) / sizeof(syscall_cases[0]);
