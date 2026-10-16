#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "scheduler/task.h"
#include "memory/pmm.h"

/* Context-switching tests (docs/process-model.md). Tasks here are switched by hand with
   task_switch_to: the order is fully deterministic, so a corrupted register, stack or
   alignment shows up as a specific failure rather than a timing-dependent one. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)
#define WORKERS 3U
#define ROUNDS 40U

static struct task *controller;
static struct task *workers[WORKERS];
static volatile uint64_t mismatch[WORKERS];
static volatile uint32_t rounds_done[WORKERS];
static volatile uint32_t bad_alignment[WORKERS];
static volatile uint32_t bad_stack_range[WORKERS];
static volatile bool finished[WORKERS];

static uint64_t irq_save(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static void irq_restore(uint64_t f) { if (f & (1ULL << 9)) __asm__ volatile("sti" ::: "memory"); }

/* uint64_t ultrium_test_roundtrip(uint64_t seed, void (*yield)(void)): load rbx, rbp and r12-r15
   with seed-derived values, call yield() (which switches away and comes back later) and return
   a bit per register that did not survive. */
__asm__(
".global ultrium_test_roundtrip\n"
"ultrium_test_roundtrip:\n"
"pushq %rbp\n" "pushq %rbx\n" "pushq %r12\n" "pushq %r13\n" "pushq %r14\n" "pushq %r15\n"
"subq $8, %rsp\n"
"leaq 1(%rdi), %rbx\n" "leaq 2(%rdi), %rbp\n" "leaq 3(%rdi), %r12\n"
"leaq 4(%rdi), %r13\n" "leaq 5(%rdi), %r14\n" "leaq 6(%rdi), %r15\n"
"movq %rdi, (%rsp)\n"
"movq %rsi, %rax\n"
"call *%rax\n"
"movq (%rsp), %rcx\n"
"xorl %eax, %eax\n"
"leaq 1(%rcx), %rdx\n" "cmpq %rdx, %rbx\n" "je 0f\n" "orl $1, %eax\n" "0:\n"
"leaq 2(%rcx), %rdx\n" "cmpq %rdx, %rbp\n" "je 1f\n" "orl $2, %eax\n" "1:\n"
"leaq 3(%rcx), %rdx\n" "cmpq %rdx, %r12\n" "je 2f\n" "orl $4, %eax\n" "2:\n"
"leaq 4(%rcx), %rdx\n" "cmpq %rdx, %r13\n" "je 3f\n" "orl $8, %eax\n" "3:\n"
"leaq 5(%rcx), %rdx\n" "cmpq %rdx, %r14\n" "je 4f\n" "orl $16, %eax\n" "4:\n"
"leaq 6(%rcx), %rdx\n" "cmpq %rdx, %r15\n" "je 5f\n" "orl $32, %eax\n" "5:\n"
"addq $8, %rsp\n"
"popq %r15\n" "popq %r14\n" "popq %r13\n" "popq %r12\n" "popq %rbx\n" "popq %rbp\n"
"retq\n");
extern uint64_t ultrium_test_roundtrip(uint64_t seed, void (*yield)(void));

static void yield_to_controller(void) {
    uint64_t f = irq_save();
    task_switch_to(controller);
    irq_restore(f);
}

static void worker(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    uint8_t local[1024];
    uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    if (frame & 15) ++bad_alignment[id];
    for (unsigned r = 0; r < ROUNDS; ++r) {
        for (unsigned i = 0; i < sizeof(local); ++i) local[i] = (uint8_t)(id * 59 + r * 7 + i);
        uint64_t seed = 0x1000000000000000ULL * (id + 1) + r * 0x100;
        mismatch[id] |= ultrium_test_roundtrip(seed, yield_to_controller);
        for (unsigned i = 0; i < sizeof(local); ++i)
            if (local[i] != (uint8_t)(id * 59 + r * 7 + i)) { mismatch[id] |= 64; break; }
        uintptr_t here = (uintptr_t)__builtin_frame_address(0);
        if (here < (uintptr_t)workers[id]->stack_base || here >= workers[id]->stack_top) ++bad_stack_range[id];
        ++rounds_done[id];
    }
    finished[id] = true;
    for (;;) yield_to_controller();                 /* the controller destroys the task; it never returns here */
}

static const char *switching(void) {
    controller = task_current();
    uint64_t pages = pmm_free_pages();
    for (unsigned i = 0; i < WORKERS; ++i) {
        mismatch[i] = rounds_done[i] = bad_alignment[i] = bad_stack_range[i] = 0; finished[i] = false;
        workers[i] = task_create("worker", worker, (void *)(uintptr_t)i, TASK_FLAG_KERNEL);
        if (!workers[i]) return "task creation failed";
    }
    REQUIRE(workers[0]->stack_base != workers[1]->stack_base && workers[1]->stack_base != workers[2]->stack_base, "tasks share a kernel stack");
    REQUIRE(workers[0]->saved_rsp != workers[1]->saved_rsp, "tasks share saved state");
    for (unsigned round = 0; round <= ROUNDS; ++round) {
        for (unsigned i = 0; i < WORKERS; ++i) {
            if (finished[i]) continue;
            workers[i]->state = TASK_RUNNING;
            uint64_t f = irq_save();
            task_switch_to(workers[i]);
            irq_restore(f);
        }
    }
    const char *result = 0;
    for (unsigned i = 0; i < WORKERS; ++i) {
        if (!finished[i] || rounds_done[i] != ROUNDS) result = "a task did not complete all rounds";
        else if (mismatch[i] & 64) result = "a task's stack contents changed across switches";
        else if (mismatch[i]) result = "a callee-saved register changed across a switch";
        else if (bad_alignment[i]) result = "a task entry has a misaligned stack";
        else if (bad_stack_range[i]) result = "a task ran outside its own kernel stack";
        else if (!task_stack_ok(workers[i])) result = "stack canary overwritten";
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        workers[i]->state = TASK_TERMINATED;
        if (!task_destroy(workers[i]) && !result) result = "task destroy failed";
    }
    if (!result && pmm_free_pages() != pages) result = "kernel stacks leaked frames";
    return result;
}

static const char *lifecycle(void) {
    uint64_t pages = pmm_free_pages();
    unsigned live = task_count_live();
    struct task *t[TASK_MAX];
    unsigned made = 0;
    while (made < TASK_MAX && (t[made] = task_create("fill", worker, 0, TASK_FLAG_KERNEL))) ++made;
    REQUIRE(made == TASK_MAX - live, "task table capacity is wrong");
    REQUIRE(task_create("one-too-many", worker, 0, TASK_FLAG_KERNEL) == NULL, "task table exhaustion was not reported");
    for (unsigned i = 0; i < made; ++i) REQUIRE(t[i]->state == TASK_CREATED && t[i]->tid, "new task has wrong state");
    for (unsigned i = 0; i < made; ++i) for (unsigned j = i + 1; j < made; ++j) REQUIRE(t[i]->tid != t[j]->tid, "duplicate task id");
    for (unsigned i = 0; i < made; ++i) REQUIRE(task_destroy(t[i]), "destroy of an idle task failed");
    REQUIRE(task_count_live() == live && pmm_free_pages() == pages, "task slots or frames leaked");
    for (unsigned round = 0; round < 64; ++round) {         /* repeated create/destroy reuses slots without leaking */
        struct task *x = task_create("again", worker, 0, TASK_FLAG_KERNEL);
        REQUIRE(x && task_destroy(x), "repeated create/destroy failed");
    }
    REQUIRE(pmm_free_pages() == pages, "repeated create/destroy leaked frames");
    return 0;
}

static const char *invalid_use(void) {
    REQUIRE(task_create("x", NULL, 0, 0) == NULL, "task with no entry accepted");
    REQUIRE(!task_destroy(NULL), "destroy(NULL) accepted");
    REQUIRE(!task_destroy(task_current()), "destroying the running task was accepted");
    REQUIRE(task_by_tid(0) == NULL && task_by_tid(0xfffffff0U) == NULL, "invalid tid resolved");
    task_switch_to(NULL);                                    /* no-op */
    task_switch_to(task_current());                          /* switch to self: no-op */
    REQUIRE(task_current() && task_current()->tid == 1, "boot task identity changed");
    struct task *t = task_create("rsp", worker, 0, TASK_FLAG_KERNEL);
    REQUIRE(t && (t->saved_rsp & 7) == 0 && t->saved_rsp > (uint64_t)(uintptr_t)t->stack_base && t->saved_rsp < t->stack_top, "initial frame outside its stack");
    REQUIRE(task_destroy(t), "cleanup failed");
    return 0;
}

const struct test_case task_cases[] = {
    {"task context switch preserves registers, stacks and alignment", switching},
    {"task create/destroy lifecycle and exhaustion", lifecycle},
    {"task invalid use is rejected", invalid_use},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
