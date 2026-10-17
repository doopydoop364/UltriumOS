#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "ush.h"
#include "terminal/terminal.h"
#include "processes/process.h"
#include "processes/process_cmd.h"
#include "scheduler/scheduler.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "system/interrupts.h"
#include "memory.h"

/* USH process commands: run, jobs, terminate, ultrium system processes. */

#define REQUIRE(cond, text) do { if (!(cond)) return text; } while (0)

static void clear_processes(void) {
    struct process_info list[PROCESS_MAX];
    unsigned n = process_list(list, PROCESS_MAX);
    for (unsigned i = 0; i < n; ++i) {
        int64_t st;
        if (list[i].state == PROC_ACTIVE) (void)process_kill(list[i].pid);
        if (list[i].state == PROC_CREATED) (void)process_discard(list[i].pid);
        else (void)process_wait(list[i].pid, &st);
    }
}
static void wait_ticks(uint64_t n) {
    uint64_t end = interrupts_timer_ticks() + n;
    while (interrupts_timer_ticks() < end) sched_idle_wait();
}
/* Run a command with terminal capture on; the capture is left on for the caller to inspect. */
static enum ush_result capture(const char *line) {
    terminal_test_capture(1);
    enum ush_result r = ush_execute(line);
    terminal_test_capture(0);
    return r;
}

static const char *help_and_usage(void) {
    REQUIRE(ush_registry_valid(), "the command registry is invalid");
    REQUIRE(capture("help") == USH_OK && terminal_test_contains("run ") && terminal_test_contains("jobs") && terminal_test_contains("terminate"), "top-level help does not list the process commands");
    REQUIRE(capture("run help") == USH_OK && terminal_test_contains("[-b|--background] <program>") && terminal_test_contains("Ctrl+C"), "run help missing");
    REQUIRE(capture("jobs help") == USH_OK && capture("terminate help") == USH_OK && terminal_test_contains("<pid>"), "jobs/terminate help missing");
    REQUIRE(capture("ultrium system help") == USH_OK && terminal_test_contains("processes"), "the parent menu does not link to processes");
    REQUIRE(capture("ultrium system processes help") == USH_OK && terminal_test_contains("Processes:"), "processes help missing");
    REQUIRE(ush_execute("run") == USH_USAGE && ush_execute("run a b c") == USH_USAGE && ush_execute("run -b -b") == USH_USAGE, "run argument bounds not enforced");
    REQUIRE(ush_execute("terminate") == USH_USAGE && ush_execute("terminate 1 2") == USH_USAGE && ush_execute("jobs now") == USH_USAGE, "argument bounds not enforced");
    REQUIRE(ush_execute("terminate abc") == USH_USAGE && ush_execute("terminate -1") == USH_USAGE && ush_execute("terminate 99999999999999999999") == USH_USAGE && ush_execute("terminate 4294967296") == USH_USAGE, "a malformed pid was accepted");
    REQUIRE(ush_execute("terminate 0") == USH_FS_ERROR && ush_execute("terminate 12345") == USH_FS_ERROR, "an invalid pid was accepted");
    REQUIRE(ush_execute("stop 1") == USH_UNKNOWN && ush_execute("ps") == USH_UNKNOWN && ush_execute("kill 1") == USH_UNKNOWN && ush_execute("bg") == USH_UNKNOWN, "a Linux-style process command exists by default");
    REQUIRE(process_count_live() == 0, "usage errors created processes");
    return 0;
}

static const char *foreground(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    REQUIRE(capture("run hello") == USH_OK, "run hello failed");
    REQUIRE(terminal_test_contains("Hello from Ultrium userspace!") && terminal_test_contains("exited with status 0"), "run hello output wrong");
    REQUIRE(process_count_live() == 0, "a foreground job was left behind");
    REQUIRE(ush_execute("go /bin") == USH_OK && capture("run ./hello") == USH_OK && terminal_test_contains("exited with status 0"), "run with a relative path failed");
    REQUIRE(ush_execute("go /") == USH_OK && capture("run /bin/hello") == USH_OK && terminal_test_contains("exited with status 0"), "run with an absolute path failed");
    REQUIRE(capture("run nothing-here") == USH_FS_ERROR && terminal_test_contains("Cannot run '/bin/nothing-here'"), "missing program not reported");
    REQUIRE(capture("run /bin") == USH_FS_ERROR && terminal_test_contains("Cannot run"), "directory accepted");
    REQUIRE(ush_execute("run ../../x") == USH_FS_ERROR, "bad path accepted");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames && heap_live_allocations() == live, "run leaked");
    return 0;
}

static const char *background_jobs(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    REQUIRE(capture("run -b counter") == USH_OK && terminal_test_contains("started in the background"), "background start failed");
    REQUIRE(capture("run counter --background") == USH_OK, "flag after the program rejected");
    REQUIRE(process_count_live() == 2, "two jobs expected");
    struct process_info l[PROCESS_MAX];
    REQUIRE(process_list(l, PROCESS_MAX) == 2, "listing wrong");
    uint32_t first = l[0].pid, second = l[1].pid;
    REQUIRE(capture("jobs") == USH_OK && terminal_test_contains("counter"), "jobs does not show the programs");
    REQUIRE(capture("ultrium system processes") == USH_OK && terminal_test_contains("Processes: 2 of 16") && terminal_test_contains("PID"), "processes table wrong");
    /* The shell prompt stays usable while they run. */
    REQUIRE(ush_execute("where") == USH_OK, "shell unusable while jobs run");
    char cmd[40] = "terminate ";
    size_t n = 10; uint32_t v = first; char d[12]; unsigned k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) cmd[n++] = d[--k];
    cmd[n] = 0;
    REQUIRE(capture(cmd) == USH_OK && terminal_test_contains("terminated"), "terminate failed");
    REQUIRE(ush_execute(cmd) == USH_FS_ERROR, "terminating a finished process accepted");
    struct process_info info;
    REQUIRE(!process_info_get(first, &info) && process_info_get(second, &info) && info.state == PROC_ACTIVE, "the other job was affected");
    /* Let the other one finish; jobs reports it once and releases it. */
    for (unsigned guard = 0; guard < 400 && process_info_get(second, &info) && info.state != PROC_EXITED; ++guard) wait_ticks(2);
    REQUIRE(capture("jobs") == USH_OK && terminal_test_contains("exited with status 0"), "a finished job was not reported");
    REQUIRE(capture("jobs") == USH_OK && terminal_test_contains("No jobs."), "a finished job was not released");
    REQUIRE(process_count_live() == 0 && pmm_free_pages() == frames, "jobs leaked");
    return 0;
}

/* A hand-built executable whose first instruction faults (read of address 0). */
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, (uint16_t)v); w16(p + 2, (uint16_t)(v >> 16)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static size_t crash_image(uint8_t *img) {
    static const uint8_t code[] = {0x31,0xc0, 0x48,0x8b,0x18, 0xeb,0xfe};
    memset(img, 0, 256);
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 2; img[5] = 1; img[6] = 1;
    w16(img + 16, 2); w16(img + 18, 62); w32(img + 20, 1); w64(img + 24, 0x400100); w64(img + 32, 64);
    w16(img + 52, 64); w16(img + 54, 56); w16(img + 56, 1);
    uint8_t *ph = img + 64;
    w32(ph, 1); w32(ph + 4, 5); w64(ph + 8, 0x100); w64(ph + 16, 0x400100); w64(ph + 24, 0x400100); w64(ph + 32, sizeof(code)); w64(ph + 40, sizeof(code)); w64(ph + 48, 0x1000);
    memcpy(img + 0x100, code, sizeof(code));
    return 0x100 + sizeof(code);
}
static const char *faulting_program(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages();
    uint8_t img[256];
    size_t size = crash_image(img);
    struct vfs_file f = {0}; size_t n;
    (void)vfs_unlink("/crash-demo");
    REQUIRE(vfs_create_file("/crash-demo") == VFS_OK && vfs_file_open("/crash-demo", VFS_OPEN_WRITE, &f) == VFS_OK && vfs_file_write(&f, img, size, &n) == VFS_OK && n == size, "setup failed");
    (void)vfs_file_close(&f);
    uint64_t t0 = interrupts_timer_ticks();
    REQUIRE(capture("run /crash-demo") == USH_OK, "the shell reported a failure for a program that crashed");
    REQUIRE(terminal_test_contains("stopped by a CPU fault: page fault") && terminal_test_contains("address 0x0") && terminal_test_contains("The system is unaffected."), "fault report wrong");
    REQUIRE(process_count_live() == 0, "the faulted process was not released");
    wait_ticks(2);
    REQUIRE(interrupts_timer_ticks() > t0, "the timer stopped");
    REQUIRE(capture("run hello") == USH_OK && terminal_test_contains("exited with status 0"), "the shell cannot run programs after a crash");
    REQUIRE(vfs_unlink("/crash-demo") == VFS_OK, "cleanup failed");
    REQUIRE(pmm_free_pages() == frames, "leaked");
    return 0;
}

static const char *compatibility_unchanged(void) {
    REQUIRE(ush_execute("ultrium shell compatibility enable") == USH_OK, "cannot enable compatibility");
    enum ush_result ps = ush_execute("ps"), kill = ush_execute("kill 1");
    enum ush_result ls = ush_execute("ls /bin");
    (void)ush_execute("ultrium shell compatibility disable");
    REQUIRE(ps == USH_UNKNOWN && kill == USH_UNKNOWN, "compatibility mode added process aliases");
    REQUIRE(ls == USH_OK, "existing aliases broke");
    REQUIRE(ush_execute("ls") == USH_UNAVAILABLE, "aliases are not session-scoped");
    return 0;
}

const struct test_case process_shell_cases[] = {
    {"shell process commands: help, argument bounds, no Linux aliases", help_and_usage},
    {"shell run: foreground programs by name, relative and absolute path", foreground},
    {"shell background jobs, jobs listing, terminate, processes table", background_jobs},
    {"shell survives a program that faults", faulting_program},
    {"shell compatibility mode adds no process aliases", compatibility_unchanged},
};
const size_t process_shell_case_count = sizeof(process_shell_cases) / sizeof(process_shell_cases[0]);
