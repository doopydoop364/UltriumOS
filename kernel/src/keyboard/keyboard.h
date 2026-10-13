#ifndef ULTRIUM_KEYBOARD_H
#define ULTRIUM_KEYBOARD_H
#include <stdbool.h>
#include <stdint.h>
#define KEY_UP 256
#define KEY_DOWN 257
#define KEY_CTRL_C 258
#define KEY_CTRL_U 259
#define KEY_CTRL_W 260
#define KEY_CTRL_L 261
#define KEY_LEFT 262
#define KEY_RIGHT 263
#define KEY_DELETE 264
#define KEY_HOME 265
#define KEY_END 266
int keyboard_poll(void);
bool keyboard_init_interrupts(void);
bool keyboard_interrupt_input(void);
uint32_t keyboard_dropped_scancodes(void);
#endif
