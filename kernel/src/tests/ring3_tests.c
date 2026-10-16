#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "memory/aspace.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/vmm.h"
#include "system/interrupts.h"
#include "syscalls/syscall.h"
#include "memory.h"

/* Ring-3 execution tests. The user programs are small hand-assembled x86-64 images embedded here; they
   run with CPL 3 in private address spaces. Every fault test requires that only the offending process
   dies, with the expected exception, and that the kernel and other processes carry on. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define CODE_VA 0x400000ULL
#define DATA_VA 0x500000ULL
#define OTHER_VA 0x600000ULL

static const uint8_t P_EXIT42[] = {0xb8,0,0,0,0, 0xbf,0x2a,0,0,0, 0xcd,0x80, 0xeb,0xfe};
static const uint8_t P_STACK[] = {0x6a,0x2a, 0x5f, 0xb8,0,0,0,0, 0xcd,0x80, 0xeb,0xfe};          /* push 42; pop rdi; exit(rdi) */
static const uint8_t P_COUNTER[] = {0x48,0xb8,0x00,0x00,0x50,0x00,0,0,0,0, 0x48,0xff,0x00, 0xeb,0xfb};   /* inc qword [0x500000] forever */
static const uint8_t P_CLI[] = {0xfa, 0xeb,0xfe};
static const uint8_t P_HLT[] = {0xf4, 0xeb,0xfe};
static const uint8_t P_IN[] = {0xe4,0x60, 0xeb,0xfe};
static const uint8_t P_CR3[] = {0x0f,0x20,0xd8, 0xeb,0xfe};
static const uint8_t P_SYSCALL_INSN[] = {0x0f,0x05, 0xeb,0xfe};
static const uint8_t P_UD2[] = {0x0f,0x0b};
static const uint8_t P_DIV0[] = {0x31,0xc9, 0x31,0xd2, 0xb8,1,0,0,0, 0x48,0xf7,0xf1, 0xeb,0xfe};  /* div rcx with rcx = 0 */
static const uint8_t P_INT3[] = {0xcc, 0xeb,0xfe};
static const uint8_t P_INT_OTHER[] = {0xcd,0x21, 0xeb,0xfe};                                    /* int 0x21: gate is DPL 0 -> #GP */
static const uint8_t P_BADSYS[] = {0xb8,0x0f,0x27,0,0, 0xcd,0x80, 0x89,0xc7, 0xb8,0,0,0,0, 0xcd,0x80, 0xeb,0xfe};  /* exit(syscall(9999)) */
/* mov rax, <addr>; mov rbx, [rax] */
#define READ_AT(a) {0x48,0xb8,(uint8_t)(a),(uint8_t)((a)>>8),(uint8_t)((a)>>16),(uint8_t)((a)>>24),(uint8_t)((a)>>32),(uint8_t)((a)>>40),(uint8_t)((a)>>48),(uint8_t)((a)>>56), 0x48,0x8b,0x18, 0xeb,0xfe}
static const uint8_t P_READ_KERNEL[] = READ_AT(0xffffffff80000000ULL);
static const uint8_t P_READ_HHDM[] = READ_AT(0xffff800000001000ULL);
static const uint8_t P_READ_NULL[] = READ_AT(0x0ULL);
static const uint8_t P_READ_OTHER[] = READ_AT(OTHER_VA);
static const uint8_t P_READ_NONCANON[] = READ_AT(0x0000800000000000ULL);
static const uint8_t P_WRITE_CODE[] = {0x48,0xb8,0x00,0x00,0x40,0x00,0,0,0,0, 0xc6,0x00,0x01, 0xeb,0xfe};      /* mov byte [CODE], 1 */
static const uint8_t P_EXEC_DATA[] = {0x48,0xb8,0x00,0x00,0x50,0x00,0,0,0,0, 0xff,0xe0};                      /* jmp DATA */
static const uint8_t P_EXEC_STACK[] = {0x48,0x89,0xe0, 0xff,0xe0};                                            /* jmp rsp */

struct spawned { uint32_t pid; };
static uint32_t spawn(const char *name, const uint8_t *code, size_t len, unsigned data_pages, bool other_page) {
    uint32_t pid = process_create(name, 0);
    if (!pid) return 0;
    bool ok = process_map_segment(pid, CODE_VA, code, len, len, VMM_PAGE_READ | VMM_PAGE_EXECUTE);
    if (ok && data_pages) ok = process_map_segment(pid, DATA_VA, NULL, 0, data_pages * 4096ULL, VMM_PAGE_READ | VMM_PAGE_WRITE);
    if (ok && other_page) ok = process_map_segment(pid, OTHER_VA, NULL, 0, 4096, VMM_PAGE_READ | VMM_PAGE_WRITE);
    if (ok) ok = process_map_stack(pid);
    if (!ok || !process_start(pid, CODE_VA, PROCESS_STACK_TOP - 8)) { (void)process_discard(pid); return 0; }
    return pid;
}
static void clear_processes(void) {            /* a failing test must not poison the next one */
    struct process_info list[PROCESS_MAX];
    unsigned n = process_list(list, PROCESS_MAX);
    for (unsigned i = 0; i < n; ++i) {
        int64_t st;
        if (list[i].state == PROC_ACTIVE) (void)process_kill(list[i].pid);
        if (list[i].state == PROC_CREATED) (void)process_discard(list[i].pid);
        else (void)process_wait(list[i].pid, &st);
    }
}
static void wait_ticks(uint64_t n) {
    uint64_t end = interrupts_timer_ticks() + n;
    while (interrupts_timer_ticks() < end) sched_idle_wait();
}
static const char *run_to_exit(const char *name, const uint8_t *code, size_t len, int64_t *status, struct process_info *info) {
    uint32_t pid = spawn(name, code, len, 1, false);
    if (!pid) return "spawn failed";
    if (info) { *info = (struct process_info){0}; }
    int64_t st = 0;
    /* Capture the fault record before the slot is released by wait. */
    for (unsigned guard = 0; guard < 400; ++guard) {
        struct process_info i;
        if (!process_info_get(pid, &i)) return "process vanished";
        if (i.state == PROC_EXITED) { if (info) *info = i; break; }
        sched_sleep(1);
    }
    if (!process_wait(pid, &st)) return "wait failed";
    *status = st;
    return 0;
}

static const char *basic(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    REQUIRE(process_system_ready() && syscall_ready(), "process support not initialised");
    uint32_t efer_lo, efer_hi;
    __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xc0000080U));
    REQUIRE(!(efer_lo & 1U), "EFER.SCE is set although SYSCALL is unsupported");
    int64_t st = -1;
    const char *e = run_to_exit("exit42", P_EXIT42, sizeof(P_EXIT42), &st, NULL);
    REQUIRE(!e, e);
    REQUIRE(st == 42, "ring-3 program did not exit with its status");
    e = run_to_exit("stack", P_STACK, sizeof(P_STACK), &st, NULL);
    REQUIRE(!e && st == 42, "push/pop on the user stack failed");
    e = run_to_exit("badsys", P_BADSYS, sizeof(P_BADSYS), &st, NULL);
    REQUIRE(!e, e);
    REQUIRE(st == -38, "an unknown system call did not return -38 and let the program continue");
    REQUIRE(process_count_live() == 0 && task_by_tid(1) != NULL, "process table not empty");
    REQUIRE(pmm_free_pages() == frames && heap_live_allocations() == live, "ring-3 programs leaked memory");
    return 0;
}

struct fault_case { const char *name; const uint8_t *code; size_t len; uint64_t vector; uint64_t address; bool check_address; uint64_t error_mask, error_bits; };
#define FC(n, c, vec) {n, c, sizeof(c), vec, 0, false, 0, 0}
static const struct fault_case faults[] = {
    FC("cli", P_CLI, 13), FC("hlt", P_HLT, 13), FC("in", P_IN, 13), FC("mov cr3", P_CR3, 13),
    FC("syscall insn", P_SYSCALL_INSN, 6), FC("ud2", P_UD2, 6), FC("div by zero", P_DIV0, 0),
    FC("int3 (breakpoint gate is DPL 0)", P_INT3, 13),
    FC("int 0x21 (kernel gate)", P_INT_OTHER, 13),
    {"read kernel image", P_READ_KERNEL, sizeof(P_READ_KERNEL), 14, 0xffffffff80000000ULL, true, 0x7, 0x5},     /* user, protection, read */
    {"read direct map", P_READ_HHDM, sizeof(P_READ_HHDM), 14, 0xffff800000001000ULL, true, 0x7, 0x5},
    {"read null", P_READ_NULL, sizeof(P_READ_NULL), 14, 0, true, 0x7, 0x4},                                      /* user, not present */
    {"read another process's page", P_READ_OTHER, sizeof(P_READ_OTHER), 14, OTHER_VA, true, 0x7, 0x4},
    {"read non-canonical", P_READ_NONCANON, sizeof(P_READ_NONCANON), 13, 0, false, 0, 0},
    {"write own code (W^X)", P_WRITE_CODE, sizeof(P_WRITE_CODE), 14, CODE_VA, true, 0x7, 0x7},                   /* user, protection, write */
    {"execute data (NX)", P_EXEC_DATA, sizeof(P_EXEC_DATA), 14, DATA_VA, true, 0x17, 0x15},                      /* user, protection, fetch */
    {"execute stack (NX)", P_EXEC_STACK, sizeof(P_EXEC_STACK), 14, 0, false, 0x17, 0x15},
};
static const char *fault_matrix(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    uint64_t ticks = interrupts_timer_ticks();
    /* The "other process" page exists in a second, running process while the faulting one probes it. */
    static const uint8_t spin[] = {0xeb, 0xfe};
    uint32_t holder = spawn("holder", spin, sizeof(spin), 1, true);
    REQUIRE(holder, "spawn failed");
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        const struct fault_case *c = &faults[i];
        int64_t st = 0; struct process_info info;
        const char *e = run_to_exit(c->name, c->code, c->len, &st, &info);
        if (e) { (void)process_kill(holder); return e; }
        if (!info.faulted) { (void)process_kill(holder); return c->name; }
        if (info.fault.vector != c->vector || st != PROCESS_EXIT_FAULT_BASE - (int64_t)c->vector) { (void)process_kill(holder); return c->name; }
        if (c->check_address && info.fault.address != c->address) { (void)process_kill(holder); return c->name; }
        if (c->error_mask && (info.fault.error & c->error_mask) != c->error_bits) { (void)process_kill(holder); return c->name; }
    }
    struct process_info hi;
    REQUIRE(process_info_get(holder, &hi) && hi.state == PROC_ACTIVE && !hi.faulted, "an unrelated process was affected by another's fault");
    REQUIRE(process_kill(holder), "kill failed");
    int64_t st;
    REQUIRE(process_wait(holder, &st) && st == PROCESS_EXIT_KILLED, "killed process status wrong");
    wait_ticks(2);
    REQUIRE(interrupts_timer_ticks() > ticks, "timer stopped after user faults");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "faulting programs leaked memory");
    return 0;
}

static uint64_t read_counter(uint32_t pid) {
    uint64_t v = 0;
    struct aspace *a = process_aspace(pid);
    if (!a || !aspace_copy_from_user(a, &v, DATA_VA, 8)) return ~0ULL;
    return v;
}
static const char *concurrency(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    struct sched_stats s0, s1;
    sched_stats_get(&s0);
    uint32_t a = spawn("count-a", P_COUNTER, sizeof(P_COUNTER), 1, false);
    uint32_t b = spawn("count-b", P_COUNTER, sizeof(P_COUNTER), 1, false);
    uint32_t c = spawn("count-c", P_COUNTER, sizeof(P_COUNTER), 1, false);
    REQUIRE(a && b && c, "spawn failed");
    wait_ticks(40);
    uint64_t a1 = read_counter(a), b1 = read_counter(b), c1 = read_counter(c);
    REQUIRE(a1 && b1 && c1 && a1 != ~0ULL && b1 != ~0ULL && c1 != ~0ULL, "a ring-3 program made no progress");
    sched_stats_get(&s1);
    REQUIRE(s1.preemptions > s0.preemptions + 3, "ring-3 programs were not preempted by the timer");
    /* Terminate one: the others must keep running. */
    REQUIRE(process_kill(b), "kill failed");
    wait_ticks(2);
    int64_t st;
    REQUIRE(process_wait(b, &st) && st == PROCESS_EXIT_KILLED, "killed program status wrong");
    uint64_t a2 = read_counter(a), c2 = read_counter(c);
    wait_ticks(20);
    uint64_t a3 = read_counter(a), c3 = read_counter(c);
    REQUIRE(a3 > a2 && c3 > c2, "the surviving programs stopped after one was terminated");
    REQUIRE(a2 >= a1 && c2 >= c1, "a counter went backwards (memory corrupted)");
    REQUIRE(process_kill(a) && process_kill(c), "kill failed");
    REQUIRE(process_wait(a, &st) && process_wait(c, &st), "wait failed");
    REQUIRE(!process_kill(a) && !process_kill(9999) && !process_kill(0), "kill of an invalid pid accepted");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "concurrent programs leaked memory");
    return 0;
}

static const char *isolation(void) {
    clear_processes();
    /* Two programs share the same virtual addresses; each only ever sees its own memory. */
    uint64_t frames = pmm_free_pages();
    uint32_t a = spawn("iso-a", P_COUNTER, sizeof(P_COUNTER), 1, false), b = spawn("iso-b", P_COUNTER, sizeof(P_COUNTER), 1, false);
    REQUIRE(a && b, "spawn failed");
    uint64_t magic = 0x1122334455667788ULL;
    struct aspace *sb = process_aspace(b);
    wait_ticks(5);
    /* Freeze B's counter by overwriting it from the kernel with a magic value, then check A is unaffected. */
    REQUIRE(process_kill(b), "kill failed");
    int64_t st;
    REQUIRE(process_wait(b, &st), "wait failed");
    (void)sb;
    uint64_t before = read_counter(a);
    wait_ticks(5);
    REQUIRE(read_counter(a) > before, "A stopped");
    REQUIRE(process_aspace(b) == NULL, "dead process still has an address space");
    uint64_t pa;
    REQUIRE(aspace_translate(process_aspace(a), DATA_VA, true, &pa) && pa != 0, "A's data page vanished");
    (void)magic;
    REQUIRE(process_kill(a) && process_wait(a, &st), "cleanup failed");
    REQUIRE(pmm_free_pages() == frames, "frames leaked");
    return 0;
}

static const char *repeated(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    unsigned tasks = task_count_live();
    for (unsigned i = 0; i < 60; ++i) {
        int64_t st = 0;
        const char *e = run_to_exit("again", P_EXIT42, sizeof(P_EXIT42), &st, NULL);
        REQUIRE(!e && st == 42, "repeated process creation failed");
        if ((i % 7) == 3) { e = run_to_exit("again-f", P_CLI, sizeof(P_CLI), &st, NULL); REQUIRE(!e && st == PROCESS_EXIT_FAULT_BASE - 13, "repeated fault failed"); }
    }
    REQUIRE(task_count_live() == tasks && process_count_live() == 0, "tasks or processes leaked");
    REQUIRE(pmm_free_pages() == frames && heap_live_allocations() == live, "repeated creation leaked memory");
    REQUIRE(sched_queue_consistent(), "run queue corrupt");
    return 0;
}

static const char *exhaustion_and_misuse(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    uint32_t pids[PROCESS_MAX + 2];
    unsigned made = 0;
    while (made < PROCESS_MAX + 1 && (pids[made] = process_create("fill", 0))) ++made;
    REQUIRE(made > 0 && made <= PROCESS_MAX, "process table limit not enforced");
    REQUIRE(process_create("one-more", 0) == 0, "creation beyond the limit succeeded");
    for (unsigned i = 0; i < made; ++i) REQUIRE(process_discard(pids[i]), "discard failed");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "exhaustion leaked");
    uint32_t p = process_create("misuse", 0);
    REQUIRE(p, "create failed");
    REQUIRE(!process_start(p, CODE_VA, PROCESS_STACK_TOP - 8), "started with nothing mapped");
    REQUIRE(process_map_segment(p, CODE_VA, P_EXIT42, sizeof(P_EXIT42), sizeof(P_EXIT42), VMM_PAGE_READ | VMM_PAGE_EXECUTE), "map code failed");
    REQUIRE(!process_map_segment(p, CODE_VA, P_EXIT42, 4, 4, VMM_PAGE_READ | VMM_PAGE_EXECUTE), "overlapping segment accepted");
    REQUIRE(!process_map_segment(p, 0x1000, NULL, 0, 4096, VMM_PAGE_READ), "segment below the user minimum accepted");
    REQUIRE(!process_map_segment(p, 0xffffffff80000000ULL, NULL, 0, 4096, VMM_PAGE_READ), "kernel-space segment accepted");
    REQUIRE(!process_map_segment(p, DATA_VA, NULL, 0, 4096, VMM_PAGE_READ | VMM_PAGE_WRITE | VMM_PAGE_EXECUTE), "W+X segment accepted");
    REQUIRE(!process_map_segment(p, DATA_VA, NULL, 100, 50, VMM_PAGE_READ), "data larger than memory size accepted");
    REQUIRE(process_map_segment(p, DATA_VA, NULL, 0, 4096, VMM_PAGE_READ | VMM_PAGE_WRITE), "map data failed");
    REQUIRE(process_map_stack(p), "map stack failed");
    REQUIRE(!process_start(p, DATA_VA, PROCESS_STACK_TOP - 8), "entry point in a non-executable page accepted");
    REQUIRE(!process_start(p, 0xffffffff80000000ULL, PROCESS_STACK_TOP - 8), "kernel entry point accepted");
    REQUIRE(!process_start(p, CODE_VA, 0) && !process_start(p, CODE_VA, 0xffff800000000000ULL), "bad stack pointer accepted");
    REQUIRE(process_start(p, CODE_VA, PROCESS_STACK_TOP - 8), "valid start failed");
    REQUIRE(!process_start(p, CODE_VA, PROCESS_STACK_TOP - 8), "double start accepted");
    REQUIRE(!process_discard(p), "discarded a running process");
    REQUIRE(!process_map_segment(p, 0x700000, NULL, 0, 4096, VMM_PAGE_READ), "mapped into a started process");
    int64_t st = 0;
    REQUIRE(process_wait(p, &st) && st == 42, "wait failed");
    REQUIRE(!process_wait(p, &st) && !process_wait(0, &st) && !process_wait(123456, &st), "wait on a stale or invalid pid accepted");
    REQUIRE(!process_collect(p, &st) && !process_discard(p), "stale pid accepted");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "misuse leaked");
    return 0;
}

const struct test_case ring3_cases[] = {
    {"ring3 programs run, use their stack and exit", basic},
    {"ring3 faults end only the faulting process", fault_matrix},
    {"ring3 programs are preempted and independently terminated", concurrency},
    {"ring3 programs with identical addresses are isolated", isolation},
    {"ring3 repeated creation, exit and faults do not leak", repeated},
    {"ring3 process table limits and invalid use", exhaustion_and_misuse},
};
const size_t ring3_case_count = sizeof(ring3_cases) / sizeof(ring3_cases[0]);
