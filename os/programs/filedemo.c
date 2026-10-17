#include "ultrium.h"
/* Writes a file, reads it back and compares. Uses /volumes/disk when a persistent volume is mounted
   there, otherwise the volatile root filesystem. Exit status: 0 success, otherwise the failing step. */
static const char message[] = "Written by filedemo through system calls.\n";
int main(void) {
    const char *path = "/volumes/disk/filedemo.txt";
    int64_t fd = ultrium_open(path, ULTRIUM_O_WRITE | ULTRIUM_O_CREATE);
    if (fd < 0) { path = "/filedemo.txt"; fd = ultrium_open(path, ULTRIUM_O_WRITE | ULTRIUM_O_CREATE); }
    if (fd < 0) { uputs("filedemo: open for writing failed: "); uputdec(fd); uputs("\n"); return 1; }
    int64_t n = ultrium_write((int)fd, message, sizeof(message) - 1);
    if (n != (int64_t)(sizeof(message) - 1)) { uputs("filedemo: short write\n"); return 2; }
    if (ultrium_close((int)fd) != 0) { uputs("filedemo: close failed\n"); return 3; }
    fd = ultrium_open(path, ULTRIUM_O_READ);
    if (fd < 0) { uputs("filedemo: open for reading failed\n"); return 4; }
    char back[96];
    int64_t got = ultrium_read((int)fd, back, sizeof(back));
    (void)ultrium_close((int)fd);
    if (got != (int64_t)(sizeof(message) - 1) || umemcmp(back, message, (size_t)got)) { uputs("filedemo: content mismatch\n"); return 5; }
    uputs("filedemo: wrote and read back "); uputdec(got); uputs(" bytes at "); uputs(path); uputs(": OK\n");
    return 0;
}
