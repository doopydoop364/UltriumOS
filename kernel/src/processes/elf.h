#ifndef ULTRIUM_ELF_H
#define ULTRIUM_ELF_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal ELF64 loader for native Ultrium programs (docs/elf-loader.md).
   Accepts only statically linked, non-PIE x86-64 little-endian ET_EXEC files. */

#define ELF_MAX_FILE_BYTES (256U * 1024U)
#define ELF_MAX_SEGMENTS 8U
#define ELF_MAX_PAGES 512U                  /* total pages of all PT_LOAD segments */

enum elf_error {
    ELF_OK = 0,
    ELF_E_IO,            /* cannot read the file */
    ELF_E_NOT_FOUND,
    ELF_E_NOT_FILE,
    ELF_E_TOO_BIG,       /* file or memory image above the limits */
    ELF_E_TOO_SMALL,     /* shorter than a header or a table it claims */
    ELF_E_MAGIC,
    ELF_E_CLASS,         /* not 64-bit */
    ELF_E_ENDIAN,        /* not little-endian */
    ELF_E_VERSION,
    ELF_E_TYPE,          /* not ET_EXEC (dynamic, relocatable, core are refused) */
    ELF_E_MACHINE,       /* not x86-64 */
    ELF_E_HEADER,        /* inconsistent header sizes or counts */
    ELF_E_PHDR_RANGE,    /* program header table outside the file */
    ELF_E_SEGMENT_TYPE,  /* dynamic linking, TLS, unknown type, executable stack */
    ELF_E_SEGMENT_BOUNDS,/* offset/size outside the file, filesz > memsz */
    ELF_E_SEGMENT_ADDR,  /* outside the user range, wraps, misaligned flag combination */
    ELF_E_SEGMENT_PERM,  /* writable and executable */
    ELF_E_OVERLAP,
    ELF_E_ENTRY,         /* entry point not inside an executable PT_LOAD */
    ELF_E_NO_LOAD,
    ELF_E_NOMEM,
    ELF_E_PROCESS        /* no process slot, or a mapping failed */
};
const char *elf_error_name(enum elf_error error);

struct elf_segment { uint64_t vaddr, offset, filesz, memsz; uint32_t permissions; };   /* permissions: enum vmm_page_permissions */
struct elf_plan { uint64_t entry; unsigned count; struct elf_segment segment[ELF_MAX_SEGMENTS]; uint32_t pages; };

/* Pure validation of an in-memory image; touches no process and allocates nothing. */
enum elf_error elf_parse(const uint8_t *image, size_t size, struct elf_plan *plan);
/* Create a process from a validated image: map and fill the segments, map the stack and start it.
   On any failure nothing remains (no process, no frames). Returns the PID or 0. */
uint32_t elf_spawn_image(const char *name, uint32_t parent_pid, const uint8_t *image, size_t size, enum elf_error *error);
/* Read `absolute_path` through the VFS (any filesystem) and spawn it. */
uint32_t elf_spawn_file(const char *absolute_path, uint32_t parent_pid, enum elf_error *error);
#endif
