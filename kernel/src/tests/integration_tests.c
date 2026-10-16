#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tests_internal.h"
#include "ush.h"
#include "processes/elf.h"
#include "processes/process.h"
#include "processes/programs.h"
#include "scheduler/scheduler.h"
#include "storage/block.h"
#include "storage/storage_ops.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "system/interrupts.h"
#include "abi/ultrium_syscall.h"
#include "memory.h"

/* End-to-end scenarios across scheduler, address spaces, ring 3, system calls, the ELF loader, the VFS and
   a persistent UltriumFS 2 volume, plus the failure paths between them. */

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
static uint32_t launch(const char *name) {
    char path[40] = "/bin/";
    size_t n = 5; for (const char *p = name; *p; ++p) path[n++] = *p; path[n] = 0;
    enum elf_error e;
    return elf_spawn_file(path, 0, &e);
}
static bool contains(const char *text, const char *needle) {
    size_t n = 0; while (needle[n]) ++n;
    for (const char *p = text; *p; ++p) { size_t i = 0; while (i < n && p[i] == needle[i]) ++i; if (i == n) return true; }
    return false;
}

static volatile uint64_t kernel_progress[2];
static volatile bool kernel_stop;
static void kernel_spinner(void *arg) { unsigned id = (unsigned)(uintptr_t)arg; while (!kernel_stop) ++kernel_progress[id]; }

static const char *concurrent_system(void) {
    clear_processes();
    REQUIRE(programs_install_set(true, true), "cannot install the test programs");
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    unsigned tasks = task_count_live();
    struct sched_stats s0, s1;
    sched_stats_get(&s0);
    kernel_stop = false; kernel_progress[0] = kernel_progress[1] = 0;
    struct task *k0 = sched_spawn("kspin0", kernel_spinner, (void *)0, TASK_FLAG_KERNEL | TASK_FLAG_PREEMPTIBLE);
    struct task *k1 = sched_spawn("kspin1", kernel_spinner, (void *)1, TASK_FLAG_KERNEL | TASK_FLAG_PREEMPTIBLE);
    REQUIRE(k0 && k1, "kernel task spawn failed");
    uint32_t hello = launch("hello"), bad = launch("badcalls"), crash = launch("crash"), spin = launch("spin"), sleeper = launch("sleeper");
    uint32_t c1 = launch("counter"), c2 = launch("counter"), fpu = launch("fpudemo");
    REQUIRE(hello && bad && crash && spin && sleeper && c1 && c2 && fpu, "launching programs failed");
    REQUIRE(process_count_live() == 8, "wrong number of processes");
    /* The shell stays usable and the timer keeps running while all of this executes. */
    uint64_t t0 = interrupts_timer_ticks();
    REQUIRE(ush_execute("where") == USH_OK, "the shell is unusable");
    int64_t st; static char out[PROCESS_CONSOLE_MAX];
    REQUIRE(process_wait_ex(hello, &st, out, sizeof(out)) && st == 0 && contains(out, "Hello from Ultrium"), "hello failed");
    REQUIRE(process_wait_ex(bad, &st, out, sizeof(out)) && st == 0 && contains(out, "every hostile call was refused"), "badcalls: a hostile system call was not refused");
    struct process_info info;
    REQUIRE(process_info_get(crash, &info), "crash vanished");
    REQUIRE(process_wait_ex(crash, &st, out, sizeof(out)), "wait failed");
    REQUIRE(st == -1000 - 14 && contains(out, "about to read kernel memory"), "the crashing program did not fault as expected");
    REQUIRE(process_wait(c1, &st) && st == 0 && process_wait(c2, &st) && st == 0, "a counter failed");
    REQUIRE(process_wait(fpu, &st) && st == 0, "fpudemo failed");
    /* The sleeper is inside the kernel (sleep system call); spin has been running throughout. */
    REQUIRE(process_info_get(sleeper, &info) && info.state == PROC_ACTIVE && info.task_state == TASK_BLOCKED, "the sleeper is not blocked inside the sleep system call");
    REQUIRE(process_info_get(spin, &info) && info.state == PROC_ACTIVE && info.switches > 2, "spin was not scheduled repeatedly");
    uint64_t k0a = kernel_progress[0], k1a = kernel_progress[1];
    wait_ticks(10);
    REQUIRE(kernel_progress[0] > k0a && kernel_progress[1] > k1a, "kernel tasks stopped making progress");
    /* End one process while it runs and one while it is blocked inside a system call; the rest continue. */
    REQUIRE(process_kill(spin) && process_kill(sleeper), "terminate failed");
    REQUIRE(process_wait(spin, &st) && st == PROCESS_EXIT_KILLED && process_wait(sleeper, &st) && st == PROCESS_EXIT_KILLED, "terminated status wrong");
    uint64_t k0b = kernel_progress[0];
    wait_ticks(5);
    REQUIRE(kernel_progress[0] > k0b, "kernel stopped after terminating user processes");
    kernel_stop = true;
    for (unsigned guard = 0; guard < 200 && (task_by_tid(k0->tid) || task_by_tid(k1->tid)); ++guard) wait_ticks(1);
    REQUIRE(interrupts_timer_ticks() > t0, "timer stopped");
    sched_stats_get(&s1);
    REQUIRE(s1.preemptions > s0.preemptions + 5, "too little preemption for this workload");
    REQUIRE(sched_queue_consistent(), "run queue corrupt");
    bool clean = process_count_live() == 0 && task_count_live() == tasks && pmm_free_pages() == frames && heap_live_allocations() == live;
    REQUIRE(programs_install_set(true, false), "cannot remove the test programs");
    REQUIRE(clean, "the scenario leaked tasks, frames or heap blocks");
    return 0;
}

static const char *process_table_exhaustion(void) {
    clear_processes();
    uint64_t frames = pmm_free_pages(), live = heap_live_allocations();
    unsigned tasks = task_count_live();
    uint32_t pids[PROCESS_MAX + 4];
    unsigned made = 0;
    while (made < PROCESS_MAX + 2 && (pids[made] = launch("spin"))) ++made;
    REQUIRE(made >= 8 && made < PROCESS_MAX + 2, "the table or task limit was not reached cleanly");
    REQUIRE(launch("spin") == 0, "a launch beyond the limit succeeded");
    REQUIRE(sched_queue_consistent(), "run queue corrupt after exhaustion");
    wait_ticks(20);                                     /* every process gets the CPU */
    for (unsigned i = 0; i < made; ++i) {
        struct process_info info;
        REQUIRE(process_info_get(pids[i], &info) && info.switches >= 1, "a process never ran");
    }
    REQUIRE(ush_execute("where") == USH_OK, "shell unusable under load");
    for (unsigned i = 0; i < made; ++i) REQUIRE(process_kill(pids[i]), "terminate failed");
    for (unsigned i = 0; i < made; ++i) { int64_t st; REQUIRE(process_wait(pids[i], &st) && st == PROCESS_EXIT_KILLED, "wait failed"); }
    uint32_t again = launch("hello");
    int64_t st;
    REQUIRE(again && process_wait(again, &st) && st == 0, "the system cannot launch after exhaustion");
    REQUIRE(process_count_live() == 0 && task_count_live() == tasks && pmm_free_pages() == frames && heap_live_allocations() == live, "exhaustion leaked");
    return 0;
}

static const char *persistent_volume(void) {
    clear_processes();
    (void)ush_execute("go /");
    REQUIRE(programs_install_set(true, true), "cannot install the test programs");
    uint64_t frames = pmm_free_pages();
    REQUIRE(!vfs_is_mount_point("/volumes/disk"), "a volume is already mounted at /volumes/disk");
    (void)vfs_create_directory("/volumes");
    REQUIRE(vfs_create_directory("/volumes/disk") == VFS_OK || vfs_is_directory("/volumes/disk"), "mount point setup failed");
    struct block_device *dev = block_memory_create("integ", 512, 16384, false);
    REQUIRE(dev, "device creation failed");
    const char *err = 0;
    int64_t st;
    static char out[PROCESS_CONSOLE_MAX];
    if (ush_execute("ultrium storage format integ -y") != USH_OK || ush_execute("ultrium storage mount integ /volumes/disk") != USH_OK) err = "format or mount failed";
    uint32_t pid = 0;
    if (!err) {
        pid = launch("filedemo");
        if (!pid || !process_wait_ex(pid, &st, out, sizeof(out)) || st != 0 || !contains(out, "/volumes/disk/filedemo.txt: OK")) err = "filedemo did not write and verify its file on the persistent volume";
    }
    if (!err) {
        struct vfs_info info;
        if (vfs_stat("/volumes/disk/filedemo.txt", &info) != VFS_OK || !info.persistent || info.size != 42) err = "the file is not on the persistent volume";
    }
    if (!err) {
        pid = launch("filecheck");
        if (!pid || !process_wait(pid, &st) || st != 0) err = "filecheck could not read the persistent file";
    }
    if (!err) {                                           /* hostile calls while a persistent volume is mounted */
        pid = launch("badcalls");
        if (!pid || !process_wait(pid, &st) || st != 0) err = "badcalls misbehaved with a persistent volume mounted";
    }
    if (!err && process_count_live() != 0) err = "a process remained";
    if (!err && ush_execute("ultrium storage unmount /volumes/disk") != USH_OK) err = "unmount failed (a user descriptor leaked?)";
    else if (err) (void)ush_execute("ultrium storage unmount /volumes/disk");
    if (!err && ush_execute("ultrium storage check integ") != USH_OK) err = "the volume is not consistent after userspace file I/O";
    struct vfs_remove_report rr;
    (void)vfs_remove_tree("/volumes", &rr);
    if (!block_memory_destroy(dev) && !err) err = "device destroy failed";
    if (!err && pmm_free_pages() != frames) err = "frames leaked";
    (void)programs_install_set(true, false);
    return err;
}

static const char *kill_during_file_io(void) {
    /* A process ended while it still holds descriptors on a persistent volume must not pin the volume. */
    clear_processes();
    (void)ush_execute("go /");
    REQUIRE(programs_install_set(true, true), "cannot install the test programs");
    (void)vfs_create_directory("/volumes");
    REQUIRE(vfs_create_directory("/volumes/disk") == VFS_OK || vfs_is_directory("/volumes/disk"), "mount point setup failed");
    struct block_device *dev = block_memory_create("integ", 512, 16384, false);
    REQUIRE(dev, "device creation failed");
    const char *err = 0;
    if (ush_execute("ultrium storage format integ -y") != USH_OK || ush_execute("ultrium storage mount integ /volumes/disk") != USH_OK) err = "format or mount failed";
    uint32_t pid = err ? 0 : launch("diskwriter");
    if (!err && !pid) err = "launch failed";
    if (!err) {
        wait_ticks(25);
        struct vfs_info info;
        if (vfs_stat("/volumes/disk/spam.bin", &info) != VFS_OK || info.size == 0) err = "the disk writer made no progress";
        else if (ush_execute("ultrium storage unmount /volumes/disk") != USH_FS_ERROR) err = "a volume with an open user file was unmounted";
        else if (!process_kill(pid)) err = "terminate failed";
        else { int64_t st; if (!process_wait(pid, &st) || st != PROCESS_EXIT_KILLED) err = "wait failed"; }
    }
    if (!err && ush_execute("ultrium storage unmount /volumes/disk") != USH_OK) err = "the volume stayed busy after the process was terminated";
    if (!err && ush_execute("ultrium storage check integ") != USH_OK) err = "the volume is inconsistent after the writer was terminated";
    else if (err) { clear_processes(); (void)ush_execute("ultrium storage unmount /volumes/disk"); }
    struct vfs_remove_report rr;
    (void)vfs_remove_tree("/volumes", &rr);
    if (!block_memory_destroy(dev) && !err) err = "device destroy failed";
    (void)programs_install_set(true, false);
    return err;
}

const struct test_case integration_cases[] = {
    {"integration: kernel tasks and eight user programs run together, one faults, two are terminated", concurrent_system},
    {"integration: process and task limits are reached cleanly and the system recovers", process_table_exhaustion},
    {"integration: userspace reads and writes a persistent UltriumFS 2 volume", persistent_volume},
    {"integration: terminating a process in the middle of file I/O releases the volume", kill_during_file_io},
};
const size_t integration_case_count = sizeof(integration_cases) / sizeof(integration_cases[0]);
