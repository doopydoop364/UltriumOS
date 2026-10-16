#ifndef ULTRIUM_SCHEDULER_H
#define ULTRIUM_SCHEDULER_H
#include <stdbool.h>
#include <stdint.h>
#include "scheduler/task.h"

/* Round-robin scheduler for one CPU (docs/process-model.md, "Scheduling").
   Queues are changed only with interrupts disabled. Kernel code is not preemptible:
   the timer switches tasks only when it interrupted ring 3 or a task flagged
   TASK_FLAG_PREEMPTIBLE. This is NOT SMP-safe. */

#define SCHED_SLICE_TICKS 5U          /* 50 ms at 100 Hz */
#define SCHED_BLOCK_EVENT 1U          /* waits for sched_wake */
#define SCHED_BLOCK_SLEEP 2U          /* waits for a tick deadline */
#define SCHED_BLOCK_CHILD 3U          /* waits for a child process (process layer) */

bool sched_init(void);                /* creates the idle task and hooks timer, exit and reaper paths */
bool sched_ready(void);
struct task *sched_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t flags);
bool sched_make_ready(struct task *task);   /* CREATED -> READY */
void sched_yield(void);               /* give the CPU to the next READY task (no-op when none) */
void sched_block(uint32_t reason);    /* RUNNING -> BLOCKED; returns after sched_wake (immediately if a wake was pending) */
void sched_sleep(uint32_t ticks);     /* block until `ticks` timer ticks have passed (other tasks or idle run meanwhile) */
bool sched_wake(struct task *task);   /* BLOCKED -> READY; a wake for a running task is remembered */
__attribute__((noreturn)) void sched_exit(void);
bool sched_terminate(struct task *task);    /* not the caller, not immortal; the caller reaps via the reaper */
/* Timer entry, called from the IRQ handler after EOI. `from_user`: the interrupt arrived in ring 3. */
void sched_tick(bool from_user);
/* Wait for something to happen: run another task if one is READY, otherwise halt until an interrupt. */
void sched_idle_wait(void);
/* Optional hook called for each terminated task before its resources are released. */
void sched_set_reap_hook(void (*hook)(struct task *task));

struct sched_stats {
    uint64_t ticks, switches, preemptions, yields, blocks, wakes, reaped, invalid_transitions;
    unsigned ready, blocked;
    uint32_t idle_tid;
};
void sched_stats_get(struct sched_stats *stats);
unsigned sched_ready_count(void);
bool sched_queue_consistent(void);
#endif
