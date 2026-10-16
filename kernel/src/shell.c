#include <stddef.h>
#include "shell.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"
#include "system/reboot.h"
#include "system/power.h"
#include "storage/storage_cmd.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "system/exceptions.h"
#include "system/interrupts.h"
#include "memory/heap.h"
#include "tests/tests.h"
#include "fs/vfs.h"
#include "version.h"
#include "ush.h"
#include "memory.h"

static int same(const char *a, const char *b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static void prompt(void) {
    terminal_write_color(ush_confirmation_pending() ? ush_confirmation_prompt() : "ultrium> ", TERMINAL_ACCENT);
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

static void power_status_command(void) { power_status(); }
static void shutdown_command(void) { power_shutdown(); }
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
#define A(n,d,u,o,lo,hi) {n,"",d,u,o,0,lo,hi,true}
static const struct ush_command registry[] = {
    G("help", "", "Show help; also accepts a command path"),
    B("clear", "", "Clear screen", clear),
    B("history", "clear", "Erase command history", clear_history),
    B("reboot", "", "Restart the machine", reboot),
    B("shutdown", "", "Power off the machine (ACPI)", shutdown_command),
    N("list","","List folder contents","[path]","list",0,1),
    N("go","","Change current folder","path","go",1,1),
    N("where","","Show current folder","","where",0,0),
    G("new","","Create an item"),
    N("file","new","Create an empty file","path","new file",1,1),
    N("folder","new","Create a folder","path","new folder",1,1),
    N("view","","Display a file","path","view",1,1),
    N("write","","Create a file containing text","path text","write",2,15),
    N("remove","","Remove an item; recursion requires -r","path [-r|--recursive] [-y|--yes]","remove",1,5),
    N("info","","Show stored metadata","path","info",1,1),
    N("copy","","Copy a file; refuse existing destination","source destination","copy",2,2),
    N("move","","Move an item; refuse existing destination","source destination","move",2,2),
    N("rename","","Rename an item to an exact destination path","source destination","rename",2,2),
    G("file","","Structured file operations"),
    N("copy","file","Copy a file","source destination","copy",2,2),
    N("move","file","Move an item","source destination","move",2,2),
    N("rename","file","Rename an item","source destination","rename",2,2),
    N("new","file","Create an empty file","path","new file",1,1),
    N("view","file","Display a file","path","view",1,1),
    N("write","file","Create a file containing text","path text","write",2,15),
    N("remove","file","Remove an item","path [-r] [-y]","remove",1,5),
    N("info","file","Show stored metadata","path","info",1,1),
    G("folder","","Structured folder operations"),
    N("new","folder","Create a folder","path","new folder",1,1),
    N("list","folder","List folder contents","[path]","list",0,1),
    N("go","folder","Change current folder","path","go",1,1),
    N("remove","folder","Remove an item","path [-r] [-y]","remove",1,5),
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
    G("power","ultrium system","Power management"),
    B("status","ultrium system power","ACPI power-off support",power_status_command),
    B("shutdown","ultrium system power","Power off the machine (ACPI)",shutdown_command),
    B("exceptions","ultrium system","Exception and IST status",exception_status),
    B("interrupts","ultrium system","APIC and timer status",interrupts_status),
    G("test","ultrium","Kernel test suites"),
    B("memory","ultrium test","Memory and exceptions",kernel_tests_memory),
    B("interrupts","ultrium test","Timer and keyboard IRQs",kernel_tests_interrupts),
    B("filesystem","ultrium test","VFS and RAMFS",kernel_tests_filesystem),
    B("storage","ultrium test","Block-device tests (memory-backed)",kernel_tests_storage),
    G("disk","ultrium test storage","Scratch virtio disk tests (writes to a scratch disk only)"),
    B("run","ultrium test storage disk","Run virtio disk tests",kernel_tests_storage_disk),
    B("ultriumfs","ultrium test","UltriumFS tests (memory-backed devices)",kernel_tests_ultriumfs),
    G("persist","ultrium test","Multi-boot persistence checks (need a mounted scratch volume)"),
    B("blank","ultrium test persist","Unformatted disk is rejected",kernel_tests_persist_blank),
    B("populate","ultrium test persist","Write multi-block fixture files",kernel_tests_persist_populate),
    B("stage1","ultrium test persist","Verify the boot-one fixture",kernel_tests_persist_stage1),
    B("modify","ultrium test persist","Update, grow and remove fixture files",kernel_tests_persist_modify),
    B("stage3","ultrium test persist","Verify the boot-two changes",kernel_tests_persist_stage3),
    B("clean","ultrium test persist","Previous session ended cleanly",kernel_tests_persist_clean),
    B("unclean","ultrium test persist","Abrupt stop is reported",kernel_tests_persist_unclean),
    B("v1fixture","ultrium test persist","Build an UltriumFS 1 volume (explicit v1 format)",kernel_tests_persist_v1fixture),
    B("v1verify","ultrium test persist","Verify a v1 volume mounted read-only",kernel_tests_persist_v1verify),
    B("ufs2","ultrium test","UltriumFS 2 journal and crash-recovery tests",kernel_tests_ufs2),
    B("ufs2fs","ultrium test","UltriumFS 2 filesystem tests",kernel_tests_ufs2fs),
    B("ufs2crash","ultrium test","UltriumFS 2 recovery matrix (power cut at every I/O event; slow, not in 'all')",kernel_tests_ufs2crash),
    B("shell","ultrium test","USH command tests",kernel_tests_shell),
    B("begin","ultrium test shell","Begin bounded interactive test capture",kernel_tests_shell_interactive_begin),
    B("interactive","ultrium test shell","Verify QMP shell fixture and output",kernel_tests_shell_interactive),
    B("all","ultrium test","All kernel suites",kernel_tests_all),
    G("storage","ultrium","Block devices, UltriumFS volumes and mounts"),
    B("devices","ultrium storage","List block devices and capacity",storage_cmd_devices),
    N("volumes","ultrium storage","List mounted filesystems","","storage volumes",0,0),
    N("format","ultrium storage","Format a scratch disk as UltriumFS (destructive)","<device> [-y|--yes]","storage format",1,2),
    N("mount","ultrium storage","Mount an UltriumFS device on an empty folder","<device> <path> [-r|--read-only]","storage mount",2,3),
    N("unmount","ultrium storage","Synchronize and unmount a volume","<path>","storage unmount",1,1),
    N("sync","ultrium storage","Flush all mounted persistent volumes","","storage sync",0,0),
    N("check","ultrium storage","Verify an UltriumFS 2 volume; --repair applies conservative repairs","<device> [--repair]","storage check",1,2),
    N("info","ultrium storage","Show filesystem, space and mode for a path","<path>","storage info",1,1),
    G("ai","ultrium","AI namespace; services unavailable"),
    B("status","ultrium ai","Report service availability",ai_status),
    G("shell","ultrium","Ultrium Shell; auth/as planned and unavailable (no privilege boundary)"),
    B("version","ultrium shell","USH component version",ush_version),
    G("compatibility","ultrium shell","Session-only aliases; disabled at boot"),
    B("status","ultrium shell compatibility","Show alias availability",ush_compatibility_status),
    B("enable","ultrium shell compatibility","Enable selected aliases",ush_compatibility_enable),
    B("disable","ultrium shell compatibility","Disable aliases",ush_compatibility_disable),
    A("ls","list","[path]","list",0,1), A("cd","go","path","go",1,1), A("pwd","where","","where",0,0),
    A("mkdir","new folder","path","new folder",1,1), A("touch","new file","path","new file",1,1),
    A("cat","view","path","view",1,1), A("rm","remove","path [-r] [-y]","remove",1,5), A("cp","copy","source destination","copy",2,2), A("mv","move","source destination","move",2,2)
};
#define COMMAND_COUNT (sizeof(registry)/sizeof(registry[0]))
static const struct ush_command *child(const char *parent, const char *word) {
    for (size_t i = 0; i < COMMAND_COUNT; ++i)
        if (same(parent, registry[i].parent) && ush_word_equal(word, registry[i].name))
            return &registry[i];
    return 0;
}
static bool append_word(char *path, const char *word) {
    size_t n = 0, count = 0;
    while (path[n] && n < USH_INPUT_MAX) ++n;
    while (word[count] && count < USH_INPUT_MAX) ++count;
    if (n + (n ? 1U : 0U) + count > USH_INPUT_MAX) return false;
    if (n) path[n++] = ' ';
    for (size_t i = 0; i < count; ++i) path[n++] = word[i];
    path[n] = 0; return true;
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
        if (c->operation) {
            const char *operations[] = {"list","go","where","new file","new folder","view","write","remove","info","copy","move","rename",
                "storage volumes","storage format","storage mount","storage unmount","storage sync","storage info","storage check"};
            bool known = false;
            for (size_t j = 0; j < sizeof(operations)/sizeof(operations[0]); ++j)
                if (same(c->operation, operations[j])) known = true;
            if (!known) return false;
        }
        for (size_t j = 0; j < i; ++j)
            if (same(c->parent, registry[j].parent) && same(c->name, registry[j].name)) return false;
        if (!c->parent[0]) continue;
        bool found = false;
        for (size_t j = 0; j < COMMAND_COUNT; ++j) {
            char path[USH_INPUT_MAX + 1] = {0};
            if (!append_word(path, registry[j].parent) || !append_word(path, registry[j].name)) return false;
            if (same(path, c->parent)) found = true;
        }
        if (!found) return false;
    }
    return true;
}
enum ush_result ush_execute(const char *line) {
    if (ush_confirmation_pending()) return USH_PENDING;
    struct ush_args args;
    if (ush_tokenize(line, &args) != USH_OK) {
        terminal_write(args.error); terminal_write("\n"); return USH_USAGE;
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
        const char *suggestion = ush_suggest(path, args.values[used < args.count ? used : 0]);
        if (suggestion) { terminal_write("Did you mean: "); terminal_write(suggestion); terminal_write("?\n"); }
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
/* Completion is single-CPU shell work. Static scratch avoids growing the
   kernel stack with 128 path-sized candidates; no allocations or IRQ use. */
static char completion_candidates[VFS_NODE_MAX][VFS_PATH_MAX + 1U];
static size_t completion_count;
static size_t text_length(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
static bool starts_with(const char *text, const char *prefix, bool fold) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (!text[i]) return false;
        char a = text[i], b = prefix[i];
        if (fold) {
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        }
        if (a != b) return false;
    }
    return true;
}
const char *ush_suggest(const char *parent, const char *word) {
    size_t n = text_length(word);
    if (!n || n > VFS_NAME_MAX) return 0;
    unsigned best = 3;
    const char *suggestion = 0;
    for (size_t c = 0; c < COMMAND_COUNT; ++c) {
        const struct ush_command *command = &registry[c];
        if (!same(parent, command->parent) || (command->alias && !ush_compatibility())) continue;
        size_t m = text_length(command->name);
        if (m > VFS_NAME_MAX || n > m+2 || m > n+2) continue;
        unsigned previous[VFS_NAME_MAX+1], current[VFS_NAME_MAX+1];
        for (size_t j = 0; j <= m; ++j) previous[j] = (unsigned)j;
        for (size_t i = 1; i <= n; ++i) {
            current[0] = (unsigned)i;
            for (size_t j = 1; j <= m; ++j) {
                char a[2] = {word[i-1],0}, b[2] = {command->name[j-1],0};
                unsigned replace = previous[j-1] + !ush_word_equal(a,b);
                unsigned insert = current[j-1]+1, erase = previous[j]+1;
                current[j] = replace < insert ? replace : insert;
                if (erase < current[j]) current[j] = erase;
            }
            for (size_t j = 0; j <= m; ++j) previous[j] = current[j];
        }
        if (previous[m] < best) { best = previous[m]; suggestion = command->name; }
    }
    return suggestion;
}
static void completion_add(const char *prefix, const char *name, bool directory) {
    if (completion_count == VFS_NODE_MAX) return;
    size_t a = text_length(prefix), b = text_length(name);
    if (a+b+(directory ? 1U:0U) > VFS_PATH_MAX) return;
    char *out = completion_candidates[completion_count++];
    memcpy(out, prefix, a); memcpy(out+a, name, b);
    if (directory) out[a+b++] = '/';
    out[a+b] = 0;
}
void ush_completion_show(void) {
    for (size_t i = 0; i < completion_count; ++i) {
        terminal_write("  "); terminal_write(completion_candidates[i]); terminal_write("\n");
    }
}
enum ush_completion ush_complete(char line[USH_INPUT_MAX + 1U], size_t *caret) {
    size_t size = text_length(line);
    if (*caret > size || size > USH_INPUT_MAX) return USH_COMPLETE_NONE;
    completion_count = 0;
    char prefix[USH_INPUT_MAX+1];
    memcpy(prefix, line, *caret); prefix[*caret] = 0;
    struct ush_args args;
    if (ush_tokenize_prefix(prefix, &args) != USH_OK) return USH_COMPLETE_NONE;
    bool empty = !args.count || args.trailing_space;
    size_t before = empty ? args.count : args.count-1;
    const char *fragment = empty ? "" : args.values[before];
    size_t start = empty ? *caret : args.starts[before];
    char parent[USH_INPUT_MAX+1] = {0};
    const struct ush_command *command = 0;
    size_t resolved = 0;
    if (before && ush_word_equal(args.values[0], "help")) resolved = 1;
    for (; resolved < before; ++resolved) {
        const struct ush_command *next = child(parent, args.values[resolved]);
        if (!next) break;
        command = next; append_word(parent, command->name);
    }
    bool command_position = resolved == before && (!command || !command->operation);
    if (command_position) {
        for (size_t i = 0; i < COMMAND_COUNT; ++i) {
            const struct ush_command *candidate = &registry[i];
            if (same(parent, candidate->parent) && (!candidate->alias || ush_compatibility()) &&
                starts_with(candidate->name, fragment, true)) completion_add("", candidate->name, false);
        }
    }
    if (!completion_count && command && command->operation) {
        size_t argument = before - resolved;
        bool transfer = same(command->operation,"copy") || same(command->operation,"move") || same(command->operation,"rename");
        if (argument > (transfer ? 1U:0U) || same(command->operation,"where") || fragment[0] == '-')
            return USH_COMPLETE_NONE;
        size_t split = 0;
        for (size_t i = 0; fragment[i]; ++i) if (fragment[i] == '/') split = i+1;
        char directory[VFS_PATH_MAX+1], raw_directory[VFS_PATH_MAX+1], path_prefix[VFS_PATH_MAX+1];
        memcpy(path_prefix, fragment, split); path_prefix[split] = 0;
        if (!split) { raw_directory[0] = '.'; raw_directory[1] = 0; }
        else { memcpy(raw_directory, fragment, split); raw_directory[split] = 0; }
        if (vfs_resolve_path(raw_directory, ush_cwd(), directory, sizeof(directory)) != VFS_OK)
            return USH_COMPLETE_NONE;
        struct vfs_dir dir = {0};
        if (vfs_dir_open(directory, &dir) != VFS_OK) return USH_COMPLETE_NONE;
        char name[VFS_NAME_MAX+1]; enum vfs_node_type type;
        bool directories = same(command->operation,"go") || same(command->operation,"list");
        while (vfs_dir_next(&dir, name, sizeof(name), &type) == VFS_OK)
            if ((!directories || type == VFS_NODE_DIRECTORY) && starts_with(name, fragment+split, false))
                completion_add(path_prefix, name, type == VFS_NODE_DIRECTORY);
        vfs_dir_close(&dir);
    }
    if (!completion_count) return USH_COMPLETE_NONE;
    size_t common = text_length(completion_candidates[0]);
    for (size_t i = 1; i < completion_count; ++i) {
        size_t j = 0;
        while (j < common && completion_candidates[0][j] == completion_candidates[i][j]) ++j;
        common = j;
    }
    if (completion_count > 1 && common <= text_length(fragment)) return USH_COMPLETE_MULTIPLE;
    /* Replace the complete raw token, preserving subsequent arguments and
       normalizing quoted/escaped spelling to a safely encoded token. */
    size_t end = start; char quote = 0;
    while (line[end]) {
        char c = line[end];
        if (c == '\\' && line[end+1]) { end += 2; continue; }
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == ' ') break;
        ++end;
    }
    char encoded[VFS_PATH_MAX*2+4]; size_t used = 0;
    bool quoted = false;
    for (size_t i = 0; i < common; ++i) {
        char c = completion_candidates[0][i];
        if (c == ' ' || c == '\'' || c == '"' || c == '\\') quoted = true;
    }
    if (quoted) encoded[used++] = '"';
    for (size_t i = 0; i < common; ++i) {
        char c = completion_candidates[0][i];
        if (c == '"' || c == '\\') encoded[used++] = '\\';
        encoded[used++] = c;
    }
    if (quoted) encoded[used++] = '"';
    if (completion_count == 1 && (!common || completion_candidates[0][common-1] != '/') && !line[end])
        encoded[used++] = ' ';
    if (start + used + size - end > USH_INPUT_MAX) return USH_COMPLETE_LIMIT;
    memmove(line + start + used, line + end, size - end + 1);
    memcpy(line + start, encoded, used); *caret = start + used;
    return completion_count == 1 ? USH_COMPLETE_UNIQUE : USH_COMPLETE_MULTIPLE;
}

/* Expanded preallocated history pending a real memory allocator and filesystem.
   Limit command length to 70 so input stays on one line at common QEMU sizes. */
#define INPUT_LIMIT USH_INPUT_MAX
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
    bool overflow = false, tab_again = false;
    prompt();
    terminal_input_begin();
    terminal_cursor(1);
    for (;;) {
        int key = keyboard_poll();
        if (!key) continue;
        terminal_cursor(0);
        if (key != '\t') tab_again = false;
        if (key == KEY_CTRL_C) {
            terminal_input_redraw(line, (unsigned)len, (unsigned)len);
            terminal_write("^C");
            terminal_newline();
            if (ush_confirmation_pending()) (void)ush_answer_confirmation("");
            len = caret = 0;
            overflow = false;
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
        } else if (key == '\t') {
            if (!ush_confirmation_pending() && !overflow) {
                enum ush_completion result = ush_complete(line, &caret);
                len = line_length(line);
                if ((tab_again && result == USH_COMPLETE_MULTIPLE) || result == USH_COMPLETE_LIMIT) {
                    terminal_input_redraw(line, (unsigned)len, (unsigned)len);
                    terminal_newline();
                    if (result == USH_COMPLETE_LIMIT) terminal_write("Completion exceeds input limit; input preserved.\n");
                    else ush_completion_show();
                    prompt(); terminal_input_begin();
                }
                tab_again = true;
            }
        } else if (key == KEY_UP) {
            overflow = false;
            if (selected > 0) replace_input(line, &len, &caret, history[--selected]);
        } else if (key == KEY_DOWN) {
            overflow = false;
            if (selected < history_size) {
                ++selected;
                replace_input(line, &len, &caret,
                    selected == history_size ? "" : history[selected]);
            }
        } else if (key == KEY_CTRL_U) {
            replace_input(line, &len, &caret, "");
            overflow = false;
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
            if (overflow) {
                terminal_write("Input exceeds 70 characters; command rejected.\n");
                if (ush_confirmation_pending()) (void)ush_answer_confirmation("");
            } else if (ush_confirmation_pending()) (void)ush_answer_confirmation(line);
            else {
                if (!same(line, "clear history")) save_history(line);
                dispatch(line);
            }
            len = caret = 0;
            overflow = false;
            line[0] = 0;
            selected = history_size;
            prompt();
            terminal_input_begin();
        } else if (key >= 32 && key <= 126 && len == INPUT_LIMIT) {
            overflow = true;
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
