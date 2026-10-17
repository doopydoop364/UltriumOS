#include <stdbool.h>
#include <stdint.h>
#include "syscalls/syscall.h"
#include "processes/process.h"
#include "scheduler/scheduler.h"
#include "system/exceptions.h"
#include "system/interrupts.h"
#include "memory/aspace.h"
#include "fs/vfs.h"
#include "terminal/terminal.h"
#include "abi/ultrium_syscall.h"

static bool ready;
static uint64_t calls;

__asm__(
".global ultrium_syscall_entry\n"
"ultrium_syscall_entry:\n"
"pushfq\n" "andq $-263169, (%rsp)\n" "popfq\n"             /* clear DF and AC: ring 3 can set both (AC would disable SMAP for the call) */
"pushq %rax\n" "pushq %rbx\n" "pushq %rcx\n" "pushq %rdx\n"
"pushq %rbp\n" "pushq %rsi\n" "pushq %rdi\n"
"pushq %r8\n" "pushq %r9\n" "pushq %r10\n" "pushq %r11\n"
"pushq %r12\n" "pushq %r13\n" "pushq %r14\n" "pushq %r15\n"
"movq %rsp, %r12\n"
"movq %rsp, %rdi\n"
"andq $-16, %rsp\n"
"call ultrium_syscall_dispatch\n"
"movq %r12, %rsp\n"
"popq %r15\n" "popq %r14\n" "popq %r13\n" "popq %r12\n"
"popq %r11\n" "popq %r10\n" "popq %r9\n" "popq %r8\n"
"popq %rdi\n" "popq %rsi\n" "popq %rbp\n" "popq %rdx\n"
"popq %rcx\n" "popq %rbx\n" "popq %rax\n"
"iretq\n");
extern void ultrium_syscall_entry(void);

static int64_t err(enum ultrium_error e) { return -(int64_t)e; }
static int64_t from_vfs(enum vfs_error e) {
    switch (e) {
        case VFS_OK: return 0;
        case VFS_NOT_FOUND: return err(ULTRIUM_E_NOENT);
        case VFS_EXISTS: return err(ULTRIUM_E_EXIST);
        case VFS_NOT_DIRECTORY: return err(ULTRIUM_E_NOTDIR);
        case VFS_IS_DIRECTORY: return err(ULTRIUM_E_ISDIR);
        case VFS_INVALID_PATH: return err(ULTRIUM_E_INVAL);
        case VFS_NO_SPACE: return err(ULTRIUM_E_NOSPC);
        case VFS_BAD_HANDLE: return err(ULTRIUM_E_BADF);
        case VFS_BUFFER_TOO_SMALL: return err(ULTRIUM_E_NAMETOOLONG);
        case VFS_READ_ONLY: return err(ULTRIUM_E_ROFS);
        case VFS_BUSY: return err(ULTRIUM_E_BUSY);
        case VFS_DENIED: return err(ULTRIUM_E_ACCESS);
        case VFS_IO: case VFS_CORRUPT: case VFS_RECOVERY: return err(ULTRIUM_E_IO);
        default: return err(ULTRIUM_E_INVAL);
    }
}

/* Console output is limited to printable ASCII, newline, carriage return and tab so a program cannot
   inject terminal control sequences; anything else is shown as '?'. */
static int64_t sys_write_console(struct process *p, uint64_t buf, uint64_t len) {
    if (len > ULTRIUM_MAX_IO) return err(ULTRIUM_E_TOOBIG);
    if (!len) return 0;
    struct aspace *as = process_aspace_of(p);
    if (!aspace_range_ok(as, buf, (size_t)len, false)) return err(ULTRIUM_E_FAULT);
    char chunk[129];
    for (uint64_t done = 0; done < len;) {
        size_t n = len - done > 128 ? 128 : (size_t)(len - done);
        if (!aspace_copy_from_user(as, chunk, buf + done, n)) return err(ULTRIUM_E_FAULT);
        for (size_t i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)chunk[i];
            if (c != '\n' && c != '\r' && c != '\t' && (c < 0x20 || c > 0x7e)) chunk[i] = '?';
        }
        chunk[n] = 0;
        terminal_write(chunk);
        process_console_append(p, chunk, n);
        done += n;
    }
    return (int64_t)len;
}

static int64_t sys_open(struct process *p, uint64_t path_ptr, uint64_t path_len, uint64_t flags) {
    if (flags & ~(uint64_t)(ULTRIUM_O_READ | ULTRIUM_O_WRITE | ULTRIUM_O_CREATE)) return err(ULTRIUM_E_INVAL);
    uint32_t rights = (uint32_t)flags & (ULTRIUM_O_READ | ULTRIUM_O_WRITE);
    if (!rights || ((flags & ULTRIUM_O_CREATE) && !(flags & ULTRIUM_O_WRITE))) return err(ULTRIUM_E_INVAL);
    if (!path_len) return err(ULTRIUM_E_INVAL);
    if (path_len > ULTRIUM_MAX_PATH) return err(ULTRIUM_E_NAMETOOLONG);
    char raw[ULTRIUM_MAX_PATH + 1], path[VFS_PATH_MAX + 1];
    if (!aspace_copy_from_user(process_aspace_of(p), raw, path_ptr, (size_t)path_len)) return err(ULTRIUM_E_FAULT);
    raw[path_len] = 0;
    for (uint64_t i = 0; i < path_len; ++i) if (!raw[i]) return err(ULTRIUM_E_INVAL);     /* no embedded NUL */
    if (raw[0] != '/') return err(ULTRIUM_E_INVAL);                                      /* no working directory in ABI v1 */
    enum vfs_error e = vfs_normalize(raw, "/", path, sizeof(path));
    if (e != VFS_OK) return from_vfs(e);
    struct vfs_info info;
    e = vfs_stat(path, &info);
    if (e == VFS_NOT_FOUND && (flags & ULTRIUM_O_CREATE)) { e = vfs_create_file(path); if (e != VFS_OK) return from_vfs(e); e = vfs_stat(path, &info); }
    if (e != VFS_OK) return from_vfs(e);
    if (info.type == VFS_NODE_DIRECTORY) return err(ULTRIUM_E_ISDIR);
    if (process_fd_count(p) >= ULTRIUM_FD_MAX - 3) return err(ULTRIUM_E_MFILE);
    uint32_t vflags = (rights & ULTRIUM_O_READ ? VFS_OPEN_READ : 0) | (rights & ULTRIUM_O_WRITE ? VFS_OPEN_WRITE : 0);
    struct vfs_file file = {0};
    e = vfs_file_open(path, vflags, &file);
    if (e != VFS_OK) return from_vfs(e);
    int fd = process_fd_open(p, &file, rights);
    if (fd < 0) { (void)vfs_file_close(&file); return err(ULTRIUM_E_MFILE); }
    return fd;
}

static int64_t sys_read(struct process *p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (len > ULTRIUM_MAX_IO) return err(ULTRIUM_E_TOOBIG);
    if (fd == ULTRIUM_STDIN) return aspace_range_ok(process_aspace_of(p), buf, (size_t)len, true) ? 0 : err(ULTRIUM_E_FAULT);   /* no input device: end of file */
    if (fd >= ULTRIUM_FD_MAX) return err(ULTRIUM_E_BADF);
    struct vfs_file *f = process_fd_file(p, (int)fd, ULTRIUM_O_READ);
    if (!f) return err(ULTRIUM_E_BADF);
    struct aspace *as = process_aspace_of(p);
    if (!aspace_range_ok(as, buf, (size_t)len, true)) return err(ULTRIUM_E_FAULT);          /* nothing is consumed on a bad buffer */
    uint8_t chunk[512];
    uint64_t total = 0;
    while (total < len) {
        size_t want = len - total > sizeof(chunk) ? sizeof(chunk) : (size_t)(len - total), got = 0;
        enum vfs_error e = vfs_file_read(f, chunk, want, &got);
        if (e != VFS_OK && e != VFS_END) return total ? (int64_t)total : from_vfs(e);
        if (!got) break;
        if (!aspace_copy_to_user(as, buf + total, chunk, got)) return err(ULTRIUM_E_FAULT);
        total += got;
        if (got < want) break;
    }
    return (int64_t)total;
}

static int64_t sys_write(struct process *p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (len > ULTRIUM_MAX_IO) return err(ULTRIUM_E_TOOBIG);
    if (fd == ULTRIUM_STDOUT || fd == ULTRIUM_STDERR) return sys_write_console(p, buf, len);
    if (fd == ULTRIUM_STDIN || fd >= ULTRIUM_FD_MAX) return err(ULTRIUM_E_BADF);
    struct vfs_file *f = process_fd_file(p, (int)fd, ULTRIUM_O_WRITE);
    if (!f) return err(ULTRIUM_E_BADF);
    struct aspace *as = process_aspace_of(p);
    if (!aspace_range_ok(as, buf, (size_t)len, false)) return err(ULTRIUM_E_FAULT);
    uint8_t chunk[512];
    uint64_t total = 0;
    while (total < len) {
        size_t want = len - total > sizeof(chunk) ? sizeof(chunk) : (size_t)(len - total), wrote = 0;
        if (!aspace_copy_from_user(as, chunk, buf + total, want)) return err(ULTRIUM_E_FAULT);
        enum vfs_error e = vfs_file_write(f, chunk, want, &wrote);
        total += wrote;
        if (e != VFS_OK) return total ? (int64_t)total : from_vfs(e);
        if (wrote < want) break;
    }
    return (int64_t)total;
}

static int64_t sys_close(struct process *p, uint64_t fd) {
    if (fd < 3 || fd >= ULTRIUM_FD_MAX) return err(ULTRIUM_E_BADF);                          /* the console descriptors stay open */
    if (!process_fd_file(p, (int)fd, 0)) return err(ULTRIUM_E_BADF);
    return process_fd_close(p, (int)fd) ? 0 : err(ULTRIUM_E_IO);
}

int64_t syscall_run(struct process *p, uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (number) {
        case ULTRIUM_SYS_EXIT: process_exit_current((int64_t)(int32_t)a1);
        case ULTRIUM_SYS_YIELD: sched_yield(); return 0;
        case ULTRIUM_SYS_WRITE_CONSOLE: return sys_write_console(p, a1, a2);
        case ULTRIUM_SYS_GET_PID: return (int64_t)process_pid_of(p);
        case ULTRIUM_SYS_OPEN: return sys_open(p, a1, a2, a3);
        case ULTRIUM_SYS_READ: return sys_read(p, a1, a2, a3);
        case ULTRIUM_SYS_WRITE: return sys_write(p, a1, a2, a3);
        case ULTRIUM_SYS_CLOSE: return sys_close(p, a1);
        case ULTRIUM_SYS_ABI_VERSION: return ULTRIUM_ABI_VERSION;
        case ULTRIUM_SYS_GET_TICKS: return (int64_t)interrupts_timer_ticks();
        case ULTRIUM_SYS_SLEEP:
            if (a1 > ULTRIUM_MAX_SLEEP_TICKS) return err(ULTRIUM_E_INVAL);
            sched_sleep((uint32_t)a1);
            return 0;
        default: return err(ULTRIUM_E_NOSYS);
    }
}

void ultrium_syscall_dispatch(struct syscall_frame *f) {
    ++calls;
    struct process *p = process_current();
    if ((f->cs & 3) != 3 || !p) { f->rax = (uint64_t)err(ULTRIUM_E_ACCESS); return; }     /* only a user process may enter */
    f->rax = (uint64_t)syscall_run(p, f->rax, f->rdi, f->rsi, f->rdx);
}

/* Test hook: run a system call on behalf of `p` without entering ring 3, so hostile arguments can be
   tried deterministically. Not reachable from userspace. exit is refused. */
int64_t syscall_test_invoke(uint32_t pid, uint64_t number, uint64_t a1, uint64_t a2, uint64_t a3) {
    struct process *p = process_find(pid);
    if (!p || number == ULTRIUM_SYS_EXIT) return err(ULTRIUM_E_NOPROC);
    struct task *t = task_current();
    struct process *saved = t->process;
    t->process = p;
    int64_t r = syscall_run(p, number, a1, a2, a3);
    t->process = saved;
    return r;
}

bool syscall_init(void) {
    if (ready) return false;
    ready = exceptions_install_syscall_gate(ultrium_syscall_entry);
    return ready;
}
bool syscall_ready(void) { return ready; }
uint64_t syscall_count(void) { return calls; }
