#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"

/* Built-in Ultrium commands and future external programs have separate dispatch paths. */
typedef void (*command_handler)(void);
struct command { const char *name; command_handler handler; };

static int same(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void normalize(char *line) {
    size_t read = 0, write = 0;
    while (line[read] == ' ') ++read;
    int space = 0;
    while (line[read]) {
        char ch = line[read++];
        if (ch == ' ') { space = write > 0; continue; }
        if (space) { line[write++] = ' '; space = 0; }
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
        line[write++] = ch;
    }
    line[write] = 0;
}
static void prompt(void) { terminal_write_color("ultrium> ", TERMINAL_ACCENT); }
static void help(void) {
    terminal_write("Ultrium commands:\n");
    terminal_write("  ultrium help\n  ultrium clear\n  ultrium version\n");
    terminal_write("  ultrium system info\n  ultrium ai status\n");
    terminal_write("External programs will use unprefixed names when supported.\n");
}
static void version(void) { terminal_write("Ultrium OS 0.0.5\n"); }
static void system_info(void) {
    terminal_write("OS: Ultrium OS\nKernel: 0.0.5\nArchitecture: x86_64\nMode: CLI\n");
}
static void ai_status(void) { terminal_write("AI services are not installed.\n"); }
static void clear(void) { terminal_clear(); }
static const struct command commands[] = {
    {"help", help}, {"clear", clear}, {"version", version},
    {"system info", system_info}, {"ai status", ai_status},
};
static void dispatch(const char *line) {
    if (!*line) return;
    if (same(line, "ultrium")) { help(); return; }
    const char *prefix = "ultrium ";
    const char *p = line;
    while (*prefix && *p == *prefix) { ++prefix; ++p; }
    if (*prefix == '\0') {
        for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i)
            if (same(p, commands[i].name)) { commands[i].handler(); return; }
        terminal_write_color("Unknown Ultrium command. Try: ultrium help\n", TERMINAL_MUTED);
    } else {
        terminal_write_color("External programs are not supported yet.\n", TERMINAL_MUTED);
        terminal_write("For built-in commands, use: ultrium help\n");
    }
}
void shell_run(void) {
    char line[128] = {0};
    size_t len = 0;
    prompt();
    terminal_cursor(1);
    for (;;) {
        int key = keyboard_poll();
        if (!key) continue;
        terminal_cursor(0);
        if (key == '\b') {
            if (len) { line[--len] = 0; terminal_backspace(); }
        } else if (key == '\n') {
            line[len] = 0;
            terminal_newline();
            normalize(line);
            dispatch(line);
            len = 0;
            prompt();
        } else if (key >= 32 && key <= 126 && len < sizeof(line) - 1) {
            line[len++] = (char)key;
            terminal_putc_color((char)key, TERMINAL_NORMAL);
        }
        terminal_cursor(1);
    }
}
