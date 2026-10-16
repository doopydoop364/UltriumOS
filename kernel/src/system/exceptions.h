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
#endif
