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

/* Built-in Ultrium commands and future external programs have separate dispatch paths. */
typedef void (*command_handler)(void);
struct command { const char *name; command_handler handler; };

static int same(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static char shell_cwd[VFS_PATH_MAX + 1U] = "/";
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
static void help(void) {
    terminal_write("Prefixes:\n");
    terminal_write("  ultrium     Ultrium OS commands (ultrium help)\n");
    terminal_write("  ultrium ai  AI commands (ultrium ai help)\n");
    terminal_write("  clear       Clear commands (clear help)\n");
    terminal_write("General commands:\n");
    terminal_write("  help, reboot, pwd, ls, cd, mkdir, touch, write, cat, rm\n");
    terminal_write("Shortcuts: Left/Right, Home/End, Delete, Ctrl+C/U/W/L\n");
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
    terminal_write("  ultrium test help  (Kernel tests)\n");
    terminal_write("  ultrium ai help  (AI namespace)\n");
}
static void system_help(void) {
    terminal_write("Ultrium system commands:\n");
    terminal_write("  ultrium system help\n");
    terminal_write("  ultrium system info\n");
    terminal_write("  ultrium system memory help  (Memory namespace)\n");
    terminal_write("  ultrium system exceptions\n");
    terminal_write("  ultrium system interrupts help  (Interrupt status)\n");
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
    terminal_write("  ultrium system memory mapping managed test\n");
}
static void ai_help(void) {
    terminal_write("Ultrium AI commands:\n");
    terminal_write("  ultrium ai help\n");
    terminal_write("  ultrium ai status\n");
    terminal_write("AI services are not installed.\n");
}
static void test_help(void) {
    terminal_write("Kernel test commands:\n");
    terminal_write("  ultrium test help\n");
    terminal_write("  ultrium test memory\n");
    terminal_write("  ultrium test interrupts\n");
    terminal_write("  ultrium test interrupts help  (Interrupt tests)\n");
    terminal_write("  ultrium test filesystem help  (Filesystem tests)\n");
    terminal_write("  ultrium test filesystem\n");
    terminal_write("  ultrium test all\n");
}
static void test_interrupts_help(void) {
    terminal_write("Interrupt test commands:\n");
    terminal_write("  ultrium test interrupts help\n");
    terminal_write("  ultrium test interrupts\n");
}
static void test_filesystem_help(void) {
    terminal_write("Filesystem test commands:\n");
    terminal_write("  ultrium test filesystem help\n");
    terminal_write("  ultrium test filesystem\n");
}
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
static void interrupts_help(void) {
    terminal_write("Interrupt status commands:\n");
    terminal_write("  ultrium system interrupts help\n");
    terminal_write("  ultrium system interrupts\n");
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

static const char *skip_spaces(const char *text) {
    while (*text == ' ') ++text;
    return text;
}

static bool command_word(const char *line, const char *word,
                        const char **arguments) {
    size_t i = 0;
    while (word[i] && line[i] == word[i]) ++i;
    if (word[i] || (line[i] && line[i] != ' ')) return false;
    if (arguments) *arguments = skip_spaces(line + i);
    return true;
}

static enum vfs_error shell_path(const char *argument,
                                 char path[VFS_PATH_MAX + 1U]) {
    return vfs_normalize(argument, shell_cwd, path, VFS_PATH_MAX + 1U);
}

static void fs_error(enum vfs_error error) {
    terminal_write("Filesystem operation failed: ");
    switch (error) {
        case VFS_NOT_FOUND: terminal_write("not found\n"); break;
        case VFS_EXISTS: terminal_write("already exists\n"); break;
        case VFS_NOT_DIRECTORY: terminal_write("not a directory\n"); break;
        case VFS_IS_DIRECTORY: terminal_write("is a directory\n"); break;
        case VFS_NO_SPACE: terminal_write("out of memory or capacity\n"); break;
        case VFS_INVALID_PATH: terminal_write("invalid path\n"); break;
        default: terminal_write("unsupported or invalid operation\n"); break;
    }
}

static void fs_help(const char *command) {
    terminal_write(command);
    terminal_write(" usage: ");
    terminal_write(command);
    if (same(command, "pwd")) terminal_write("\n");
    else if (same(command, "ls")) terminal_write(" [path]\n");
    else terminal_write(" path\n");
}

static bool dispatch_filesystem(const char *line) {
    const char *arguments;
    char path[VFS_PATH_MAX + 1U];
    if (command_word(line, "pwd", &arguments)) {
        if (same(arguments, "help")) { fs_help("pwd"); return true; }
        if (*arguments) { fs_error(VFS_INVALID_PATH); return true; }
        terminal_write(shell_cwd); terminal_write("\n");
        return true;
    }
    if (command_word(line, "ls", &arguments)) {
        if (same(arguments, "help")) { fs_help("ls"); return true; }
        if (!*arguments) {
            size_t i = 0; do { path[i] = shell_cwd[i]; } while (shell_cwd[i++]);
        } else if (shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        struct vfs_dir dir = {0};
        enum vfs_error error = vfs_dir_open(path, &dir);
        if (error != VFS_OK) { fs_error(error); return true; }
        char name[VFS_NAME_MAX + 1U];
        enum vfs_node_type type;
        while ((error = vfs_dir_next(&dir, name, sizeof(name), &type)) == VFS_OK) {
            terminal_write(name);
            if (type == VFS_NODE_DIRECTORY) terminal_write("/");
            terminal_write("\n");
        }
        if (error != VFS_END) fs_error(error);
        vfs_dir_close(&dir);
        return true;
    }
    if (command_word(line, "cd", &arguments)) {
        if (same(arguments, "help")) { fs_help("cd"); return true; }
        if (!*arguments || shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        if (!vfs_is_directory(path)) { fs_error(VFS_NOT_DIRECTORY); return true; }
        size_t i = 0; do { shell_cwd[i] = path[i]; } while (path[i++]);
        return true;
    }
    if (command_word(line, "mkdir", &arguments)) {
        if (same(arguments, "help")) { fs_help("mkdir"); return true; }
        if (!*arguments || shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        enum vfs_error error = vfs_create_directory(path);
        if (error != VFS_OK) fs_error(error);
        return true;
    }
    if (command_word(line, "touch", &arguments)) {
        if (same(arguments, "help")) { fs_help("touch"); return true; }
        if (!*arguments || shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        enum vfs_error error = vfs_create_file(path);
        if (error == VFS_EXISTS) {
            if (vfs_is_directory(path)) fs_error(VFS_IS_DIRECTORY);
            else error = VFS_OK;
        }
        if (error != VFS_OK) fs_error(error);
        return true;
    }
    if (command_word(line, "write", &arguments)) {
        if (same(arguments, "help")) {
            terminal_write("write usage: write path text (creates a new file)\n");
            return true;
        }
        size_t name_length = 0;
        while (arguments[name_length] && arguments[name_length] != ' ') ++name_length;
        const char *contents = skip_spaces(arguments + name_length);
        if (!name_length || name_length > VFS_PATH_MAX || !*contents) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        char name[VFS_PATH_MAX + 1U];
        for (size_t i = 0; i < name_length; ++i) name[i] = arguments[i];
        name[name_length] = 0;
        if (shell_path(name, path) != VFS_OK) { fs_error(VFS_INVALID_PATH); return true; }
        enum vfs_error error = vfs_create_file(path);
        if (error != VFS_OK) { fs_error(error); return true; }
        struct vfs_file file = {0};
        error = vfs_file_open(path, VFS_OPEN_WRITE, &file);
        size_t written = 0, content_length = 0;
        while (contents[content_length]) ++content_length;
        if (error == VFS_OK)
            error = vfs_file_write(&file, contents, content_length, &written);
        if (file.generation && vfs_file_close(&file) != VFS_OK) error = VFS_BAD_HANDLE;
        if (error != VFS_OK || written != content_length) {
            vfs_unlink(path);
            fs_error(error == VFS_OK ? VFS_NO_SPACE : error);
        }
        return true;
    }
    if (command_word(line, "rm", &arguments)) {
        if (same(arguments, "help")) { fs_help("rm"); return true; }
        if (!*arguments || shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        enum vfs_error error = vfs_unlink(path);
        if (error != VFS_OK) fs_error(error);
        return true;
    }
    if (command_word(line, "cat", &arguments)) {
        if (same(arguments, "help")) { fs_help("cat"); return true; }
        if (!*arguments || shell_path(arguments, path) != VFS_OK) {
            fs_error(VFS_INVALID_PATH); return true;
        }
        struct vfs_file file = {0};
        enum vfs_error error = vfs_file_open(path, VFS_OPEN_READ, &file);
        if (error != VFS_OK) { fs_error(error); return true; }
        char buffer[128];
        size_t count;
        while ((error = vfs_file_read(&file, buffer, sizeof(buffer), &count)) == VFS_OK && count)
            for (size_t i = 0; i < count; ++i) terminal_putc_color(buffer[i], TERMINAL_NORMAL);
        if (error != VFS_OK) fs_error(error);
        vfs_file_close(&file);
        return true;
    }
    return false;
}

static const struct command commands[] = {
    {"help", ultrium_help}, {"version", version},
    {"system help", system_help}, {"system info", system_info},
    {"system memory", memory_info}, {"system memory help", memory_help},
    {"system memory map help", memory_map_help},
    {"system memory mapping help", memory_mapping_help},
    {"system memory test", memory_test},
    {"system memory map test", memory_map_test},
    {"system memory mapping test", mapping_test},
    {"system memory mapping managed test", managed_test},
    {"system exceptions", exception_status},
    {"system interrupts help", interrupts_help},
    {"system interrupts", interrupts_status},
    {"test help", test_help}, {"test memory", kernel_tests_memory},
    {"test interrupts help", test_interrupts_help},
    {"test interrupts", kernel_tests_interrupts},
    {"test filesystem help", test_filesystem_help},
    {"test filesystem", kernel_tests_filesystem},
    {"test all", kernel_tests_all},
    {"ai help", ai_help}, {"ai status", ai_status},
};
static void dispatch(const char *line) {
    if (!*line) return;
    if (same(line, "help")) { help(); return; }
    if (same(line, "clear")) { clear(); return; }
    if (same(line, "clear help")) { clear_help(); return; }
    if (same(line, "clear history")) { clear_history(); return; }
    if (dispatch_filesystem(line)) return;
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
