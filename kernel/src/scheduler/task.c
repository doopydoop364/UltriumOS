#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scheduler/task.h"
#include "memory/vmm.h"
#include "system/exceptions.h"
#include "memory/aspace.h"

#define STACK_CANARY 0x554c5452534b5354ULL   /* "ULTRSKST" */

static struct task tasks[TASK_MAX];
static struct task *current;
static uint32_t next_tid = 1;
static bool ready;
static void (*exit_handler)(void);
static void (*post_switch)(void);

/* ultrium_switch(uint64_t *save_rsp, uint64_t new_rsp): the callee-saved registers,
   rflags and the return address live on the stacks; everything else is saved by the
   C calling convention of the caller or by the interrupt entry stubs. */
__asm__(
".global ultrium_switch\n"
"ultrium_switch:\n"
"pushq %rbp\n" "pushq %rbx\n" "pushq %r12\n" "pushq %r13\n" "pushq %r14\n" "pushq %r15\n"
"pushfq\n"
"movq %rsp, (%rdi)\n"
"movq %rsi, %rsp\n"
"popfq\n"
"popq %r15\n" "popq %r14\n" "popq %r13\n" "popq %r12\n" "popq %rbx\n" "popq %rbp\n"
"retq\n"
/* First instruction of a new task: finish the switch, then run entry(arg). rsp is
   aligned here so the C call below has the ABI alignment. */
".global ultrium_task_trampoline\n"
"ultrium_task_trampoline:\n"
"andq $-16, %rsp\n"
"call ultrium_task_main\n"
"ud2\n");
extern void ultrium_switch(uint64_t *save_rsp, uint64_t new_rsp);
extern void ultrium_task_trampoline(void);

__attribute__((noreturn)) void ultrium_task_main(void) {
    task_finish_switch();
    struct task *self = current;
    __asm__ volatile("sti" ::: "memory");   /* new tasks start with interrupts enabled */
    if (self->entry) self->entry(self->arg);
    __asm__ volatile("cli" ::: "memory");
    if (exit_handler) exit_handler();
    for (;;) __asm__ volatile("hlt");
}

void task_set_exit_handler(void (*handler)(void)) { exit_handler = handler; }
void task_set_post_switch_hook(void (*hook)(void)) { post_switch = hook; }

static void copy_name(char *out, const char *name) {
    size_t i = 0;
    if (name) for (; name[i] && i + 1 < TASK_NAME_MAX; ++i) out[i] = name[i];
    out[i] = 0;
}

bool task_system_init(void) {
    if (ready) return false;
    struct task *boot = &tasks[0];
    *boot = (struct task){0};
    boot->tid = next_tid++;
    boot->state = TASK_RUNNING;
    boot->flags = TASK_FLAG_KERNEL | TASK_FLAG_IMMORTAL;
    copy_name(boot->name, "boot");
    current = boot;
    ready = true;
    return true;
}
bool task_system_ready(void) { return ready; }
struct task *task_current(void) { return current; }
struct task *task_by_tid(uint32_t tid) {
    for (unsigned i = 0; i < TASK_MAX; ++i)
        if (tasks[i].state != TASK_UNUSED && tasks[i].tid == tid) return &tasks[i];
    return NULL;
}
struct task *task_first(void) { return task_next(NULL); }
struct task *task_next(const struct task *after) {
    unsigned i = after ? (unsigned)(after - tasks) + 1 : 0;
    for (; i < TASK_MAX; ++i) if (tasks[i].state != TASK_UNUSED) return &tasks[i];
    return NULL;
}
unsigned task_count_live(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < TASK_MAX; ++i) if (tasks[i].state != TASK_UNUSED) ++n;
    return n;
}

unsigned task_stack_allocations(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < TASK_MAX; ++i) if (tasks[i].state != TASK_UNUSED && tasks[i].stack_base) ++n;
    return n;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t flags) {
    if (!ready || !entry || next_tid == UINT32_MAX) return NULL;
    struct task *slot = NULL;
    for (unsigned i = 0; i < TASK_MAX; ++i) if (tasks[i].state == TASK_UNUSED) { slot = &tasks[i]; break; }
    if (!slot) return NULL;
    uint8_t *stack = vmm_alloc_pages(TASK_KSTACK_PAGES);
    if (!stack) return NULL;
    *slot = (struct task){0};
    slot->tid = next_tid++;
    slot->flags = flags;
    copy_name(slot->name, name);
    slot->stack_base = stack;
    slot->stack_top = (uint64_t)(uintptr_t)stack + (uint64_t)TASK_KSTACK_PAGES * 4096U;
    *(uint64_t *)stack = STACK_CANARY;
    slot->entry = entry; slot->arg = arg;
    /* Initial frame, popped by ultrium_switch: rflags, r15..rbp, return address. */
    uint64_t *frame = (uint64_t *)(uintptr_t)(slot->stack_top - 9 * sizeof(uint64_t));
    for (unsigned i = 0; i < 9; ++i) frame[i] = 0;
    frame[0] = 0x2;                                   /* rflags: reserved bit only, interrupts off until the task enables them */
    frame[7] = (uint64_t)(uintptr_t)ultrium_task_trampoline;
    /* frame[8] is padding so rsp at the trampoline is 16-byte aligned after the ret. */
    slot->saved_rsp = (uint64_t)(uintptr_t)frame;
    slot->state = TASK_CREATED;
    return slot;
}

bool task_stack_ok(const struct task *t) {
    return !t->stack_base || *(const uint64_t *)t->stack_base == STACK_CANARY;
}

uint64_t task_stack_high_water(const struct task *t) {
    if (!t || !t->stack_base) return 0;
    uint64_t size = (uint64_t)TASK_KSTACK_PAGES * 4096U, i = 8;           /* skip the canary word */
    while (i < size && !t->stack_base[i]) ++i;
    return size - i;
}

bool task_destroy(struct task *t) {
    if (!t || t == current || t->state == TASK_UNUSED || t->state == TASK_RUNNING || (t->flags & TASK_FLAG_IMMORTAL)) return false;
    if (t->stack_base && !vmm_free_pages(t->stack_base)) return false;
    *t = (struct task){0};
    return true;
}

void task_finish_switch(void) {
    /* Runs on the incoming task's stack. */
    if (current->stack_top) exceptions_set_kernel_stack(current->stack_top);
    ++current->switches_in;
    if (post_switch) post_switch();
}

void task_switch_to(struct task *next) {
    struct task *prev = current;
    if (!next || next == prev) return;
    if (!task_stack_ok(prev) || !task_stack_ok(next)) {
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");              /* kernel stack overflow: unrecoverable */
    }
    if (prev->uses_fpu) __asm__ volatile("fxsave %0" : "=m"(prev->fxsave));        /* eager SIMD/x87 state switch, user tasks only */
    if (next->aspace != aspace_active()) {
        if (next->aspace) aspace_activate(next->aspace); else aspace_activate_kernel();
    }
    current = next;
    if (next->stack_top) exceptions_set_kernel_stack(next->stack_top);
    if (next->uses_fpu) __asm__ volatile("fxrstor %0" : : "m"(next->fxsave));
    ultrium_switch(&prev->saved_rsp, next->saved_rsp);
    /* Back in `prev`, switched in again. */
    ++prev->switches_in;
    if (post_switch) post_switch();
}
