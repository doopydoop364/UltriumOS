#include "ultrium.h"
/* Verifies the file written by filedemo on the persistent volume. Exit 0 = present and identical. */
static const char message[] = "Written by filedemo through system calls.\n";
int main(void) {
    int64_t fd = ultrium_open("/volumes/disk/filedemo.txt", ULTRIUM_O_READ);
    if (fd < 0) { uputs("filecheck: the file is missing\n"); return 1; }
    char back[96];
    int64_t got = ultrium_read((int)fd, back, sizeof(back));
    (void)ultrium_close((int)fd);
    if (got != (int64_t)(sizeof(message) - 1) || umemcmp(back, message, (size_t)got)) { uputs("filecheck: content differs\n"); return 2; }
    uputs("filecheck: persistent file verified\n");
    return 0;
}
