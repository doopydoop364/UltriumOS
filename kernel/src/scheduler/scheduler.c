#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scheduler/scheduler.h"
#include "system/interrupts.h"

static struct task *queue[TASK_MAX];
static unsigned q_head, q_count;
static struct task *idle;
static bool ready;
static struct sched_stats stats;
static void (*reap_hook)(struct task *);
static volatile bool in_reap;

static inline uint64_t irq_off(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_back(uint64_t f) { if (f & (1ULL << 9)) __asm__ volatile("sti" ::: "memory"); }

static bool enqueue(struct task *t) {
    if (q_count >= TASK_MAX) return false;
    queue[(q_head + q_count) % TASK_MAX] = t;
    ++q_count;
    return true;
}
static struct task *dequeue(void) {
    if (!q_count) return NULL;
    struct task *t = queue[q_head];
    queue[q_head] = NULL;
    q_head = (q_head + 1) % TASK_MAX;
    --q_count;
    return t;
}
static bool unqueue(struct task *t) {
    for (unsigned i = 0; i < q_count; ++i) {
        if (queue[(q_head + i) % TASK_MAX] != t) continue;
        for (unsigned j = i; j + 1 < q_count; ++j) queue[(q_head + j) % TASK_MAX] = queue[(q_head + j + 1) % TASK_MAX];
        queue[(q_head + q_count - 1) % TASK_MAX] = NULL;
        --q_count;
        return true;
    }
    return false;
}
unsigned sched_ready_count(void) { return q_count; }
bool sched_queue_consistent(void) {
    for (unsigned i = 0; i < q_count; ++i) {
        struct task *t = queue[(q_head + i) % TASK_MAX];
        if (!t || t->state != TASK_READY || t == idle) return false;
        for (unsigned j = i + 1; j < q_count; ++j) if (queue[(q_head + j) % TASK_MAX] == t) return false;
    }
    return true;
}

/* Interrupts are off. Frees terminated tasks other than the running one. */
static void reap(void) {
    if (in_reap) return;
    in_reap = true;
    struct task *cur = task_current();
    for (struct task *t = task_first(); t; t = task_next(t)) {
        if (t->state != TASK_TERMINATED || t == cur) continue;
        if (reap_hook) reap_hook(t);
        if (task_destroy(t)) ++stats.reaped;
    }
    in_reap = false;
}
void sched_set_reap_hook(void (*hook)(struct task *)) { reap_hook = hook; }

static void idle_main(void *arg) {
    (void)arg;
    for (;;) {
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}
static void exit_trampoline(void) { sched_exit(); }

bool sched_init(void) {
    if (ready || !task_system_ready()) return false;
    idle = task_create("idle", idle_main, NULL, TASK_FLAG_KERNEL | TASK_FLAG_PREEMPTIBLE | TASK_FLAG_IMMORTAL);
    if (!idle) return false;
    idle->state = TASK_READY;                          /* never queued: chosen only when the queue is empty */
    stats.idle_tid = idle->tid;
    task_set_exit_handler(exit_trampoline);
    task_set_post_switch_hook(reap);
    task_current()->slice_left = SCHED_SLICE_TICKS;
    ready = true;
    return true;
}
bool sched_ready(void) { return ready; }

struct task *sched_spawn(const char *name, void (*entry)(void *), void *arg, uint32_t flags) {
    if (!ready) return NULL;
    struct task *t = task_create(name, entry, arg, flags);
    if (!t) return NULL;
    if (!sched_make_ready(t)) { (void)task_destroy(t); return NULL; }
    return t;
}
bool sched_make_ready(struct task *t) {
    uint64_t f = irq_off();
    bool ok = ready && t && t != idle && t->state == TASK_CREATED && enqueue(t);
    if (ok) { t->state = TASK_READY; t->slice_left = SCHED_SLICE_TICKS; }
    else ++stats.invalid_transitions;
    irq_back(f);
    return ok;
}

/* Interrupts are off. Move the current task to `new_state` and run the next one. */
static void switch_away(enum task_state new_state) {
    struct task *prev = task_current();
    prev->state = new_state;
    if (new_state == TASK_READY && prev != idle) (void)enqueue(prev);   /* cannot overflow: at most TASK_MAX-1 others */
    struct task *next = dequeue();
    if (!next) next = idle;
    if (next == prev) { prev->state = TASK_RUNNING; return; }
    next->state = TASK_RUNNING;
    next->slice_left = SCHED_SLICE_TICKS;
    ++stats.switches;
    task_switch_to(next);
}

void sched_yield(void) {
    if (!ready) return;
    uint64_t f = irq_off();
    ++stats.yields;
    if (q_count) switch_away(TASK_READY);
    reap();                                         /* tasks terminated by others are released at the next scheduling point */
    irq_back(f);
}

void sched_block(uint32_t reason) {
    if (!ready) return;
    uint64_t f = irq_off();
    struct task *cur = task_current();
    if (cur == idle) { ++stats.invalid_transitions; irq_back(f); return; }
    if (cur->wake_pending) {
        cur->wake_pending = false;
    } else {
        cur->block_reason = reason;
        ++stats.blocks;
        switch_away(TASK_BLOCKED);
    }
    irq_back(f);
}

void sched_sleep(uint32_t ticks) {
    if (!ready || !ticks) return;
    uint64_t f = irq_off();
    struct task *cur = task_current();
    if (cur != idle) {
        cur->wake_tick = stats.ticks + ticks;
        cur->wake_pending = false;
        cur->block_reason = SCHED_BLOCK_SLEEP;
        ++stats.blocks;
        switch_away(TASK_BLOCKED);
    }
    irq_back(f);
}

bool sched_wake(struct task *t) {
    if (!ready || !t) return false;
    uint64_t f = irq_off();
    bool ok = false;
    if (t->state == TASK_BLOCKED && enqueue(t)) {
        t->state = TASK_READY; t->block_reason = 0; ++stats.wakes; ok = true;
    } else if (t->state == TASK_RUNNING || t->state == TASK_READY) {
        t->wake_pending = true;                    /* wake arrived before the block: not lost */
        ok = true;
    } else ++stats.invalid_transitions;
    irq_back(f);
    return ok;
}

__attribute__((noreturn)) void sched_exit(void) {
    __asm__ volatile("cli" ::: "memory");
    struct task *cur = task_current();
    if (cur->flags & TASK_FLAG_IMMORTAL) for (;;) __asm__ volatile("hlt");
    switch_away(TASK_TERMINATED);
    for (;;) __asm__ volatile("hlt");              /* a terminated task is never switched back in */
}

bool sched_terminate(struct task *t) {
    if (!ready || !t) return false;
    uint64_t f = irq_off();
    bool ok = false;
    if (t != task_current() && !(t->flags & TASK_FLAG_IMMORTAL)) {
        if (t->state == TASK_READY) { ok = unqueue(t); }
        else ok = t->state == TASK_BLOCKED || t->state == TASK_CREATED;
        if (ok) t->state = TASK_TERMINATED;
    }
    if (!ok) ++stats.invalid_transitions;
    irq_back(f);
    return ok;
}

void sched_tick(bool from_user) {
    ++stats.ticks;
    if (!ready) return;
    for (struct task *t = task_first(); t; t = task_next(t))
        if (t->state == TASK_BLOCKED && t->block_reason == SCHED_BLOCK_SLEEP && t->wake_tick <= stats.ticks && enqueue(t)) {
            t->state = TASK_READY; t->block_reason = 0; ++stats.wakes;
        }
    struct task *cur = task_current();
    if (cur == idle) {
        if (q_count) { ++stats.preemptions; switch_away(TASK_READY); }
        return;
    }
    if (cur->slice_left) --cur->slice_left;
    if (cur->slice_left) return;
    if (!q_count) { cur->slice_left = SCHED_SLICE_TICKS; return; }
    if (from_user || (cur->flags & TASK_FLAG_PREEMPTIBLE)) {
        ++stats.preemptions;
        switch_away(TASK_READY);
    }
    /* Otherwise kernel code is running: it is switched out at its next yield or block. */
}

void sched_idle_wait(void) {
    if (!ready) { __asm__ volatile("pause"); return; }
    if (q_count) { sched_yield(); return; }
    { uint64_t f = irq_off(); reap(); irq_back(f); }
    if (interrupts_ready()) __asm__ volatile("sti; hlt" ::: "memory");
    else __asm__ volatile("pause");
}

void sched_stats_get(struct sched_stats *out) {
    if (!out) return;
    uint64_t f = irq_off();
    *out = stats;
    out->ready = q_count;
    out->blocked = 0;
    for (struct task *t = task_first(); t; t = task_next(t)) if (t->state == TASK_BLOCKED) ++out->blocked;
    irq_back(f);
}
