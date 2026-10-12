#ifndef ULTRIUM_TERMINAL_H
#define ULTRIUM_TERMINAL_H
struct limine_framebuffer;
enum terminal_color { TERMINAL_NORMAL, TERMINAL_ACCENT, TERMINAL_MUTED };
void terminal_init(struct limine_framebuffer *framebuffer);
void terminal_clear(void);
void terminal_newline(void);
void terminal_backspace(void);
void terminal_cursor(int visible);
void terminal_putc_color(char ch, enum terminal_color color);
void terminal_write_color(const char *text, enum terminal_color color);
void terminal_write(const char *text);
#endif
