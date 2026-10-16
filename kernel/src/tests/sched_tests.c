#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "scheduler/scheduler.h"
#include "system/interrupts.h"
#include "memory/pmm.h"

/* Scheduler tests (docs/process-model.md): round robin, timer preemption of tasks that never yield,
   yielding order, blocking/waking, sleeping with only the idle task runnable, exit and reaping,
   termination, and repeated create/exit. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)

static volatile uint64_t counters[8];
static volatile bool stop_flag;
static volatile uint32_t order_log[64];
static volatile uint32_t order_len;
static volatile uint32_t progress;

static void wait_ticks(uint64_t n) {
    uint64_t end = interrupts_timer_ticks() + n;
    while (interrupts_timer_ticks() < end) sched_idle_wait();
}
static void settle(void) {                       /* let terminated tasks be reaped */
    for (unsigned i = 0; i < 4; ++i) sched_yield();
    wait_ticks(1);
    for (unsigned i = 0; i < 4; ++i) sched_yield();
}
static void spinner(void *arg) {                 /* never yields: only the timer can take the CPU away */
    unsigned id = (unsigned)(uintptr_t)arg;
    while (!stop_flag) ++counters[id];
}
static const char *preempt(unsigned n, const char *fail_text) {
    struct sched_stats before, after;
    sched_stats_get(&before);
    uint64_t frames = pmm_free_pages();
    struct task *t[8];
    stop_flag = false;
    for (unsigned i = 0; i < n; ++i) counters[i] = 0;
    for (unsigned i = 0; i < n; ++i) {
        t[i] = sched_spawn("spin", spinner, (void *)(uintptr_t)i, TASK_FLAG_KERNEL | TASK_FLAG_PREEMPTIBLE);
        if (!t[i]) return "spawn failed";
    }
    wait_ticks(30 * n);
    uint64_t seen[8];
    for (unsigned i = 0; i < n; ++i) seen[i] = counters[i];
    stop_flag = true;
    uint32_t tids[8];
    for (unsigned i = 0; i < n; ++i) tids[i] = t[i]->tid;
    for (unsigned guard = 0; guard < 400; ++guard) {
        bool gone = true;
        for (unsigned i = 0; i < n; ++i) if (task_by_tid(tids[i])) gone = false;
        if (gone) break;
        wait_ticks(1);
        settle();
    }
    sched_stats_get(&after);
    for (unsigned i = 0; i < n; ++i) REQUIRE(seen[i] > 0, fail_text);
    REQUIRE(after.preemptions > before.preemptions, "the timer never preempted a running task");
    for (unsigned i = 0; i < n; ++i) REQUIRE(!task_by_tid(tids[i]), "spinner was not reaped");
    REQUIRE(pmm_free_pages() == frames, "spinner stacks leaked");
    REQUIRE(sched_queue_consistent(), "run queue corrupt");
    return 0;
}
static const char *two_tasks(void) { return preempt(2, "a runnable task starved"); }
static const char *many_tasks(void) { return preempt(6, "a runnable task starved"); }

static void yielder(void *arg) {
    unsigned id = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < 6; ++i) {
        if (order_len < 64) order_log[order_len++] = id;
        sched_yield();
    }
    ++progress;
}
static const char *yielding(void) {
    order_len = 0; progress = 0;
    struct task *t[3];
    for (unsigned i = 0; i < 3; ++i) if (!(t[i] = sched_spawn("yield", yielder, (void *)(uintptr_t)(i + 1), TASK_FLAG_KERNEL))) return "spawn failed";
    for (unsigned guard = 0; progress < 3 && guard < 500; ++guard) sched_yield();
    REQUIRE(progress == 3 && order_len == 18, "yielding tasks did not finish");
    for (unsigned i = 0; i < 18; ++i) REQUIRE(order_log[i] == (i % 3) + 1, "round-robin order violated");
    settle();
    REQUIRE(sched_queue_consistent(), "run queue corrupt");
    return 0;
}

static void blocker(void *arg) {
    (void)arg;
    counters[0] = 1;
    sched_block(SCHED_BLOCK_EVENT);
    counters[0] = 2;
    sched_block(SCHED_BLOCK_EVENT);               /* a wake delivered before this block must not be lost */
    counters[0] = 3;
}
static const char *block_wake(void) {
    counters[0] = 0;
    struct task *t = sched_spawn("block", blocker, NULL, TASK_FLAG_KERNEL);
    REQUIRE(t, "spawn failed");
    uint32_t tid = t->tid;
    for (unsigned guard = 0; counters[0] != 1 && guard < 200; ++guard) sched_yield();
    REQUIRE(counters[0] == 1, "task did not start");
    wait_ticks(3);
    REQUIRE(t->state == TASK_BLOCKED && counters[0] == 1, "a blocked task ran");
    REQUIRE(sched_wake(t), "wake of a blocked task failed");
    for (unsigned guard = 0; counters[0] != 2 && guard < 200; ++guard) sched_yield();
    REQUIRE(counters[0] == 2, "woken task did not run");
    /* Wake it while it is still running/ready... then it blocks: the pending wake makes block return at once. */
    for (unsigned guard = 0; t->state != TASK_BLOCKED && guard < 200; ++guard) sched_yield();
    REQUIRE(t->state == TASK_BLOCKED, "task did not block again");
    REQUIRE(sched_wake(t), "second wake failed");
    for (unsigned guard = 0; counters[0] != 3 && guard < 200; ++guard) sched_yield();
    REQUIRE(counters[0] == 3, "second wake lost");
    settle();
    REQUIRE(!task_by_tid(tid), "exited task not reaped");
    REQUIRE(!sched_wake(NULL), "wake(NULL) accepted");
    REQUIRE(sched_queue_consistent(), "run queue corrupt");
    return 0;
}

static void early_waker_target(void *arg) {
    (void)arg;
    sched_yield();
    sched_block(SCHED_BLOCK_EVENT);               /* wake_pending is already set: returns without blocking */
    counters[1] = 7;
}
static const char *wake_before_block(void) {
    counters[1] = 0;
    struct task *t = sched_spawn("early", early_waker_target, NULL, TASK_FLAG_KERNEL);
    REQUIRE(t, "spawn failed");
    REQUIRE(sched_wake(t), "wake of a READY task must be remembered");
    for (unsigned guard = 0; counters[1] != 7 && guard < 300; ++guard) sched_yield();
    REQUIRE(counters[1] == 7, "a wake delivered before the block was lost");
    settle();
    return 0;
}

static void sleeper(void *arg) { (void)arg; sched_sleep(8); ++progress; }
static const char *idle_and_sleep(void) {
    struct sched_stats a, b;
    progress = 0;
    sched_stats_get(&a);
    struct task *idle = task_by_tid(a.idle_tid);
    REQUIRE(idle, "no idle task");
    uint64_t idle_before = idle->switches_in, t0 = interrupts_timer_ticks();
    struct task *s = sched_spawn("sleeper", sleeper, NULL, TASK_FLAG_KERNEL);
    REQUIRE(s, "spawn failed");
    sched_sleep(15);                               /* both tasks asleep: only idle can run */
    uint64_t waited = interrupts_timer_ticks() - t0;
    REQUIRE(waited >= 15, "sleep returned early");
    REQUIRE(progress == 1, "the other sleeper did not finish while this task slept");
    REQUIRE(idle->switches_in > idle_before, "the idle task never ran");
    sched_stats_get(&b);
    REQUIRE(b.ticks > a.ticks, "no ticks while idle");
    settle();
    return 0;
}

static void exiter(void *arg) { ++counters[(unsigned)(uintptr_t)arg]; }
static void explicit_exiter(void *arg) { ++counters[(unsigned)(uintptr_t)arg]; sched_exit(); }
static const char *exit_and_cleanup(void) {
    uint64_t frames = pmm_free_pages();
    unsigned live = task_count_live();
    counters[2] = 0;
    for (unsigned i = 0; i < 100; ++i) {
        struct task *t = sched_spawn("exit", i & 1 ? exiter : explicit_exiter, (void *)(uintptr_t)2, TASK_FLAG_KERNEL);
        REQUIRE(t, "spawn failed during repeated creation");
        for (unsigned guard = 0; task_by_tid(t->tid) && guard < 100; ++guard) sched_yield();
        settle();
    }
    REQUIRE(counters[2] == 100, "not every task ran");
    REQUIRE(task_count_live() == live, "task slots leaked");
    REQUIRE(pmm_free_pages() == frames, "frames leaked across repeated creation");
    return 0;
}

static void parked(void *arg) { (void)arg; sched_block(SCHED_BLOCK_EVENT); counters[3] = 99; }
static const char *termination(void) {
    uint64_t frames = pmm_free_pages();
    counters[3] = 0;
    struct task *blocked = sched_spawn("parked", parked, NULL, TASK_FLAG_KERNEL);
    struct task *ready = sched_spawn("ready", parked, NULL, TASK_FLAG_KERNEL);
    REQUIRE(blocked && ready, "spawn failed");
    REQUIRE(ready->state == TASK_READY, "new task is not READY");
    REQUIRE(sched_terminate(ready), "terminating a READY task failed");
    REQUIRE(ready->state == TASK_TERMINATED && sched_queue_consistent(), "terminated READY task still queued");
    for (unsigned guard = 0; blocked->state != TASK_BLOCKED && guard < 200; ++guard) sched_yield();
    REQUIRE(blocked->state == TASK_BLOCKED, "task did not block");
    REQUIRE(sched_terminate(blocked), "terminating a BLOCKED task failed");
    REQUIRE(!sched_wake(blocked), "a terminated task was woken");
    settle();
    REQUIRE(counters[3] == 0, "a terminated task ran");
    REQUIRE(!sched_terminate(task_current()), "self-termination through terminate was accepted");
    struct sched_stats st; sched_stats_get(&st);
    REQUIRE(!sched_terminate(task_by_tid(st.idle_tid)) && !sched_terminate(task_by_tid(1)), "an immortal task was terminated");
    REQUIRE(!sched_terminate(NULL), "terminate(NULL) accepted");
    REQUIRE(pmm_free_pages() == frames && sched_queue_consistent(), "termination leaked or corrupted the queue");
    return 0;
}

static const char *invalid_transitions(void) {
    struct sched_stats st; sched_stats_get(&st);
    struct task *t = sched_spawn("inv", parked, NULL, TASK_FLAG_KERNEL);
    REQUIRE(t, "spawn failed");
    REQUIRE(!sched_make_ready(t), "READY task made READY again");
    REQUIRE(!sched_make_ready(NULL) && !sched_make_ready(task_current()), "invalid make_ready accepted");
    uint32_t before = (uint32_t)(sched_ready_count());
    sched_yield(); sched_yield();
    REQUIRE(t->state == TASK_BLOCKED || t->state == TASK_READY, "task in an impossible state");
    (void)before;
    REQUIRE(sched_terminate(t), "cleanup terminate failed");
    settle();
    return 0;
}

const struct test_case sched_cases[] = {
    {"scheduler preempts two tasks that never yield", two_tasks},
    {"scheduler shares the CPU among six tasks", many_tasks},
    {"scheduler round-robin yield order", yielding},
    {"scheduler blocking and waking", block_wake},
    {"scheduler remembers a wake before the block", wake_before_block},
    {"scheduler idle task runs when nothing else can", idle_and_sleep},
    {"scheduler task exit, reaping and repeated creation", exit_and_cleanup},
    {"scheduler terminates READY and BLOCKED tasks, protects immortal ones", termination},
    {"scheduler rejects invalid transitions", invalid_transitions},
};
const size_t sched_case_count = sizeof(sched_cases) / sizeof(sched_cases[0]);
