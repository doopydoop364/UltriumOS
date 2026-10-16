#ifndef ULTRIUM_CPU_EXCEPTIONS_H
#define ULTRIUM_CPU_EXCEPTIONS_H
#include <stdbool.h>
#include <stdint.h>
bool exceptions_init(void);
bool exceptions_ready(void);
bool exceptions_self_test(void);
bool exceptions_install_irq_gate(unsigned vector, void (*handler)(void));
/* TSS.RSP0: the kernel stack the CPU switches to when an interrupt or exception arrives from ring 3. */
void exceptions_set_kernel_stack(uint64_t top);
uint64_t exceptions_kernel_stack(void);
#define USER_DATA_SELECTOR 0x1bU      /* 0x18 | RPL 3 */
#define USER_CODE_SELECTOR 0x23U      /* 0x20 | RPL 3 */
/* Called for an exception taken in ring 3 (not NMI, double fault or machine check). Must not return. */
void exceptions_set_user_fault_hook(void (*hook)(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address));
bool exceptions_install_syscall_gate(void (*handler)(void));
#endif
