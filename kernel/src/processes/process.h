#ifndef ULTRIUM_PROCESS_H
#define ULTRIUM_PROCESS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "scheduler/task.h"
#include "fs/vfs.h"

/* Processes (docs/process-model.md). A user process owns an address space and one task. */

#define PROCESS_MAX 16U
#define PROCESS_NAME_MAX 16U
#define PROCESS_STACK_PAGES 8U
#define PROCESS_STACK_TOP 0x00007ffffff00000ULL        /* the page below the stack stays unmapped (guard) */
#define PROCESS_CONSOLE_MAX 4096U                    /* bounded copy of a process's console output (tests, diagnostics) */
#define PROCESS_EXIT_KILLED (-999)                     /* terminated from outside */
#define PROCESS_EXIT_FAULT_BASE (-1000)                /* status = -1000 - exception vector */

enum process_state { PROC_FREE = 0, PROC_CREATED, PROC_ACTIVE, PROC_EXITED };

struct process_fault { uint64_t vector, error, rip, address; };
struct process_info {
    uint32_t pid, ppid;
    char name[PROCESS_NAME_MAX];
    enum process_state state;
    enum task_state task_state;
    int64_t exit_status;
    bool faulted;
    struct process_fault fault;
    uint32_t data_pages;
    uint64_t switches;
};

bool process_system_init(void);
bool process_system_ready(void);
/* Create a user process with an empty address space and a task that has not run. */
uint32_t process_create(const char *name, uint32_t parent_pid);
/* Map `memory_size` bytes at `va` with `permissions` (enum vmm_page_permissions), initialised from `data`
   (`data_size` <= memory_size, rest zero). All or nothing for this segment. */
bool process_map_segment(uint32_t pid, uint64_t va, const void *data, size_t data_size, size_t memory_size, uint32_t permissions);
/* Map the default user stack. */
bool process_map_stack(uint32_t pid);
/* Make the process runnable at `entry` with the initial stack pointer `stack`. */
bool process_start(uint32_t pid, uint64_t entry, uint64_t stack);
/* Abandon a process that was created but never started. */
bool process_discard(uint32_t pid);
/* Foreground wait: block the calling kernel task until the process exits; returns its status and
   releases the table slot. */
bool process_wait(uint32_t pid, int64_t *status);
/* Release the slot of an exited process (zombie) without blocking. */
bool process_collect(uint32_t pid, int64_t *status);
/* Terminate from outside (status PROCESS_EXIT_KILLED). */
bool process_kill(uint32_t pid);
__attribute__((noreturn)) void process_exit_current(int64_t status);
uint32_t process_current_pid(void);                 /* 0 for kernel tasks */
bool process_info_get(uint32_t pid, struct process_info *info);
unsigned process_list(struct process_info *out, unsigned max);
unsigned process_count_live(void);
/* Read or write the process's address space from kernel code (loader/tests/syscalls). */
struct aspace *process_aspace(uint32_t pid);
/* Open-file table (descriptors 3..15; 0-2 are the console). Rights are ULTRIUM_O_READ/WRITE. */
struct process;
int process_fd_open(struct process *p, const struct vfs_file *file, uint32_t rights);
struct vfs_file *process_fd_file(struct process *p, int fd, uint32_t need_rights);
bool process_fd_close(struct process *p, int fd);
unsigned process_fd_count(const struct process *p);
struct aspace *process_aspace_of(struct process *p);
uint32_t process_pid_of(const struct process *p);
/* Console output of a process: written to the terminal and also kept (bounded) for diagnostics. */
unsigned process_console_peek(uint32_t pid, char *out, size_t capacity);   /* copy the captured console text (NUL-terminated) */
void process_console_append(struct process *p, const char *data, size_t length);
/* As process_wait, additionally copying out the captured console text (NUL-terminated). */
bool process_wait_ex(uint32_t pid, int64_t *status, char *console, size_t capacity);
struct process *process_current(void);
struct process *process_find(uint32_t pid);
#endif
