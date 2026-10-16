#include <stdbool.h>
#include <stdint.h>
#include "syscalls/syscall.h"
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "system/exceptions.h"

static bool ready;
static uint64_t calls;

__asm__(
".global ultrium_syscall_entry\n"
"ultrium_syscall_entry:\n"
"cld\n"
"pushq %rax\n" "pushq %rbx\n" "pushq %rcx\n" "pushq %rdx\n"
"pushq %rbp\n" "pushq %rsi\n" "pushq %rdi\n"
"pushq %r8\n" "pushq %r9\n" "pushq %r10\n" "pushq %r11\n"
"pushq %r12\n" "pushq %r13\n" "pushq %r14\n" "pushq %r15\n"
"movq %rsp, %r12\n"
"movq %rsp, %rdi\n"
"andq $-16, %rsp\n"
"call ultrium_syscall_dispatch\n"
"movq %r12, %rsp\n"
"popq %r15\n" "popq %r14\n" "popq %r13\n" "popq %r12\n"
"popq %r11\n" "popq %r10\n" "popq %r9\n" "popq %r8\n"
"popq %rdi\n" "popq %rsi\n" "popq %rbp\n" "popq %rdx\n"
"popq %rcx\n" "popq %rbx\n" "popq %rax\n"
"iretq\n");
extern void ultrium_syscall_entry(void);

/* Minimal table for the ring-3 bring-up; the full ABI is added with the syscall interface. */
void ultrium_syscall_dispatch(struct syscall_frame *f) {
    ++calls;
    if ((f->cs & 3) != 3) { f->rax = (uint64_t)-1; return; }          /* only ring 3 may enter */
    switch (f->rax) {
        case 0: process_exit_current((int64_t)(int32_t)f->rdi); break;
        case 1: sched_yield(); f->rax = 0; break;
        default: f->rax = (uint64_t)-38; break;                        /* ULTRIUM_E_NOSYS */
    }
}

bool syscall_init(void) {
    if (ready) return false;
    ready = exceptions_install_syscall_gate(ultrium_syscall_entry);
    return ready;
}
bool syscall_ready(void) { return ready; }
uint64_t syscall_count(void) { return calls; }
