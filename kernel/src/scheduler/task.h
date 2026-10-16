#ifndef ULTRIUM_TASK_H
#define ULTRIUM_TASK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kernel tasks and x86-64 context switching (docs/process-model.md).
   Single CPU. This layer owns task slots, kernel stacks and the register-level
   switch; queues, time slices and blocking live in scheduler.h. */

#define TASK_MAX 16U
#define TASK_KSTACK_PAGES 4U
#define TASK_NAME_MAX 16U

enum task_state { TASK_UNUSED = 0, TASK_CREATED, TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_TERMINATED };

#define TASK_FLAG_KERNEL      1U   /* never enters ring 3 */
#define TASK_FLAG_PREEMPTIBLE 2U   /* a kernel task the timer may preempt (touches no shared kernel state) */
#define TASK_FLAG_IMMORTAL    4U   /* boot and idle tasks cannot be terminated */

struct process;
struct aspace;
struct task {
    /* Offset 0 is read by the assembly switch routine. */
    uint64_t saved_rsp;                  /* kernel stack pointer while not running */
    uint32_t tid;
    enum task_state state;
    uint32_t flags;
    char name[TASK_NAME_MAX];
    uint8_t *stack_base;                 /* NULL for the boot task (Limine's stack) */
    uint64_t stack_top;
    void (*entry)(void *);
    void *arg;
    struct process *process;
    struct aspace *aspace;               /* NULL = the kernel's tables */
    uint32_t slice_left;
    uint32_t block_reason;
    uint64_t wake_tick;                  /* deadline for SCHED_BLOCK_SLEEP */
    bool wake_pending;                   /* a wake arrived before the block */
    uint64_t switches_in;
    uint8_t fxsave[512] __attribute__((aligned(16)));   /* user tasks only (see process-model.md) */
    bool uses_fpu;
};

/* Adopt the code that is running now (the boot/shell task) as task 1. */
bool task_system_init(void);
bool task_system_ready(void);
struct task *task_current(void);
struct task *task_by_tid(uint32_t tid);
struct task *task_first(void);               /* iteration over used slots */
struct task *task_next(const struct task *task);
/* Allocate a task and its kernel stack. State CREATED; it has not run. */
struct task *task_create(const char *name, void (*entry)(void *), void *arg, uint32_t flags);
/* Release a task that is not running (kernel stack + slot). */
bool task_destroy(struct task *task);
/* Low-level switch: save the current task's context, run `next`. Returns when
   the current task is switched back in. The caller has already updated both
   task states and must have interrupts disabled. */
void task_switch_to(struct task *next);
/* Called by a task that has just been switched in (including the first time). */
void task_finish_switch(void);
bool task_stack_ok(const struct task *task);
unsigned task_count_live(void);
/* Kernel-stack allocations held by live tasks (each is TASK_KSTACK_PAGES pages of the VMM region). */
unsigned task_stack_allocations(void);

/* Hook used by the scheduler to learn when a task entry function returns. */
void task_set_exit_handler(void (*handler)(void));
/* Called on the incoming task's stack after every completed switch (reaper hook). */
void task_set_post_switch_hook(void (*hook)(void));
#endif
