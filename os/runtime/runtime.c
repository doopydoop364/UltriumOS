#include "ultrium.h"

__attribute__((noreturn)) void ultrium_exit(int status) {
    ultrium_syscall3(ULTRIUM_SYS_EXIT, (int64_t)status, 0, 0);
    for (;;) __asm__ volatile("ud2");         /* exit does not return */
}
int64_t ultrium_write_console(const char *text, size_t length) { return ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, text, length, 0); }
int64_t ultrium_open(const char *path, uint32_t flags) { return ultrium_syscall3(ULTRIUM_SYS_OPEN, path, ustrlen(path), flags); }
int64_t ultrium_read(int fd, void *buffer, size_t length) { return ultrium_syscall3(ULTRIUM_SYS_READ, (int64_t)fd, buffer, length); }
int64_t ultrium_write(int fd, const void *buffer, size_t length) { return ultrium_syscall3(ULTRIUM_SYS_WRITE, (int64_t)fd, buffer, length); }
int64_t ultrium_close(int fd) { return ultrium_syscall3(ULTRIUM_SYS_CLOSE, (int64_t)fd, 0, 0); }
int64_t ultrium_yield(void) { return ultrium_syscall3(ULTRIUM_SYS_YIELD, 0, 0, 0); }
int64_t ultrium_get_pid(void) { return ultrium_syscall3(ULTRIUM_SYS_GET_PID, 0, 0, 0); }
uint64_t ultrium_ticks(void) { return (uint64_t)ultrium_syscall3(ULTRIUM_SYS_GET_TICKS, 0, 0, 0); }
int64_t ultrium_sleep(uint64_t ticks) { return ultrium_syscall3(ULTRIUM_SYS_SLEEP, ticks, 0, 0); }
int64_t ultrium_abi_version(void) { return ultrium_syscall3(ULTRIUM_SYS_ABI_VERSION, 0, 0, 0); }

size_t ustrlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int ustrcmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
void *umemcpy(void *dst, const void *src, size_t n) { uint8_t *d = dst; const uint8_t *s = src; while (n--) *d++ = *s++; return dst; }
int umemcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; --n, ++x, ++y) if (*x != *y) return *x - *y;
    return 0;
}
/* The compiler may emit calls to these even in freestanding code. */
void *memcpy(void *dst, const void *src, size_t n) { return umemcpy(dst, src, n); }
void *memset(void *dst, int c, size_t n) { uint8_t *d = dst; while (n--) *d++ = (uint8_t)c; return dst; }
int memcmp(const void *a, const void *b, size_t n) { return umemcmp(a, b, n); }

void uputs(const char *s) { (void)ultrium_write_console(s, ustrlen(s)); }
void uputdec(int64_t value) {
    char buf[24];
    unsigned i = sizeof(buf);
    uint64_t v = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    buf[--i] = 0;
    do { buf[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    if (value < 0) buf[--i] = '-';
    uputs(buf + i);
}
void uputhex(uint64_t value) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; ++i) { unsigned d = (unsigned)((value >> (60 - 4 * i)) & 15); buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
    buf[18] = 0;
    uputs(buf);
}

/* Entry point: the kernel starts the program here with registers cleared and a 16-byte-aligned stack
   minus 8, exactly as if _start had been called. */
__attribute__((noreturn, used)) void _start(void) { ultrium_exit(main()); }
