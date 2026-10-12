#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"
#include "system/reboot.h"
#include "memory/pmm.h"
#include "memory/heap.h"

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
    terminal_write("Shell: help, clear, clear history, reboot\n");
    terminal_write("Ultrium: ultrium help, ultrium version\n");
    terminal_write("  ultrium system info, ultrium system memory\n  ultrium ai status\n");
    terminal_write("External programs will use unprefixed names when supported.\n");
}
static void version(void) { terminal_write("Ultrium OS 0.0.7\n"); }
static void system_info(void) {
    terminal_write("OS: Ultrium OS\nKernel: 0.0.7\nArchitecture: x86_64\nMode: CLI\n");
}
static void print_uint(uint64_t value) {
    char digits[21];
    size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) terminal_putc_color(digits[--n], TERMINAL_NORMAL);
}
static void memory_info(void) {
    terminal_write("Usable RAM: ");
    print_uint(pmm_total_bytes() / (1024 * 1024));
    terminal_write(" MiB\nFree pages: ");
    print_uint(pmm_free_pages());
    terminal_write("\nAllocated pages: ");
    print_uint(pmm_used_pages());
    terminal_write("\nHeap allocations: ");
    print_uint(heap_live_allocations());
    terminal_write("\nPage size: 4096 bytes\n");
}
static void ai_status(void) { terminal_write("AI services are not installed.\n"); }
static void clear(void) { terminal_clear(); }
static void clear_history(void);
static const struct command commands[] = {
    {"help", help}, {"version", version},
    {"system info", system_info}, {"system memory", memory_info},
    {"ai status", ai_status},
};
static void dispatch(const char *line) {
    if (!*line) return;
    if (same(line, "help")) { help(); return; }
    if (same(line, "clear")) { clear(); return; }
    if (same(line, "clear history")) { clear_history(); return; }
    if (same(line, "reboot")) {
        terminal_write("Rebooting...\n");
        system_reboot();
        return;
    }
    if (same(line, "ultrium")) { help(); return; }
    const char *prefix = "ultrium ";
    const char *p = line;
    while (*prefix && *p == *prefix) { ++prefix; ++p; }
    if (*prefix == '\0') {
        for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i)
            if (same(p, commands[i].name)) { commands[i].handler(); return; }
        terminal_write_color("Unknown Ultrium command. Try: ultrium help\n", TERMINAL_MUTED);
    } else {
        terminal_write_color("Program not found (external execution unavailable).\n", TERMINAL_MUTED);
        terminal_write("For built-in commands, use: ultrium help\n");
    }
}
/* Expanded preallocated history pending a real memory allocator and filesystem.
   Limit command length to 70 so input stays on one line at common QEMU sizes. */
#define INPUT_LIMIT 70
#define HISTORY_COUNT 4096
static char history[HISTORY_COUNT][INPUT_LIMIT + 1];
static size_t history_size;
static void clear_history(void) {
    history_size = 0;
    terminal_write("Command history cleared.\n");
}

static void copy_line(char *dest, const char *src) {
    size_t i = 0;
    while (i < INPUT_LIMIT && src[i]) { dest[i] = src[i]; ++i; }
    dest[i] = '\0';
}

static size_t line_length(const char *line) {
    size_t i = 0;
    while (line[i] && i < INPUT_LIMIT) ++i;
    return i;
}

static void replace_input(char *line, size_t *len, const char *text) {
    while (*len) {
        terminal_backspace();
        --*len;
    }
    copy_line(line, text);
    *len = line_length(line);
    terminal_write(line);
}

static void save_history(const char *line) {
    if (!*line) return;
    if (history_size && same(history[history_size - 1], line)) return;
    if (history_size == HISTORY_COUNT) {
        for (size_t i = 1; i < HISTORY_COUNT; ++i)
            copy_line(history[i - 1], history[i]);
        --history_size;
    }
    copy_line(history[history_size++], line);
}

void shell_run(void) {
    char line[INPUT_LIMIT + 1] = {0};
    size_t len = 0;
    size_t selected = 0;
    prompt();
    terminal_cursor(1);
    for (;;) {
        int key = keyboard_poll();
        if (!key) continue;
        terminal_cursor(0);
        if (key == KEY_UP) {
            if (selected > 0) {
                --selected;
                replace_input(line, &len, history[selected]);
            }
        } else if (key == KEY_DOWN) {
            if (selected < history_size) {
                ++selected;
                replace_input(line, &len,
                    selected == history_size ? "" : history[selected]);
            }
        } else if (key == '\b') {
            if (len) { line[--len] = '\0'; terminal_backspace(); }
        } else if (key == '\n') {
            line[len] = '\0';
            terminal_newline();
            normalize(line);
            if (!same(line, "clear history")) save_history(line);
            dispatch(line);
            len = 0;
            line[0] = '\0';
            selected = history_size;
            prompt();
        } else if (key >= 32 && key <= 126 && len < INPUT_LIMIT) {
            line[len++] = (char)key;
            line[len] = '\0';
            terminal_putc_color((char)key, TERMINAL_NORMAL);
            selected = history_size;
        }
        terminal_cursor(1);
    }
}
