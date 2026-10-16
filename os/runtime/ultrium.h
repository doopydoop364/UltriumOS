#ifndef ULTRIUM_RUNTIME_H
#define ULTRIUM_RUNTIME_H
/* Minimal freestanding runtime for native Ultrium programs. No C library: just system-call wrappers over
   the shared ABI (shared/abi/ultrium_syscall.h), a few string helpers and tiny console output helpers.
   Programs link against nothing from the kernel. */
#include <stddef.h>
#include <stdint.h>
#include "abi/ultrium_syscall.h"

static inline int64_t ultrium_syscall6(uint64_t n, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6) {
    int64_t result;
    register uint64_t r10 __asm__("r10") = a4;
    register uint64_t r8 __asm__("r8") = a5;
    register uint64_t r9 __asm__("r9") = a6;
    __asm__ volatile("int $0x80" : "=a"(result) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return result;
}
#define ultrium_syscall3(n, a, b, c) ultrium_syscall6((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0)

__attribute__((noreturn)) void ultrium_exit(int status);
int64_t ultrium_write_console(const char *text, size_t length);
int64_t ultrium_open(const char *path, uint32_t flags);       /* path is NUL-terminated here; the ABI takes a length */
int64_t ultrium_read(int fd, void *buffer, size_t length);
int64_t ultrium_write(int fd, const void *buffer, size_t length);
int64_t ultrium_close(int fd);
int64_t ultrium_yield(void);
int64_t ultrium_get_pid(void);
uint64_t ultrium_ticks(void);
int64_t ultrium_sleep(uint64_t ticks);
int64_t ultrium_abi_version(void);

size_t ustrlen(const char *s);
int ustrcmp(const char *a, const char *b);
void *umemcpy(void *dst, const void *src, size_t n);
int umemcmp(const void *a, const void *b, size_t n);
void uputs(const char *s);                  /* write to the console */
void uputdec(int64_t value);
void uputhex(uint64_t value);

/* Provided by the program. */
int main(void);
#endif
