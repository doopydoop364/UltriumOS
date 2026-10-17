#ifndef ULTRIUM_SHARED_ABI_SYSCALL_H
#define ULTRIUM_SHARED_ABI_SYSCALL_H
/* Ultrium system-call ABI, version 1.0. Shared by the kernel and by userspace programs.
   Freestanding: only explicit-width types. See docs/syscall-abi.md.

   Entry:  int $0x80
   Number: rax.  Arguments: rdi, rsi, rdx, r10, r8, r9.
   Result: rax, signed 64-bit; a negative value is the negated ULTRIUM_E_* code.
   Preserved: every register except rax. */
#include <stdint.h>

#define ULTRIUM_ABI_MAJOR 1U
#define ULTRIUM_ABI_MINOR 0U
#define ULTRIUM_ABI_VERSION ((ULTRIUM_ABI_MAJOR << 16) | ULTRIUM_ABI_MINOR)
#define ULTRIUM_SYSCALL_VECTOR 0x80U

enum ultrium_syscall {
    ULTRIUM_SYS_EXIT = 0,           /* exit(int32 status) - does not return */
    ULTRIUM_SYS_YIELD = 1,          /* yield() -> 0 */
    ULTRIUM_SYS_WRITE_CONSOLE = 2,  /* write_console(const char *buf, size_t len) -> bytes written */
    ULTRIUM_SYS_GET_PID = 3,        /* get_pid() -> pid */
    ULTRIUM_SYS_OPEN = 4,           /* open(const char *path, size_t path_len, uint32 flags) -> fd */
    ULTRIUM_SYS_READ = 5,           /* read(int fd, void *buf, size_t len) -> bytes read, 0 at end of file */
    ULTRIUM_SYS_WRITE = 6,          /* write(int fd, const void *buf, size_t len) -> bytes written */
    ULTRIUM_SYS_CLOSE = 7,          /* close(int fd) -> 0 */
    ULTRIUM_SYS_ABI_VERSION = 8,    /* abi_version() -> ULTRIUM_ABI_VERSION */
    ULTRIUM_SYS_GET_TICKS = 9,      /* get_ticks() -> timer ticks since boot (100 Hz) */
    ULTRIUM_SYS_SLEEP = 10,         /* sleep(uint64 ticks) -> 0 */
    ULTRIUM_SYS_COUNT = 11
};

enum ultrium_error {
    ULTRIUM_OK = 0,
    ULTRIUM_E_NOSYS = 1,            /* no such system call */
    ULTRIUM_E_INVAL = 2,            /* invalid argument */
    ULTRIUM_E_FAULT = 3,            /* a user pointer range is not valid, mapped or permitted */
    ULTRIUM_E_BADF = 4,             /* not an open descriptor, or not opened for this operation */
    ULTRIUM_E_NOENT = 5,            /* no such file */
    ULTRIUM_E_NOMEM = 6,
    ULTRIUM_E_MFILE = 7,            /* too many open descriptors */
    ULTRIUM_E_ISDIR = 8,
    ULTRIUM_E_NOTDIR = 9,
    ULTRIUM_E_EXIST = 10,
    ULTRIUM_E_NOSPC = 11,
    ULTRIUM_E_IO = 12,
    ULTRIUM_E_ROFS = 13,            /* read-only volume */
    ULTRIUM_E_NAMETOOLONG = 14,
    ULTRIUM_E_BUSY = 15,
    ULTRIUM_E_ACCESS = 16,          /* refused by policy */
    ULTRIUM_E_NOPROC = 17,
    ULTRIUM_E_TOOBIG = 18           /* length above ULTRIUM_MAX_IO */
};

#define ULTRIUM_O_READ   1U
#define ULTRIUM_O_WRITE  2U
#define ULTRIUM_O_CREATE 4U         /* create a regular file when missing (requires ULTRIUM_O_WRITE) */

#define ULTRIUM_STDIN  0
#define ULTRIUM_STDOUT 1
#define ULTRIUM_STDERR 2
#define ULTRIUM_FD_MAX 16U
#define ULTRIUM_MAX_PATH 127U
#define ULTRIUM_MAX_IO 65536U
#define ULTRIUM_MAX_SLEEP_TICKS 6000U
#endif
