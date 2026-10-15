#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"
#include "system/reboot.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "system/exceptions.h"
#include "system/interrupts.h"
#include "memory/heap.h"
#include "tests/tests.h"
#include "fs/vfs.h"
#include "version.h"
#include "ush.h"

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
        line[write++] = ch;
    }
    line[write] = 0;
    size_t first_length = 0;
    while (line[first_length] && line[first_length] != ' ') ++first_length;
    for (size_t i = 0; i < first_length; ++i)
        if (line[i] >= 'A' && line[i] <= 'Z')
            line[i] = (char)(line[i] + ('a' - 'A'));
    bool namespace_command =
        (first_length == 7 && line[0] == 'u' && line[1] == 'l' && line[2] == 't' &&
         line[3] == 'r' && line[4] == 'i' && line[5] == 'u' && line[6] == 'm') ||
        (first_length == 5 && line[0] == 'c' && line[1] == 'l' && line[2] == 'e' &&
         line[3] == 'a' && line[4] == 'r');
    for (size_t i = first_length; namespace_command && line[i]; ++i)
        if (line[i] >= 'A' && line[i] <= 'Z')
            line[i] = (char)(line[i] + ('a' - 'A'));
}
static void prompt(void) { terminal_write_color("ultrium> ", TERMINAL_ACCENT); }
static void version(void) { terminal_write("Ultrium OS " ULTRIUM_VERSION "\n"); }
static void system_info(void) {
    terminal_write("OS: Ultrium OS\nKernel: " ULTRIUM_VERSION "\nArchitecture: x86_64\nMode: CLI\n");
}
static void print_uint(uint64_t value) {
    char digits[21];
    size_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) terminal_putc_color(digits[--n], TERMINAL_NORMAL);
}
static void memory_info(void) {
    struct vmm_region_stats regions;
    terminal_write("Usable RAM: ");
    print_uint(pmm_total_bytes() / (1024 * 1024));
    terminal_write(" MiB\nFree pages: ");
    print_uint(pmm_free_pages());
    terminal_write("\nAllocated pages: ");
    print_uint(pmm_used_pages());
    terminal_write("\nHeap allocations: ");
    print_uint(heap_live_allocations());
    terminal_write("\nPage size: 4096 bytes\n");
    if (vmm_region_stats_get(&regions)) {
        terminal_write("VM region reserved pages: "); print_uint(regions.reserved_virtual_pages);
        terminal_write("\nVM region mapped pages: "); print_uint(regions.mapped_virtual_pages);
        terminal_write("\nVM region backing pages: "); print_uint(regions.owned_backing_pages);
        terminal_write("\nVM region free pages: "); print_uint(regions.free_virtual_pages);
        terminal_write("\nVM region page tables: "); print_uint(regions.page_table_frames);
        terminal_write("\n");
    }
}
static void memory_test(void) { kernel_test_slab_stress(); }
static void memory_map_test(void) {
    terminal_write(vmm_self_test() ? "PAGE TABLE TEST PASSED\n"
                                   : "PAGE TABLE TEST FAILED\n");
}
static void exception_status(void) {
    if (exceptions_self_test())
        terminal_write("CPU exceptions: vectors 0-31; TSS loaded; double-fault IST ready\n");
    else
        terminal_write(exceptions_ready() ?
            "CPU exception handlers installed; TSS/IST check failed\n" :
            "CPU exception handlers: unavailable\n");
}
static void interrupts_status(void) {
    if (!interrupts_ready()) {
        terminal_write("Hardware interrupts unavailable; keyboard remains polled.\n");
        return;
    }
    terminal_write("Controller: local APIC + IOAPIC\nPIT frequency: 100 Hz\nTimer ticks: ");
    print_uint(interrupts_timer_ticks());
    terminal_write("\n");
}
static void mapping_test(void) {
    bool passed = vmm_map_test();
    terminal_write(passed ? "VIRTUAL MAPPING TEST PASSED\n" : "VIRTUAL MAPPING TEST FAILED\n");
    if (!passed) { terminal_write("Failure stage: "); print_uint(vmm_test_error()); terminal_write("\n"); }
}
static void managed_test(void) {
    bool passed = vmm_managed_test();
    terminal_write(passed ? "MANAGED VIRTUAL PAGES TEST PASSED\n" : "MANAGED VIRTUAL PAGES TEST FAILED\n");
    if (!passed) { terminal_write("Failure stage: "); print_uint(vmm_test_error()); terminal_write("\n"); }
}
static void ai_status(void) { terminal_write("AI services are not installed.\n"); }
static void clear(void) { terminal_clear(); }
static void clear_history(void);

static void reboot(void) { terminal_write("Rebooting...\n"); system_reboot(); }

/* Static metadata is the single source for routing, help and discovery.
   Parent paths are exact namespaces. An operation names a shared VFS handler;
   a builtin invokes existing kernel diagnostics without rewriting them. */
struct ush_command {
    const char *name, *parent, *description, *usage, *operation;
    void (*builtin)(void);
    unsigned minimum, maximum;
    bool alias;
};
#define G(n,p,d) {n,p,d,"",0,0,0,0,false}
#define N(n,p,d,u,o,lo,hi) {n,p,d,u,o,0,lo,hi,false}
#define B(n,p,d,f) {n,p,d,"",0,f,0,0,false}
#define A(n,d,o,lo,hi) {n,"",d,"",o,0,lo,hi,true}
static const struct ush_command registry[] = {
    G("help", "", "Show help; also accepts a command path"),
    B("clear", "", "Clear screen", clear),
    B("history", "clear", "Erase command history", clear_history),
    B("reboot", "", "Restart the machine", reboot),
    N("list","","List folder contents","[path]","list",0,1),
    N("go","","Change current folder","path","go",1,1),
    N("where","","Show current folder","","where",0,0),
    G("new","","Create an item"),
    N("file","new","Create an empty file","path","new file",1,1),
    N("folder","new","Create a folder","path","new folder",1,1),
    N("view","","Display a file","path","view",1,1),
    N("write","","Create a file containing text","path text","write",2,15),
    N("remove","","Remove a file or empty folder","path","remove",1,1),
    N("info","","Show stored metadata","path","info",1,1),
    G("file","","Structured file operations"),
    N("new","file","Create an empty file","path","new file",1,1),
    N("view","file","Display a file","path","view",1,1),
    N("write","file","Create a file containing text","path text","write",2,15),
    N("remove","file","Remove an item","path","remove",1,1),
    N("info","file","Show stored metadata","path","info",1,1),
    G("folder","","Structured folder operations"),
    N("new","folder","Create a folder","path","new folder",1,1),
    N("list","folder","List folder contents","[path]","list",0,1),
    N("go","folder","Change current folder","path","go",1,1),
    N("remove","folder","Remove an item","path","remove",1,1),
    N("info","folder","Show stored metadata","path","info",1,1),
    G("ultrium","","Ultrium OS administration"),
    B("version","ultrium","Kernel version",version),
    G("system","ultrium","System diagnostics"),
    B("info","ultrium system","System information",system_info),
    B("memory","ultrium system","Memory status",memory_info),
    B("test","ultrium system memory","Slab stress test",memory_test),
    G("map","ultrium system memory","Page table inspection"),
    B("test","ultrium system memory map","Page table test",memory_map_test),
    G("mapping","ultrium system memory","Virtual mapping diagnostics"),
    B("test","ultrium system memory mapping","Scratch mapping test",mapping_test),
    G("managed","ultrium system memory mapping","Owned virtual pages"),
    B("test","ultrium system memory mapping managed","Managed pages test",managed_test),
    B("exceptions","ultrium system","Exception and IST status",exception_status),
    B("interrupts","ultrium system","APIC and timer status",interrupts_status),
    G("test","ultrium","Kernel test suites"),
    B("memory","ultrium test","Memory and exceptions",kernel_tests_memory),
    B("interrupts","ultrium test","Timer and keyboard IRQs",kernel_tests_interrupts),
    B("filesystem","ultrium test","VFS and RAMFS",kernel_tests_filesystem),
    B("shell","ultrium test","USH command tests",kernel_tests_shell),
    B("all","ultrium test","All kernel suites",kernel_tests_all),
    G("ai","ultrium","AI namespace; services unavailable"),
    B("status","ultrium ai","Report service availability",ai_status),
    G("shell","ultrium","Ultrium Shell (USH)"),
    B("version","ultrium shell","USH component version",ush_version),
    G("compatibility","ultrium shell","Session-only aliases; disabled at boot"),
    B("status","ultrium shell compatibility","Show alias availability",ush_compatibility_status),
    B("enable","ultrium shell compatibility","Enable selected aliases",ush_compatibility_enable),
    B("disable","ultrium shell compatibility","Disable aliases",ush_compatibility_disable),
    A("ls","list","list",0,1), A("cd","go","go",1,1), A("pwd","where","where",0,0),
    A("mkdir","new folder","new folder",1,1), A("touch","new file","new file",1,1),
    A("cat","view","view",1,1), A("rm","remove","remove",1,1)
};
#define COMMAND_COUNT (sizeof(registry)/sizeof(registry[0]))
static const struct ush_command *child(const char *parent, const char *word) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i)
        if (same(parent, registry[i].parent) && ush_word_equal(word, registry[i].name))
            return &registry[i];
    return 0;
}
static void append_word(char *path, const char *word) {
    size_t n = 0; while (path[n]) ++n;
    if (n) path[n++] = ' ';
    while (*word) path[n++] = *word++;
    path[n] = 0;
}
static void command_help(const struct ush_command *command, const char *path) {
    if (command) {
        terminal_write(path); terminal_write(" "); terminal_write(command->usage);
        terminal_write("\n"); terminal_write(command->description); terminal_write("\n");
    } else terminal_write("Ultrium Shell (USH) " USH_VERSION " commands:\n");
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        const struct ush_command *c = &registry[i];
        if (!same(path, c->parent) || (c->alias && !ush_compatibility())) continue;
        terminal_write("  "); terminal_write(c->name); terminal_write(" ");
        terminal_write(c->usage); terminal_write("  - "); terminal_write(c->description);
        terminal_write("\n");
    }
    terminal_write("Use '<command path> help' or 'help <command path>'.\n");
}
bool ush_registry_valid(void) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i) {
        const struct ush_command *c = &registry[i];
        if (!c->name[0] || c->minimum > c->maximum || (c->operation && c->builtin)) return false;
        for (size_t j = 0; j < i; ++j)
            if (same(c->parent, registry[j].parent) && same(c->name, registry[j].name)) return false;
        if (!c->parent[0]) continue;
        bool found = false;
        for (size_t j = 0; j < COMMAND_COUNT; ++j) {
            char path[USH_INPUT_MAX + 1] = {0};
            append_word(path, registry[j].parent); append_word(path, registry[j].name);
            if (same(path, c->parent)) found = true;
        }
        if (!found) return false;
    }
    return true;
}
enum ush_result ush_execute(const char *line) {
    struct ush_args args;
    if (ush_tokenize(line, &args) != USH_OK) {
        terminal_write("Command exceeds input or argument limit.\n"); return USH_USAGE;
    }
    if (!args.count) return USH_OK;
    char path[USH_INPUT_MAX + 1] = {0};
    size_t used = 0;
    bool prefix_help = ush_word_equal(args.values[0], "help");
    if (prefix_help) ++used;
    const struct ush_command *command = 0;
    while (used < args.count) {
        const struct ush_command *next = child(path, args.values[used]);
        if (!next) break;
        command = next; append_word(path, command->name); ++used;
    }
    if (command && command->alias && !ush_compatibility()) {
        terminal_write("Command '"); terminal_write(command->name);
        terminal_write("' is unavailable in native mode.\nUse '");
        terminal_write(command->description); terminal_write("' instead.\n");
        return USH_UNAVAILABLE;
    }
    bool suffix_help = used + 1 == args.count && ush_word_equal(args.values[used], "help");
    if ((prefix_help && used == args.count) || suffix_help ||
        (command && used == args.count && !command->operation && !command->builtin)) {
        command_help(command, path); return USH_OK;
    }
    if (!command || (prefix_help && used != args.count) ||
        (!command->operation && !command->builtin)) {
        terminal_write("Unknown command: "); terminal_write(args.values[used < args.count ? used : 0]);
        terminal_write(". Use 'help' or '<namespace> help'.\n");
        return USH_UNKNOWN;
    }
    size_t count = args.count - used;
    if (count < command->minimum || count > command->maximum) {
        terminal_write("Invalid arguments. Usage: "); terminal_write(path); terminal_write(" ");
        terminal_write(command->usage); terminal_write("\n"); return USH_USAGE;
    }
    if (command->operation) return ush_operation(command->operation, count, args.values + used);
    command->builtin(); return USH_OK;
}
static void dispatch(const char *line) { (void)ush_execute(line); }
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

static void replace_input(char *line, size_t *len, size_t *caret, const char *text) {
    copy_line(line, text);
    *len = line_length(line);
    *caret = *len;
    terminal_input_redraw(line, (unsigned)*len, (unsigned)*caret);
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
    size_t len = 0, caret = 0, selected = 0;
    prompt();
    terminal_input_begin();
    terminal_cursor(1);
    for (;;) {
        int key = keyboard_poll();
        if (!key) continue;
        terminal_cursor(0);
        if (key == KEY_CTRL_C) {
            terminal_input_redraw(line, (unsigned)len, (unsigned)len);
            terminal_write("^C");
            terminal_newline();
            len = caret = 0;
            line[0] = 0;
            selected = history_size;
            prompt();
            terminal_input_begin();
        } else if (key == KEY_CTRL_L) {
            terminal_clear();
            prompt();
            terminal_input_begin();
            terminal_input_redraw(line, (unsigned)len, (unsigned)caret);
        } else if (key == KEY_LEFT) {
            if (caret) --caret;
        } else if (key == KEY_RIGHT) {
            if (caret < len) ++caret;
        } else if (key == KEY_HOME) {
            caret = 0;
        } else if (key == KEY_END) {
            caret = len;
        } else if (key == KEY_UP) {
            if (selected > 0) replace_input(line, &len, &caret, history[--selected]);
        } else if (key == KEY_DOWN) {
            if (selected < history_size) {
                ++selected;
                replace_input(line, &len, &caret,
                    selected == history_size ? "" : history[selected]);
            }
        } else if (key == KEY_CTRL_U) {
            replace_input(line, &len, &caret, "");
            selected = history_size;
        } else if (key == KEY_CTRL_W) {
            size_t start = caret;
            while (start && line[start - 1] == ' ') --start;
            while (start && line[start - 1] != ' ') --start;
            if (start != caret) {
                size_t removed = caret - start;
                for (size_t i = start; i + removed <= len; ++i)
                    line[i] = line[i + removed];
                len -= removed; caret = start;
            }
            selected = history_size;
        } else if (key == KEY_DELETE || key == '\b') {
            bool erase = key == KEY_DELETE ? caret < len : caret > 0;
            if (key == '\b' && caret) --caret;
            if (erase) {
                for (size_t i = caret; i < len; ++i) line[i] = line[i + 1];
                --len;
            }
            selected = history_size;
        } else if (key == '\n') {
            terminal_input_redraw(line, (unsigned)len, (unsigned)len);
            terminal_newline();
            normalize(line);
            if (!same(line, "clear history")) save_history(line);
            dispatch(line);
            len = caret = 0;
            line[0] = 0;
            selected = history_size;
            prompt();
            terminal_input_begin();
        } else if (key >= 32 && key <= 126 && len < INPUT_LIMIT) {
            for (size_t i = len + 1; i > caret; --i) line[i] = line[i - 1];
            line[caret++] = (char)key;
            ++len;
            selected = history_size;
        }
        terminal_input_redraw(line, (unsigned)len, (unsigned)caret);
        terminal_cursor(1);
    }
}
