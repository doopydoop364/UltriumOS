#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "memory/aspace.h"
#include "memory/vmm.h"
#include "system/exceptions.h"

struct process {
    enum process_state state;
    uint32_t pid, ppid;
    char name[PROCESS_NAME_MAX];
    struct aspace *aspace;
    struct task *task;
    uint32_t tid;
    uint64_t entry, stack;
    int64_t exit_status;
    bool faulted, reaped;
    struct process_fault fault;
    struct task *waiter;
};

static struct process table[PROCESS_MAX];
static uint32_t next_pid = 1;
static bool ready;

static inline uint64_t irq_off(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void irq_back(uint64_t f) { if (f & (1ULL << 9)) __asm__ volatile("sti" ::: "memory"); }

/* Leave ring 0 for ring 3 with a clean register file. Never returns. */
__asm__(
".global ultrium_enter_user\n"
"ultrium_enter_user:\n"                      /* rdi = entry rip, rsi = user rsp */
"cli\n"
"movq $0x1b, %rax\n"                         /* USER_DATA_SELECTOR */
"pushq %rax\n"                               /* ss */
"pushq %rsi\n"                               /* rsp */
"pushq $0x202\n"                             /* rflags: IF set, IOPL 0 */
"pushq $0x23\n"                              /* USER_CODE_SELECTOR */
"pushq %rdi\n"                               /* rip */
"xorl %eax, %eax\n" "xorl %ebx, %ebx\n" "xorl %ecx, %ecx\n" "xorl %edx, %edx\n"
"xorl %esi, %esi\n" "xorl %edi, %edi\n" "xorl %ebp, %ebp\n"
"xorl %r8d, %r8d\n" "xorl %r9d, %r9d\n" "xorl %r10d, %r10d\n" "xorl %r11d, %r11d\n"
"xorl %r12d, %r12d\n" "xorl %r13d, %r13d\n" "xorl %r14d, %r14d\n" "xorl %r15d, %r15d\n"
"iretq\n");
extern __attribute__((noreturn)) void ultrium_enter_user(uint64_t rip, uint64_t rsp);

static void user_task_main(void *arg) {
    struct process *p = arg;
    ultrium_enter_user(p->entry, p->stack);
}

static void copy_name(char *out, const char *name) {
    size_t i = 0;
    if (name) for (; name[i] && i + 1 < PROCESS_NAME_MAX; ++i) out[i] = name[i];
    out[i] = 0;
}
struct process *process_find(uint32_t pid) {
    if (!pid) return NULL;
    for (unsigned i = 0; i < PROCESS_MAX; ++i) if (table[i].state != PROC_FREE && table[i].pid == pid) return &table[i];
    return NULL;
}
struct process *process_current(void) { return task_current()->process; }
uint32_t process_current_pid(void) { struct process *p = process_current(); return p ? p->pid : 0; }
struct aspace *process_aspace(uint32_t pid) { struct process *p = process_find(pid); return p ? p->aspace : NULL; }
unsigned process_count_live(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < PROCESS_MAX; ++i) if (table[i].state != PROC_FREE) ++n;
    return n;
}

/* Runs for each terminated task before the scheduler frees it: release what the process owns. */
static void reap_task(struct task *t) {
    struct process *p = t->process;
    if (!p || p->task != t) return;
    if (p->aspace) { aspace_destroy(p->aspace); p->aspace = NULL; }
    p->task = NULL;
    p->reaped = true;
    t->aspace = NULL;
    t->process = NULL;
}

static void fault_hook(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address) {
    struct process *p = process_current();
    if (!p) return;                                     /* a ring-3 fault with no process: fall through to the fatal path */
    p->faulted = true;
    p->fault = (struct process_fault){vector, error, rip, address};
    process_exit_current(PROCESS_EXIT_FAULT_BASE - (int64_t)vector);
}

bool process_system_init(void) {
    if (ready || !sched_ready() || !aspace_system_ready()) return false;
    /* SIMD/x87 for user tasks: the kernel itself never uses them (see process-model.md). */
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr0 = (cr0 | (1ULL << 1)) & ~((1ULL << 2) | (1ULL << 3));          /* MP on, EM off, TS off */
    cr4 |= (1ULL << 9) | (1ULL << 10);                                 /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    /* The SYSCALL instruction is not supported yet: make sure it raises #UD in ring 3 instead of jumping
       through whatever LSTAR the firmware left behind. */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080U));
    if (lo & 1U) { lo &= ~1U; __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xc0000080U) : "memory"); }
    sched_set_reap_hook(reap_task);
    exceptions_set_user_fault_hook(fault_hook);
    ready = true;
    return true;
}
bool process_system_ready(void) { return ready; }

uint32_t process_create(const char *name, uint32_t parent_pid) {
    if (!ready || next_pid == UINT32_MAX) return 0;
    struct process *p = NULL;
    for (unsigned i = 0; i < PROCESS_MAX; ++i) if (table[i].state == PROC_FREE) { p = &table[i]; break; }
    if (!p) return 0;
    *p = (struct process){0};
    p->aspace = aspace_create();
    if (!p->aspace) return 0;
    p->task = task_create(name, user_task_main, p, 0);
    if (!p->task) { aspace_destroy(p->aspace); *p = (struct process){0}; return 0; }
    p->task->process = p;
    p->task->aspace = p->aspace;
    p->task->uses_fpu = true;
    p->task->fxsave[0] = 0x7f; p->task->fxsave[1] = 0x03;             /* FCW 0x037F */
    p->task->fxsave[24] = 0x80; p->task->fxsave[25] = 0x1f;           /* MXCSR 0x1F80 */
    p->tid = p->task->tid;
    p->pid = next_pid++;
    p->ppid = parent_pid;
    copy_name(p->name, name);
    p->state = PROC_CREATED;
    return p->pid;
}

bool process_map_segment(uint32_t pid, uint64_t va, const void *data, size_t data_size, size_t memory_size, uint32_t permissions) {
    struct process *p = process_find(pid);
    if (!p || p->state != PROC_CREATED || !memory_size || data_size > memory_size || (data_size && !data)) return false;
    uint64_t start = va & ~4095ULL;
    if (memory_size > UINT64_MAX - (va - start) - 4095ULL) return false;
    size_t pages = (size_t)(((va - start) + memory_size + 4095ULL) / 4096ULL);
    if (!aspace_user_bounds_ok(start, pages * 4096ULL)) return false;
    if (pages > ASPACE_MAP_MAX_PAGES) return false;                          /* segments are limited to 1 MiB */
    for (size_t i = 0; i < pages; ++i)
        if (aspace_translate(p->aspace, start + i * 4096ULL, false, NULL)) return false;   /* overlaps an earlier segment */
    /* Writable first so the loader can fill it, then tightened to the final permissions. */
    if (!aspace_map(p->aspace, start, pages, VMM_PAGE_READ | VMM_PAGE_WRITE)) return false;
    bool ok = !data_size || aspace_poke(p->aspace, va, data, data_size);
    if (ok && permissions != (VMM_PAGE_READ | VMM_PAGE_WRITE)) ok = aspace_protect(p->aspace, start, pages, permissions);
    if (!ok) { (void)aspace_unmap(p->aspace, start, pages); return false; }
    return true;
}

bool process_map_stack(uint32_t pid) {
    struct process *p = process_find(pid);
    if (!p || p->state != PROC_CREATED) return false;
    return aspace_map(p->aspace, PROCESS_STACK_TOP - (uint64_t)PROCESS_STACK_PAGES * 4096ULL, PROCESS_STACK_PAGES, VMM_PAGE_READ | VMM_PAGE_WRITE);
}

bool process_start(uint32_t pid, uint64_t entry, uint64_t stack) {
    struct process *p = process_find(pid);
    if (!p || p->state != PROC_CREATED || !aspace_user_bounds_ok(entry, 1) || !aspace_is_executable(p->aspace, entry)) return false;
    if (!aspace_user_bounds_ok(stack - 8, 8) || !aspace_translate(p->aspace, stack - 8, true, NULL)) return false;
    p->entry = entry; p->stack = stack;
    p->state = PROC_ACTIVE;
    if (!sched_make_ready(p->task)) { p->state = PROC_CREATED; return false; }
    return true;
}

static void finish_slot(struct process *p) {
    *p = (struct process){0};
}
bool process_discard(uint32_t pid) {
    struct process *p = process_find(pid);
    if (!p || p->state != PROC_CREATED) return false;
    uint64_t f = irq_off();
    struct task *t = p->task;
    if (t) {
        t->process = NULL;
        t->aspace = NULL;
        (void)task_destroy(t);
    }
    if (p->aspace) aspace_destroy(p->aspace);
    finish_slot(p);
    irq_back(f);
    return true;
}

__attribute__((noreturn)) void process_exit_current(int64_t status) {
    __asm__ volatile("cli" ::: "memory");
    struct process *p = process_current();
    if (p) {
        p->exit_status = status;
        p->state = PROC_EXITED;
        if (p->waiter) { struct task *w = p->waiter; p->waiter = NULL; (void)sched_wake(w); }
    }
    sched_exit();
}

bool process_kill(uint32_t pid) {
    struct process *p = process_find(pid);
    if (!p || p->state == PROC_EXITED) return false;
    if (p == process_current()) process_exit_current(PROCESS_EXIT_KILLED);
    uint64_t f = irq_off();
    bool ok = false;
    if (p->state == PROC_ACTIVE && p->task && sched_terminate(p->task)) {
        p->exit_status = PROCESS_EXIT_KILLED;
        p->state = PROC_EXITED;
        if (p->waiter) { struct task *w = p->waiter; p->waiter = NULL; (void)sched_wake(w); }
        ok = true;
    }
    irq_back(f);
    return ok;
}

bool process_collect(uint32_t pid, int64_t *status) {
    struct process *p = process_find(pid);
    if (!p || p->state != PROC_EXITED) return false;
    for (unsigned guard = 0; !p->reaped && guard < 64; ++guard) sched_yield();   /* let the reaper release the address space */
    if (!p->reaped) return false;
    if (status) *status = p->exit_status;
    uint64_t f = irq_off();
    finish_slot(p);
    irq_back(f);
    return true;
}

bool process_wait(uint32_t pid, int64_t *status) {
    struct process *p = process_find(pid);
    if (!p || p == process_current()) return false;
    if (p->state != PROC_ACTIVE && p->state != PROC_EXITED) return false;
    uint64_t f = irq_off();
    while (p->state == PROC_ACTIVE) {
        if (p->waiter && p->waiter != task_current()) { irq_back(f); return false; }       /* one waiter at a time */
        p->waiter = task_current();
        sched_block(SCHED_BLOCK_CHILD);
    }
    p->waiter = NULL;
    irq_back(f);
    return process_collect(pid, status);
}

bool process_info_get(uint32_t pid, struct process_info *info) {
    struct process *p = process_find(pid);
    if (!p || !info) return false;
    *info = (struct process_info){.pid = p->pid, .ppid = p->ppid, .state = p->state, .exit_status = p->exit_status,
                                  .faulted = p->faulted, .fault = p->fault};
    for (unsigned i = 0; i < PROCESS_NAME_MAX; ++i) info->name[i] = p->name[i];
    info->task_state = p->task ? p->task->state : TASK_TERMINATED;
    info->data_pages = p->aspace ? aspace_data_pages(p->aspace) : 0;
    info->switches = p->task ? p->task->switches_in : 0;
    return true;
}
unsigned process_list(struct process_info *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < PROCESS_MAX && n < max; ++i)
        if (table[i].state != PROC_FREE && process_info_get(table[i].pid, &out[n])) ++n;
    return n;
}
