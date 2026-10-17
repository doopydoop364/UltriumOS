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
EMBED(badcalls)
EMBED(filecheck)
EMBED(sleeper)
EMBED(crash)
EMBED(diskwriter)

/* `test_only` programs exist to exercise the system (hostile calls, crashes, long sleeps, a disk writer). They are
   not installed at boot - the RAM filesystem has only 64 KiB of file data - and tests install them on demand. */
static const struct { const char *name; const uint8_t *start, *end; bool test_only; } programs[] = {
    {"hello", ultrium_prog_hello_start, ultrium_prog_hello_end, false},
    {"counter", ultrium_prog_counter_start, ultrium_prog_counter_end, false},
    {"filedemo", ultrium_prog_filedemo_start, ultrium_prog_filedemo_end, false},
    {"fpudemo", ultrium_prog_fpudemo_start, ultrium_prog_fpudemo_end, false},
    {"spin", ultrium_prog_spin_start, ultrium_prog_spin_end, false},
    {"badcalls", ultrium_prog_badcalls_start, ultrium_prog_badcalls_end, true},
    {"filecheck", ultrium_prog_filecheck_start, ultrium_prog_filecheck_end, true},
    {"sleeper", ultrium_prog_sleeper_start, ultrium_prog_sleeper_end, true},
    {"crash", ultrium_prog_crash_start, ultrium_prog_crash_end, true},
    {"diskwriter", ultrium_prog_diskwriter_start, ultrium_prog_diskwriter_end, true},
};
#define COUNT (sizeof(programs) / sizeof(programs[0]))

unsigned programs_count(void) { return (unsigned)COUNT; }
const char *programs_name(unsigned i) { return i < COUNT ? programs[i].name : NULL; }
bool programs_is_test_only(unsigned i) { return i < COUNT && programs[i].test_only; }
bool programs_image(unsigned i, const uint8_t **data, size_t *size) {
    if (i >= COUNT || !data || !size) return false;
    *data = programs[i].start; *size = (size_t)(programs[i].end - programs[i].start);
    return true;
}

static void program_path(unsigned i, char *path, size_t capacity) {
    size_t n = 0;
    for (const char *p = PROGRAMS_DIRECTORY "/"; *p && n + 1 < capacity; ++p) path[n++] = *p;
    for (const char *p = programs[i].name; *p && n + 1 < capacity; ++p) path[n++] = *p;
    path[n] = 0;
}
bool programs_install_set(bool test_set, bool install) {
    enum vfs_error e = vfs_create_directory(PROGRAMS_DIRECTORY);
    if (e != VFS_OK && e != VFS_EXISTS) return false;
    bool ok = true;
    for (unsigned i = 0; i < COUNT; ++i) {
        if (programs[i].test_only != test_set) continue;
        char path[48];
        program_path(i, path, sizeof(path));
        if (!install) { e = vfs_unlink(path); if (e != VFS_OK && e != VFS_NOT_FOUND) ok = false; continue; }
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
bool programs_install(void) { return programs_install_set(false, true); }
