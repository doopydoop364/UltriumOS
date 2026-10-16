#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "processes/programs.h"
#include "fs/vfs.h"

#define EMBED(name) \
    __asm__(".section .rodata\n.balign 16\n" \
            ".global ultrium_prog_" #name "_start\nultrium_prog_" #name "_start:\n" \
            ".incbin \"../os/bin/" #name ".elf\"\n" \
            ".global ultrium_prog_" #name "_end\nultrium_prog_" #name "_end:\n.previous\n"); \
    extern const uint8_t ultrium_prog_##name##_start[], ultrium_prog_##name##_end[];
EMBED(hello)
EMBED(counter)
EMBED(filedemo)
EMBED(fpudemo)
EMBED(spin)

static const struct { const char *name; const uint8_t *start, *end; } programs[] = {
    {"hello", ultrium_prog_hello_start, ultrium_prog_hello_end},
    {"counter", ultrium_prog_counter_start, ultrium_prog_counter_end},
    {"filedemo", ultrium_prog_filedemo_start, ultrium_prog_filedemo_end},
    {"fpudemo", ultrium_prog_fpudemo_start, ultrium_prog_fpudemo_end},
    {"spin", ultrium_prog_spin_start, ultrium_prog_spin_end},
};
#define COUNT (sizeof(programs) / sizeof(programs[0]))

unsigned programs_count(void) { return (unsigned)COUNT; }
const char *programs_name(unsigned i) { return i < COUNT ? programs[i].name : NULL; }
bool programs_image(unsigned i, const uint8_t **data, size_t *size) {
    if (i >= COUNT || !data || !size) return false;
    *data = programs[i].start; *size = (size_t)(programs[i].end - programs[i].start);
    return true;
}

bool programs_install(void) {
    enum vfs_error e = vfs_create_directory(PROGRAMS_DIRECTORY);
    if (e != VFS_OK && e != VFS_EXISTS) return false;
    bool ok = true;
    for (unsigned i = 0; i < COUNT; ++i) {
        char path[48] = PROGRAMS_DIRECTORY "/";
        size_t n = sizeof(PROGRAMS_DIRECTORY);
        for (const char *p = programs[i].name; *p && n + 1 < sizeof(path); ++p) path[n++] = *p;
        path[n] = 0;
        size_t size = (size_t)(programs[i].end - programs[i].start);
        e = vfs_create_file(path);
        if (e != VFS_OK && e != VFS_EXISTS) { ok = false; continue; }
        struct vfs_file f = {0};
        size_t written = 0;
        if (vfs_file_open(path, VFS_OPEN_WRITE, &f) != VFS_OK) { ok = false; continue; }
        if (vfs_file_write(&f, programs[i].start, size, &written) != VFS_OK || written != size) ok = false;
        if (vfs_file_close(&f) != VFS_OK) ok = false;
    }
    return ok;
}
