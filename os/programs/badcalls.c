#include "ultrium.h"
/* Issues hostile system calls and requires every one to be refused with the documented error. The
   process must survive all of them. Exit status 0 = all refused correctly, otherwise the number of the
   first call that was handled wrongly. */
#define ERR(e) (-(int64_t)(e))
static volatile uint64_t kernel_pointer = 0xffffffff80000000ULL;
static volatile uint64_t wrapping_pointer = ~0ULL - 3;
static volatile uint64_t huge = ~0ULL;
static char buffer[64];
int main(void) {
    if (ultrium_syscall3(999, 0, 0, 0) != ERR(ULTRIUM_E_NOSYS)) return 1;
    if (ultrium_syscall3(ULTRIUM_SYS_COUNT, 0, 0, 0) != ERR(ULTRIUM_E_NOSYS)) return 2;
    if (ultrium_syscall3(huge, 0, 0, 0) != ERR(ULTRIUM_E_NOSYS)) return 3;
    if (ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, kernel_pointer, 16, 0) != ERR(ULTRIUM_E_FAULT)) return 4;
    if (ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, 0, 16, 0) != ERR(ULTRIUM_E_FAULT)) return 5;
    if (ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, (uint64_t)buffer, ULTRIUM_MAX_IO + 1, 0) != ERR(ULTRIUM_E_TOOBIG)) return 6;
    if (ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, (uint64_t)buffer, huge, 0) != ERR(ULTRIUM_E_TOOBIG)) return 7;
    if (ultrium_syscall3(ULTRIUM_SYS_WRITE_CONSOLE, wrapping_pointer, 16, 0) != ERR(ULTRIUM_E_FAULT)) return 8;
    if (ultrium_read(-1, buffer, 4) != ERR(ULTRIUM_E_BADF)) return 9;
    if (ultrium_read(99, buffer, 4) != ERR(ULTRIUM_E_BADF)) return 10;
    if (ultrium_write(7, buffer, 4) != ERR(ULTRIUM_E_BADF)) return 11;
    if (ultrium_close(1) != ERR(ULTRIUM_E_BADF) || ultrium_close(-1) != ERR(ULTRIUM_E_BADF)) return 12;
    if (ultrium_open("relative", ULTRIUM_O_READ) != ERR(ULTRIUM_E_INVAL)) return 13;
    if (ultrium_open("/", ULTRIUM_O_READ) != ERR(ULTRIUM_E_ISDIR)) return 14;
    if (ultrium_open("/no/such/file", ULTRIUM_O_READ) >= 0) return 15;
    if (ultrium_open("/bin/hello", 0) != ERR(ULTRIUM_E_INVAL)) return 16;
    if (ultrium_syscall3(ULTRIUM_SYS_OPEN, kernel_pointer, 8, ULTRIUM_O_READ) != ERR(ULTRIUM_E_FAULT)) return 17;
    int64_t fd = ultrium_open("/bin/hello", ULTRIUM_O_READ);
    if (fd < 3) return 18;
    if (ultrium_syscall3(ULTRIUM_SYS_READ, (uint64_t)fd, kernel_pointer, 16) != ERR(ULTRIUM_E_FAULT)) return 19;
    if (ultrium_syscall3(ULTRIUM_SYS_READ, (uint64_t)fd, (uint64_t)(uintptr_t)&main, 16) != ERR(ULTRIUM_E_FAULT)) return 20;   /* code is not writable */
    if (ultrium_write((int)fd, buffer, 4) != ERR(ULTRIUM_E_BADF)) return 21;                                                   /* opened read-only */
    if (ultrium_read((int)fd, buffer, sizeof(buffer)) != (int64_t)sizeof(buffer)) return 22;                                   /* the failed reads consumed nothing */
    if (buffer[0] != 0x7f || buffer[1] != 'E') return 23;
    if (ultrium_close((int)fd) != 0 || ultrium_close((int)fd) != ERR(ULTRIUM_E_BADF)) return 24;
    uputs("badcalls: every hostile call was refused\n");
    return 0;
}
