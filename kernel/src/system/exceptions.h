#ifndef ULTRIUM_CPU_EXCEPTIONS_H
#define ULTRIUM_CPU_EXCEPTIONS_H
#include <stdbool.h>
bool exceptions_init(void);
bool exceptions_ready(void);
bool exceptions_self_test(void);
bool exceptions_install_irq_gate(unsigned vector, void (*handler)(void));
#endif
