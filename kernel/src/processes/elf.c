#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "processes/elf.h"
#include "processes/process.h"
#include "memory/aspace.h"
#include "memory/vmm.h"
#include "fs/vfs.h"

#define EI_NIDENT 16
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_NULL 0U
#define PT_LOAD 1U
#define PT_DYNAMIC 2U
#define PT_INTERP 3U
#define PT_NOTE 4U
#define PT_SHLIB 5U
#define PT_PHDR 6U
#define PT_TLS 7U
#define PT_GNU_EH_FRAME 0x6474e550U
#define PT_GNU_STACK 0x6474e551U
#define PT_GNU_RELRO 0x6474e552U
#define PT_GNU_PROPERTY 0x6474e553U
#define PF_X 1U
#define PF_W 2U
#define PF_R 4U

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

const char *elf_error_name(enum elf_error e) {
    static const char *const names[] = {
        "ok", "cannot read file", "file not found", "not a regular file", "too big", "truncated", "bad magic",
        "not 64-bit", "not little-endian", "bad version", "not a static executable (ET_EXEC)", "not x86-64",
        "inconsistent header", "program headers outside the file", "unsupported segment type",
        "segment outside the file", "segment address invalid", "segment is writable and executable",
        "segments overlap", "entry point not in executable code", "no loadable segment", "out of memory",
        "process creation failed"};
    return (unsigned)e < sizeof(names) / sizeof(names[0]) ? names[e] : "unknown";
}

enum elf_error elf_parse(const uint8_t *image, size_t size, struct elf_plan *plan) {
    if (!image || !plan) return ELF_E_IO;
    *plan = (struct elf_plan){0};
    if (size > ELF_MAX_FILE_BYTES) return ELF_E_TOO_BIG;
    if (size < 64) return ELF_E_TOO_SMALL;
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') return ELF_E_MAGIC;
    if (image[4] != 2) return ELF_E_CLASS;
    if (image[5] != 1) return ELF_E_ENDIAN;
    if (image[6] != 1) return ELF_E_VERSION;
    if (rd16(image + 16) != ET_EXEC) return ELF_E_TYPE;
    if (rd16(image + 18) != EM_X86_64) return ELF_E_MACHINE;
    if (rd32(image + 20) != 1) return ELF_E_VERSION;
    uint64_t entry = rd64(image + 24), phoff = rd64(image + 32);
    uint16_t ehsize = rd16(image + 52), phentsize = rd16(image + 54), phnum = rd16(image + 56);
    if (ehsize != 64 || phentsize != 56 || phnum == 0 || phnum > 16) return ELF_E_HEADER;
    if (phoff < 64 || phoff > size || (uint64_t)phnum * 56U > size - phoff) return ELF_E_PHDR_RANGE;
    uint32_t total_pages = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *ph = image + phoff + (uint64_t)i * 56U;
        uint32_t type = rd32(ph), flags = rd32(ph + 4);
        uint64_t offset = rd64(ph + 8), vaddr = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40), align = rd64(ph + 48);
        switch (type) {
            case PT_NULL: case PT_NOTE: case PT_PHDR: case PT_GNU_EH_FRAME: case PT_GNU_RELRO: case PT_GNU_PROPERTY: continue;
            case PT_GNU_STACK: if (flags & PF_X) return ELF_E_SEGMENT_TYPE; continue;     /* an executable stack is refused */
            case PT_LOAD: break;
            default: return ELF_E_SEGMENT_TYPE;      /* PT_INTERP, PT_DYNAMIC, PT_SHLIB, PT_TLS, anything unknown */
        }
        if (plan->count >= ELF_MAX_SEGMENTS) return ELF_E_HEADER;
        if (flags & ~(PF_R | PF_W | PF_X)) return ELF_E_SEGMENT_PERM;
        if ((flags & PF_W) && (flags & PF_X)) return ELF_E_SEGMENT_PERM;
        if (filesz > memsz) return ELF_E_SEGMENT_BOUNDS;
        if (offset > size || filesz > size - offset) return ELF_E_SEGMENT_BOUNDS;
        if (!memsz) continue;                                            /* nothing to map */
        if (memsz > (uint64_t)ASPACE_MAP_MAX_PAGES * 4096U) return ELF_E_TOO_BIG;
        if (align > 1 && (align & (align - 1))) return ELF_E_SEGMENT_ADDR;
        if (!aspace_user_bounds_ok(vaddr, (size_t)memsz)) return ELF_E_SEGMENT_ADDR;
        uint64_t first = vaddr & ~4095ULL, last = (vaddr + memsz + 4095ULL) & ~4095ULL;
        if ((last - first) / 4096U > ASPACE_MAP_MAX_PAGES) return ELF_E_TOO_BIG;       /* the page span, not just memsz, must fit one mapping */
        for (unsigned k = 0; k < plan->count; ++k) {
            uint64_t f2 = plan->segment[k].vaddr & ~4095ULL, l2 = (plan->segment[k].vaddr + plan->segment[k].memsz + 4095ULL) & ~4095ULL;
            if (first < l2 && f2 < last) return ELF_E_OVERLAP;           /* includes two segments sharing a page */
        }
        total_pages += (uint32_t)((last - first) / 4096U);
        if (total_pages > ELF_MAX_PAGES) return ELF_E_TOO_BIG;
        plan->segment[plan->count++] = (struct elf_segment){vaddr, offset, filesz, memsz,
            (flags & PF_X) ? (VMM_PAGE_READ | VMM_PAGE_EXECUTE) : (flags & PF_W) ? (VMM_PAGE_READ | VMM_PAGE_WRITE) : VMM_PAGE_READ};
    }
    if (!plan->count) return ELF_E_NO_LOAD;
    bool entry_ok = false;
    for (unsigned k = 0; k < plan->count; ++k) {
        const struct elf_segment *s = &plan->segment[k];
        if ((s->permissions & VMM_PAGE_EXECUTE) && entry >= s->vaddr && entry - s->vaddr < s->memsz) entry_ok = true;
    }
    if (!entry_ok) return ELF_E_ENTRY;
    plan->entry = entry;
    plan->pages = total_pages;
    return ELF_OK;
}

uint32_t elf_spawn_image(const char *name, uint32_t parent_pid, const uint8_t *image, size_t size, enum elf_error *error) {
    struct elf_plan plan;
    enum elf_error e = elf_parse(image, size, &plan);
    uint32_t pid = 0;
    if (e == ELF_OK) {
        pid = process_create(name, parent_pid);
        if (!pid) e = ELF_E_PROCESS;
    }
    if (e == ELF_OK) {
        for (unsigned i = 0; i < plan.count && e == ELF_OK; ++i) {
            const struct elf_segment *s = &plan.segment[i];
            if (!process_map_segment(pid, s->vaddr, image + s->offset, (size_t)s->filesz, (size_t)s->memsz, s->permissions)) e = ELF_E_PROCESS;
        }
        if (e == ELF_OK && !process_map_stack(pid)) e = ELF_E_PROCESS;
        if (e == ELF_OK && !process_start(pid, plan.entry, PROCESS_STACK_TOP - 8)) e = ELF_E_PROCESS;
        if (e != ELF_OK) { (void)process_discard(pid); pid = 0; }
    }
    if (error) *error = e;
    return e == ELF_OK ? pid : 0;
}

uint32_t elf_spawn_file(const char *path, uint32_t parent_pid, enum elf_error *error) {
    enum elf_error e = ELF_OK;
    uint32_t pid = 0;
    struct vfs_info info;
    uint8_t *buffer = NULL;
    struct vfs_file f = {0};
    enum vfs_error v = vfs_stat(path, &info);
    if (v == VFS_NOT_FOUND || v == VFS_INVALID_PATH) e = ELF_E_NOT_FOUND;
    else if (v != VFS_OK) e = ELF_E_IO;
    else if (info.type != VFS_NODE_FILE) e = ELF_E_NOT_FILE;
    else if (info.size > ELF_MAX_FILE_BYTES) e = ELF_E_TOO_BIG;
    else if (info.size < 64) e = ELF_E_TOO_SMALL;
    if (e == ELF_OK) {
        buffer = vmm_alloc_pages((info.size + 4095) / 4096);
        if (!buffer) e = ELF_E_NOMEM;
    }
    if (e == ELF_OK && vfs_file_open(path, VFS_OPEN_READ, &f) != VFS_OK) e = ELF_E_IO;
    if (e == ELF_OK) {
        size_t have = 0;
        while (have < info.size) {
            size_t got = 0;
            v = vfs_file_read(&f, buffer + have, info.size - have, &got);
            if ((v != VFS_OK && v != VFS_END) || !got) break;
            have += got;
        }
        (void)vfs_file_close(&f);
        if (have != info.size) e = ELF_E_IO;
    }
    if (e == ELF_OK) {
        const char *base = path, *p = path;
        for (; *p; ++p) if (*p == '/') base = p + 1;
        pid = elf_spawn_image(base, parent_pid, buffer, info.size, &e);
    }
    if (buffer) (void)vmm_free_pages(buffer);
    if (error) *error = e;
    return e == ELF_OK ? pid : 0;
}
