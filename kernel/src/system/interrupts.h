#ifndef ULTRIUM_INTERRUPTS_H
#define ULTRIUM_INTERRUPTS_H
#include <stdbool.h>
#include <stdint.h>

bool interrupts_init(void);
bool interrupts_ready(void);
uint64_t interrupts_timer_ticks(void);
bool interrupts_register_irq_handler(unsigned irq, void (*handler)(void));
bool interrupts_self_test(void);
uint32_t interrupts_test_status(void);

#endif
