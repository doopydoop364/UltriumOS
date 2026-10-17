#include "ultrium.h"
/* Writes /volumes/disk/spam.bin in 4 KiB chunks until the volume is full or the machine is stopped.
   Byte at offset o is (77 + o*31 + (o>>9)) & 255, so a host can verify any prefix. */
int main(void) {
    int64_t fd = ultrium_open("/volumes/disk/spam.bin", ULTRIUM_O_WRITE | ULTRIUM_O_CREATE);
    if (fd < 0) return 1;
    static uint8_t chunk[4096];
    for (uint64_t offset = 0;; offset += sizeof(chunk)) {
        for (uint64_t i = 0; i < sizeof(chunk); ++i) { uint64_t o = offset + i; chunk[i] = (uint8_t)(77 + o * 31 + (o >> 9)); }
        if (ultrium_write((int)fd, chunk, sizeof(chunk)) != (int64_t)sizeof(chunk)) break;
    }
    (void)ultrium_close((int)fd);
    return 0;
}
