#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"
#include "system/reboot.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "system/exceptions.h"
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
    terminal_write("Prefixes:\n");
    terminal_write("  ultrium     Ultrium OS commands (ultrium help)\n");
    terminal_write("  ultrium ai  AI commands (ultrium ai help)\n");
    terminal_write("  clear       Clear commands (clear help)\n");
    terminal_write("General commands:\n");
    terminal_write("  help, reboot\n");
}
static void clear_help(void) {
    terminal_write("Clear commands:\n");
    terminal_write("  clear          Clear the screen\n");
    terminal_write("  clear history  Erase command history\n");
}
static void ultrium_help(void) {
    terminal_write("Ultrium OS commands:\n");
    terminal_write("  ultrium help\n");
    terminal_write("  ultrium version\n");
    terminal_write("  ultrium system help  (System namespace)\n");
    terminal_write("  ultrium ai help  (AI namespace)\n");
}
static void system_help(void) {
    terminal_write("Ultrium system commands:\n");
    terminal_write("  ultrium system help\n");
    terminal_write("  ultrium system info\n");
    terminal_write("  ultrium system memory help  (Memory namespace)\n");
    terminal_write("  ultrium system exceptions\n");
}
static void memory_help(void) {
    terminal_write("Ultrium memory commands:\n");
    terminal_write("  ultrium system memory\n");
    terminal_write("  ultrium system memory help\n");
    terminal_write("  ultrium system memory test\n");
    terminal_write("  ultrium system memory map help\n");
    terminal_write("  ultrium system memory mapping help\n");
}
static void memory_map_help(void) {
    terminal_write("Page table inspection commands:\n");
    terminal_write("  ultrium system memory map help\n");
    terminal_write("  ultrium system memory map test\n");
}
static void memory_mapping_help(void) {
    terminal_write("Virtual mapping commands:\n");
    terminal_write("  ultrium system memory mapping help\n");
    terminal_write("  ultrium system memory mapping test\n");
}
static void ai_help(void) {
    terminal_write("Ultrium AI commands:\n");
    terminal_write("  ultrium ai help\n");
    terminal_write("  ultrium ai status\n");
    terminal_write("AI services are not installed.\n");
}
static void version(void) { terminal_write("Ultrium OS 0.1.1\n"); }
static void system_info(void) {
    terminal_write("OS: Ultrium OS\nKernel: 0.1.1\nArchitecture: x86_64\nMode: CLI\n");
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
/* Explicit QEMU stress test: independent writes, reuse, and page accounting. */
static void memory_test(void) {
    enum { COUNT = 128 };
    void *blocks[COUNT] = {0};
    uint64_t initial_free = pmm_free_pages();
    uint64_t initial_live = heap_live_allocations();
    size_t allocated = 0;
    bool ok = true;
    for (size_t i = 0; i < COUNT; ++i) {
        blocks[i] = kmalloc(32);
        if (!blocks[i]) { ok = false; break; }
        ++allocated;
        for (size_t j = 0; j < 32; ++j)
            ((unsigned char *)blocks[i])[j] = (unsigned char)(i ^ j ^ 0x5a);
    }
    if (ok && pmm_free_pages() + 1 != initial_free) ok = false;
    for (size_t i = 0; i < allocated; ++i)
        for (size_t j = 0; j < 32; ++j)
            if (((unsigned char *)blocks[i])[j] != (unsigned char)(i ^ j ^ 0x5a))
                ok = false;
    for (size_t i = 0; i < allocated; ++i) kfree(blocks[i]);
    if (pmm_free_pages() != initial_free ||
        heap_live_allocations() != initial_live) ok = false;
    void *again = kmalloc(32);
    if (!again) ok = false;
    kfree(again);
    if (pmm_free_pages() != initial_free ||
        heap_live_allocations() != initial_live) ok = false;
    terminal_write(ok ? "SLAB STRESS TEST PASSED (128 x 32 bytes)\n"
                      : "SLAB STRESS TEST FAILED\n");
}
static void memory_map_test(void) {
    terminal_write(vmm_self_test() ? "PAGE TABLE TEST PASSED\n"
                                   : "PAGE TABLE TEST FAILED\n");
}
static void exception_status(void) {
    terminal_write(exceptions_ready() ?
        "CPU exception handlers: installed (vectors 0-31)\n" :
        "CPU exception handlers: unavailable\n");
}
static void mapping_test(void) {
    terminal_write(vmm_map_test() ?
        "VIRTUAL MAPPING TEST PASSED\n" : "VIRTUAL MAPPING TEST FAILED\n");
}
static void ai_status(void) { terminal_write("AI services are not installed.\n"); }
static void clear(void) { terminal_clear(); }
static void clear_history(void);
static const struct command commands[] = {
    {"help", ultrium_help}, {"version", version},
    {"system help", system_help}, {"system info", system_info},
    {"system memory", memory_info}, {"system memory help", memory_help},
    {"system memory map help", memory_map_help},
    {"system memory mapping help", memory_mapping_help},
    {"system memory test", memory_test},
    {"system memory map test", memory_map_test},
    {"system memory mapping test", mapping_test},
    {"system exceptions", exception_status},
    {"ai help", ai_help}, {"ai status", ai_status},
};
static void dispatch(const char *line) {
    if (!*line) return;
    if (same(line, "help")) { help(); return; }
    if (same(line, "clear")) { clear(); return; }
    if (same(line, "clear help")) { clear_help(); return; }
    if (same(line, "clear history")) { clear_history(); return; }
    if (same(line, "reboot")) {
        terminal_write("Rebooting...\n");
        system_reboot();
        return;
    }
    if (same(line, "ultrium")) { ultrium_help(); return; }
    if (same(line, "ultrium ai")) { ai_help(); return; }
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
