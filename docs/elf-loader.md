# ELF64 loader

Status: **implemented and QEMU-tested** (`ultrium test elf`, 6 cases). Code: `kernel/src/processes/elf.[ch]`.

## Executable ABI

A native Ultrium program is a **statically linked, non-PIE, little-endian x86-64 ELF64 `ET_EXEC`** file. There is no dynamic linker, no shared library, no TLS, no relocation processing, no `argv`/`envp`/auxiliary vector (the process starts with all registers zero and `rsp` pointing 8 bytes below a 16-byte-aligned stack top, so a normal C `_start` sees the usual alignment). Programs call the kernel only through the [system-call ABI](syscall-abi.md).

Address map: segments must lie in `[0x400000, 0x7fffffff0000)`; the stack is 8 pages ending at `0x7ffffff00000` (the page below stays unmapped as a guard); nothing else is mapped, no heap exists yet.

## What is validated (`elf_parse`, no side effects)

| Check | Error |
| --- | --- |
| file larger than 256 KiB, shorter than 64 bytes or than any table it claims | `TOO_BIG`, `TOO_SMALL` |
| magic, class 64, little-endian, ident and header version | `MAGIC`, `CLASS`, `ENDIAN`, `VERSION` |
| `e_type == ET_EXEC` (`ET_DYN`/PIE, `ET_REL`, `ET_CORE` refused), `e_machine == x86-64` | `TYPE`, `MACHINE` |
| `e_ehsize == 64`, `e_phentsize == 56`, 1–16 program headers, at most 8 `PT_LOAD` | `HEADER` |
| program-header table inside the file, overflow-safe | `PHDR_RANGE` |
| segment types: `PT_LOAD`, `PT_NULL/NOTE/PHDR/GNU_EH_FRAME/RELRO/PROPERTY` accepted; `PT_INTERP`, `PT_DYNAMIC`, `PT_SHLIB`, `PT_TLS`, unknown types, and a `PT_GNU_STACK` that asks for an executable stack are refused | `SEGMENT_TYPE` |
| `p_offset + p_filesz` inside the file, `p_filesz <= p_memsz` | `SEGMENT_BOUNDS` |
| virtual range inside the user range, no wraparound, canonical, alignment a power of two | `SEGMENT_ADDR` |
| not writable and executable, no unknown flag bits | `SEGMENT_PERM` |
| page ranges of any two segments do not overlap or share a page | `OVERLAP` |
| each segment ≤ 1 MiB, total ≤ 512 pages | `TOO_BIG` |
| entry point inside an executable `PT_LOAD` | `ENTRY` |
| at least one non-empty `PT_LOAD` | `NO_LOAD` |

A segment is mapped read-execute, read-write or read-only according to `p_flags`; bytes beyond `p_filesz` (bss) are zero. Nothing the file says can place a mapping in the kernel half: the user-range check is the same one the address-space layer enforces independently.

## Loading

`elf_spawn_image`/`elf_spawn_file` create a process, map and fill each segment (writable first so the loader can copy, then tightened to the final permissions), map the stack and start it. Any failure discards the process, its address space and every frame; the tests inject allocation failure at every step and fill the process table to prove it. `elf_spawn_file` takes an absolute VFS path and reads the file through the VFS into a temporary kernel buffer (released on every path): the loader does not know what filesystem holds the file (RAMFS and UltriumFS look the same).

## Verified

55-entry mutation table of header/segment corruptions with the exact expected error (the table is in `kernel/src/tests/elf_tests.c`), truncation at 18 boundaries, 4000 randomly corrupted images (accepted ones must satisfy every invariant), loading from RAMFS, missing/directory/empty/text files, process-table exhaustion, allocation failure at each step. Negative controls: removing the W+X check or the entry check is detected.

Not covered: a hostile *program* (rather than a hostile file) is covered by the ring-3 tests; ELF notes, build IDs and debug sections are ignored, not parsed.
