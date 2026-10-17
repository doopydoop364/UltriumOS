#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "processes/process_cmd.h"
#include "processes/process.h"
#include "processes/elf.h"
#include "scheduler/scheduler.h"
#include "keyboard/keyboard.h"
#include "terminal/terminal.h"
#include "fs/vfs.h"

static bool same(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static void number(uint64_t v) {
    char d[21]; size_t n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) terminal_putc_color(d[--n], TERMINAL_NORMAL);
}
static void signed_number(int64_t v) { if (v < 0) { terminal_write("-"); number((uint64_t)0 - (uint64_t)v); } else number((uint64_t)v); }
static void hex(uint64_t v) {
    terminal_write("0x");
    bool started = false;
    for (int s = 60; s >= 0; s -= 4) {
        unsigned d = (unsigned)((v >> s) & 15);
        if (d || started || s == 0) { started = true; terminal_putc_color((char)(d < 10 ? '0' + d : 'a' + d - 10), TERMINAL_NORMAL); }
    }
}
static const char *fault_name(uint64_t vector) {
    switch (vector) {
        case 0: return "divide error"; case 6: return "invalid instruction"; case 13: return "general protection (privileged instruction or bad access)";
        case 14: return "page fault"; case 16: return "x87 floating-point error"; case 17: return "alignment check"; case 19: return "SIMD floating-point error";
        default: return "CPU exception";
    }
}
static const char *state_name(const struct process_info *i) {
    if (i->state == PROC_EXITED) return i->faulted ? "faulted" : i->exit_status == PROCESS_EXIT_KILLED ? "terminated" : "exited";
    if (i->state == PROC_CREATED) return "created";
    switch (i->task_state) {
        case TASK_RUNNING: return "running"; case TASK_READY: return "ready"; case TASK_BLOCKED: return "blocked";
        default: return "active";
    }
}
static void report_end(const struct process_info *i, int64_t status) {
    terminal_write("[pid "); number(i->pid); terminal_write("] ");
    terminal_write(i->name); terminal_write(": ");
    if (i->faulted) {
        terminal_write("stopped by a CPU fault: "); terminal_write(fault_name(i->fault.vector));
        terminal_write(" at "); hex(i->fault.rip);
        if (i->fault.vector == 14) { terminal_write(", address "); hex(i->fault.address); }
        terminal_write(". The system is unaffected.\n");
    } else if (status == PROCESS_EXIT_KILLED) terminal_write("terminated\n");
    else { terminal_write("exited with status "); signed_number(status); terminal_write("\n"); }
}

unsigned process_report_finished(void) {
    struct process_info list[PROCESS_MAX];
    unsigned n = process_list(list, PROCESS_MAX), reported = 0;
    for (unsigned i = 0; i < n; ++i) {
        int64_t st;
        if (list[i].state != PROC_EXITED || !process_collect(list[i].pid, &st)) continue;
        report_end(&list[i], st);
        ++reported;
    }
    return reported;
}

static enum ush_result run(size_t argc, char **argv) {
    bool background = false;
    const char *program = 0;
    for (size_t i = 0; i < argc; ++i) {
        if (same(argv[i], "-b") || same(argv[i], "--background")) { if (background) program = 0; background = true; }
        else if (!program) program = argv[i];
        else { terminal_write("Usage: run [-b|--background] <program>\n"); return USH_USAGE; }
    }
    if (!program) { terminal_write("Usage: run [-b|--background] <program>\n"); return USH_USAGE; }
    char path[VFS_PATH_MAX + 1U];
    bool plain_name = true;
    for (const char *p = program; *p; ++p) if (*p == '/') plain_name = false;
    if (plain_name) {
        size_t n = 0;
        const char *prefix = "/bin/";
        while (*prefix && n < sizeof(path) - 1) path[n++] = *prefix++;
        for (const char *p = program; *p && n < sizeof(path) - 1; ++p) path[n++] = *p;
        path[n] = 0;
    } else {
        enum vfs_error ve = vfs_resolve_path(program, ush_cwd(), path, sizeof(path));
        if (ve != VFS_OK) return ush_fs_failure(ve);
    }
    (void)process_report_finished();                         /* free the slots of finished background jobs */
    enum elf_error ee;
    uint32_t pid = elf_spawn_file(path, 0, &ee);
    if (!pid) {
        terminal_write("Cannot run '"); terminal_write(path); terminal_write("': "); terminal_write(elf_error_name(ee)); terminal_write("\n");
        return USH_FS_ERROR;
    }
    if (background) {
        terminal_write("[pid "); number(pid); terminal_write("] started in the background; use 'jobs' to see it\n");
        return USH_OK;
    }
    /* Foreground: the shell waits. Ctrl+C ends the program; other keys typed meanwhile are discarded. */
    bool interrupted = false;
    for (;;) {
        struct process_info info;
        if (!process_info_get(pid, &info)) return USH_FS_ERROR;
        if (info.state == PROC_EXITED) break;
        int key = keyboard_poll();
        if (key == KEY_CTRL_C) { (void)process_kill(pid); interrupted = true; continue; }
        if (!key) sched_idle_wait();
    }
    struct process_info info;
    int64_t status = 0;
    (void)process_info_get(pid, &info);
    if (!process_collect(pid, &status)) { terminal_write("Could not collect the process.\n"); return USH_FS_ERROR; }
    if (interrupted) terminal_write("^C\n");
    report_end(&info, status);
    return USH_OK;
}

static enum ush_result jobs(void) {
    struct process_info list[PROCESS_MAX];
    unsigned n = process_list(list, PROCESS_MAX);
    if (!n) { terminal_write("No jobs.\n"); return USH_OK; }
    for (unsigned i = 0; i < n; ++i) {
        terminal_write("[pid "); number(list[i].pid); terminal_write("] ");
        terminal_write(list[i].name); terminal_write("  "); terminal_write(state_name(&list[i]));
        if (list[i].state == PROC_EXITED && !list[i].faulted && list[i].exit_status != PROCESS_EXIT_KILLED) { terminal_write(" ("); signed_number(list[i].exit_status); terminal_write(")"); }
        terminal_write("\n");
    }
    (void)process_report_finished();                         /* finished jobs are reported once and then released */
    return USH_OK;
}

static enum ush_result terminate(char **argv) {
    uint64_t pid = 0;
    const char *s = argv[0];
    if (!*s) { terminal_write("Usage: terminate <pid>\n"); return USH_USAGE; }
    for (; *s; ++s) {
        if (*s < '0' || *s > '9' || pid > 100000000ULL) { terminal_write("Usage: terminate <pid>\nThe pid must be a number from 'jobs'.\n"); return USH_USAGE; }
        pid = pid * 10 + (uint64_t)(*s - '0');
    }
    struct process_info info;
    if (!pid || pid > UINT32_MAX || !process_info_get((uint32_t)pid, &info)) { terminal_write("No such process.\n"); return USH_FS_ERROR; }
    if (info.state == PROC_EXITED) { terminal_write("That process has already finished; 'jobs' will report it.\n"); return USH_FS_ERROR; }
    if (!process_kill((uint32_t)pid)) { terminal_write("The process could not be terminated.\n"); return USH_FS_ERROR; }
    int64_t st;
    if (process_collect((uint32_t)pid, &st)) report_end(&info, st);
    return USH_OK;
}

static enum ush_result list(void) {
    struct process_info l[PROCESS_MAX];
    unsigned n = process_list(l, PROCESS_MAX);
    terminal_write("Processes: "); number(n); terminal_write(" of "); number(PROCESS_MAX); terminal_write("  (kernel tasks are not listed)\n");
    if (!n) return USH_OK;
    terminal_write("PID  PPID STATE       PAGES SWITCHES NAME / RESULT\n");
    for (unsigned i = 0; i < n; ++i) {
        number(l[i].pid); terminal_write("    "); number(l[i].ppid); terminal_write("    ");
        terminal_write(state_name(&l[i])); terminal_write("  ");
        number(l[i].data_pages); terminal_write("  "); number(l[i].switches); terminal_write("  ");
        terminal_write(l[i].name);
        if (l[i].state == PROC_EXITED) {
            if (l[i].faulted) { terminal_write("  fault "); number(l[i].fault.vector); terminal_write(" at "); hex(l[i].fault.rip); }
            else { terminal_write("  status "); signed_number(l[i].exit_status); }
        }
        terminal_write("\n");
    }
    return USH_OK;
}

enum ush_result process_operation(const char *name, size_t argc, char **argv) {
    if (same(name, "run")) return run(argc, argv);
    if (same(name, "jobs")) return jobs();
    if (same(name, "terminate")) return terminate(argv);
    if (same(name, "list")) return list();
    return USH_UNKNOWN;
}
