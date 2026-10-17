#ifndef ULTRIUM_SYSCALL_H
#define ULTRIUM_SYSCALL_H
#include <stdbool.h>
#include <stdint.h>

/* System-call entry via `int 0x80` (docs/syscall-abi.md). */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax;   /* pushed by the entry stub */
    uint64_t rip, cs, rflags, rsp, ss;                                                  /* CPU interrupt frame (ring 3 -> 0) */
};
bool syscall_init(void);
bool syscall_ready(void);
uint64_t syscall_count(void);
struct process;
int64_t syscall_run(struct process *process, uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3);
int64_t syscall_test_invoke(uint32_t pid, uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3);
#endif
