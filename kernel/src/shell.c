#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"

static int same(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void prompt(void) { terminal_write_color("ULTRIUM> ", TERMINAL_ACCENT); }
// Normalize case and repeated spaces so command comparisons are reliable.
static void normalize(char *line) {
    size_t read = 0, write = 0;
    while (line[read] == ' ') ++read;
    int pending_space = 0;
    for (; line[read]; ++read) {
        char ch = line[read];
        if (ch == ' ') { pending_space = write > 0; continue; }
        if (pending_space) { line[write++] = ' '; pending_space = 0; }
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + ('a' - 'A'));
        line[write++] = ch;
    }
    line[write] = 0;
}
static void execute(const char *cmd) {
    if (!*cmd) return;
    if (same(cmd, "help")) {
        terminal_write("help, clear, ultrium version, ultrium system info, ultrium ai status\n");
    } else if (same(cmd, "clear")) {
        terminal_clear();
    } else if (same(cmd, "ultrium version")) {
        terminal_write("Ultrium OS 0.0.4\n");
    } else if (same(cmd, "ultrium system info")) {
        terminal_write("OS: Ultrium OS\nARCH: x86_64\nINTERFACE: CLI\n");
    } else if (same(cmd, "ultrium ai status")) {
        terminal_write("AI SERVICES NOT INSTALLED\n");
    } else {
        terminal_write_color("UNKNOWN COMMAND. TYPE help\n", TERMINAL_MUTED);
    }
}
void shell_run(void) {
    char line[128];
    size_t len = 0;
    prompt();
    terminal_cursor(1);
    for (;;) {
        int key = keyboard_poll();
        if (key == 0) continue;
        terminal_cursor(0);
        if (key == '\b') {
            if (len) {
                --len;
                line[len] = 0;
                terminal_backspace();
            }
        } else if (key == '\n') {
            line[len] = 0;
            terminal_newline();
            normalize(line);
            execute(line);
            len = 0;
            prompt();
        } else if (key >= 32 && key <= 126 && len < sizeof(line) - 1) {
            line[len++] = (char)key;
            terminal_putc_color((char)key, TERMINAL_NORMAL);
        }
        terminal_cursor(1);
    }
}
