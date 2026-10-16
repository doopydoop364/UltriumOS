#ifndef ULTRIUM_INTERRUPTS_H
#define ULTRIUM_INTERRUPTS_H
#include <stdbool.h>
#include <stdint.h>

bool interrupts_init(void);
bool interrupts_ready(void);
/* Called from the timer IRQ after the EOI; `from_user` is true when ring 3 was interrupted. */
void interrupts_set_tick_hook(void (*hook)(bool from_user));
uint64_t interrupts_timer_ticks(void);
bool interrupts_register_irq_handler(unsigned irq, void (*handler)(void));
bool interrupts_self_test(void);
uint32_t interrupts_test_status(void);

#endif
