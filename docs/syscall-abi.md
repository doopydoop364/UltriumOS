# Ultrium system-call ABI, version 1.0

Status: **implemented and QEMU-tested** (`ultrium test syscalls`, `ultrium test ring3`). The definitions live in [`shared/abi/ultrium_syscall.h`](../shared/abi/ultrium_syscall.h), the one header shared by the kernel and userspace programs. Everything in this document is also in that header; if they disagree, the header is wrong and must be fixed.

This is an **Ultrium** ABI. Numbers and error codes are deliberately not Linux's, and nothing here implies POSIX compatibility.

## Entry mechanism

`int $0x80` (IDT vector 0x80, an interrupt gate with DPL 3, so interrupts stay disabled inside the call). It is the bring-up mechanism and is **not** the final one: `SYSCALL`/`SYSRET` may be added later under a new minor ABI version. `EFER.SCE` is cleared by the kernel, so executing `syscall` in ring 3 raises #UD and ends the process rather than jumping through firmware-provided state. Calling `int 0x80` from ring 0 is refused.

The return path is `iretq` from the interrupt frame the CPU pushed, so it is always valid: there is no non-canonical `SYSRET` hazard in this version.

## Calling convention

| Item | Register |
| --- | --- |
| System call number | `rax` |
| Arguments 1–6 | `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` |
| Result | `rax`, signed 64-bit |
| Preserved | every other register, including `rcx` and `r11`, and `rflags` |

A negative result is the negated error code (`-ULTRIUM_E_*`). Zero or positive results are successes. Arguments are 64-bit values; the kernel treats 32-bit parameters (`fd`, `flags`, `status`) as the low bits only where documented and rejects values that do not fit. The stack is not used to pass data. The kernel does not touch the user stack.

## Versioning

`ULTRIUM_ABI_VERSION = (major << 16) | minor`, currently **1.0** (`0x00010000`), returned by `abi_version`. Within a major version numbers and meanings never change; new calls take new numbers and raise the minor version. A program that needs a call checks the version first. An unknown number returns `-ULTRIUM_E_NOSYS` and the program continues; it is not a fault.

## Calls

| # | Name | Arguments | Result |
| --- | --- | --- | --- |
| 0 | `exit` | `int32 status` | does not return. The low 32 bits of `rdi` are sign-extended into the process exit status |
| 1 | `yield` | – | 0 |
| 2 | `write_console` | `const char *buf, size_t len` | `len`, or an error |
| 3 | `get_pid` | – | the caller's PID |
| 4 | `open` | `const char *path, size_t path_len, uint32 flags` | descriptor ≥ 3, or an error |
| 5 | `read` | `int fd, void *buf, size_t len` | bytes read; 0 at end of file |
| 6 | `write` | `int fd, const void *buf, size_t len` | bytes written |
| 7 | `close` | `int fd` | 0 |
| 8 | `abi_version` | – | `ULTRIUM_ABI_VERSION` |
| 9 | `get_ticks` | – | timer ticks since boot (100 Hz) |
| 10 | `sleep` | `uint64 ticks` | 0 after at least `ticks` ticks; `ticks` ≤ 6000 |

### Descriptors

16 per process. 0 = standard input (always at end of file: there is no input device for programs yet), 1 = standard output and 2 = standard error (both the console), 3–15 = files from `open`. The console descriptors cannot be closed. A descriptor remembers the access it was opened with; `read` needs `ULTRIUM_O_READ` and `write` needs `ULTRIUM_O_WRITE`, otherwise `-ULTRIUM_E_BADF`. All descriptors are closed when the process exits, is killed or faults.

### Files

`open` takes an **absolute** path (`/…`, at most 127 bytes, not NUL-terminated: the length is explicit; an embedded NUL is rejected). The kernel normalises it (`.`/`..`) through the VFS. Flags: `ULTRIUM_O_READ`, `ULTRIUM_O_WRITE`, `ULTRIUM_O_CREATE` (creates a regular file if missing; requires `WRITE`). There is no working directory, no truncate, no append and no seek in ABI 1.0: a descriptor starts at offset 0 and each read or write advances it. Writing from offset 0 overwrites; growth beyond the current end appends, as the VFS allows. Directories cannot be opened (`-ULTRIUM_E_ISDIR`). The VFS enforces the limits of its backend (RAMFS: 4096 bytes per file; UltriumFS: its own); `ULTRIUM_E_NOSPC`, `ULTRIUM_E_ROFS` and `ULTRIUM_E_IO` are reported as such.

**Access policy (temporary, not a security boundary for files):** there are no users, permissions or per-process roots yet. Any process may open any path the VFS serves, including persistent volumes, subject only to the descriptor rights above. The kernel does *not* pretend otherwise. Process authorisation will come with the permission design in [authorization-architecture.md](authorization-architecture.md).

### Console

`write_console` and writes to descriptors 1 and 2 go to the terminal. Only printable ASCII, `\n`, `\r` and `\t` pass through; every other byte is shown as `?`, so a program cannot inject terminal control sequences. Output is also kept in a bounded 4 KiB per-process buffer for diagnostics and tests.

## User pointer rules

The kernel never dereferences a user pointer. For every pointer argument it checks, **before doing anything**, that the whole range lies in `[0x400000, 0x7fffffff0000)` without overflow and that every page is present, user-accessible and (for output buffers) writable, by walking the process's page tables in software; data is then copied through the kernel's direct map. A range that fails any check returns `-ULTRIUM_E_FAULT` and has no side effect: nothing is written, no descriptor offset moves, no output is produced. A pointer into kernel memory, page 0, a gap, a read-only page used as an output buffer, or a range that runs off its mapping is simply `E_FAULT`; it does not kill the process (an actual memory access by the program to such an address does, as a page fault).

Lengths above `ULTRIUM_MAX_IO` (65536) return `-ULTRIUM_E_TOOBIG` before any pointer is examined. Large transfers are copied in bounded chunks through small kernel buffers.

## Errors

| Code | Name | Meaning |
| --- | --- | --- |
| 1 | `E_NOSYS` | no such system call |
| 2 | `E_INVAL` | invalid argument (flags, relative path, empty or NUL-containing path, sleep too long) |
| 3 | `E_FAULT` | bad user pointer range |
| 4 | `E_BADF` | not an open descriptor, or wrong access mode |
| 5 | `E_NOENT` | no such file |
| 6 | `E_NOMEM` | out of memory |
| 7 | `E_MFILE` | descriptor table full |
| 8 | `E_ISDIR` | is a directory |
| 9 | `E_NOTDIR` | a path component is not a directory |
| 10 | `E_EXIST` | already exists |
| 11 | `E_NOSPC` | no space |
| 12 | `E_IO` | device or filesystem error |
| 13 | `E_ROFS` | read-only volume |
| 14 | `E_NAMETOOLONG` | path longer than 127 bytes |
| 15 | `E_BUSY` | object busy |
| 16 | `E_ACCESS` | refused by policy (also: `int 0x80` from ring 0) |
| 17 | `E_NOPROC` | no such process |
| 18 | `E_TOOBIG` | length above `ULTRIUM_MAX_IO` |

## Blocking and scheduling

System calls run with interrupts disabled and are not preempted by the timer (kernel code is not preemptible, see [process-model.md](process-model.md)). `sleep` and `yield` give the CPU to other tasks. A long VFS operation delays the keyboard IRQ by its duration.

## What a program cannot do

It cannot call kernel functions (there is no kernel symbol table, the kernel half is unreachable from ring 3), access physical memory, change page tables or hardware (privileged instructions and port I/O fault and end the process), or reach another process's memory. It *can* read and write any file the VFS exposes (see the access policy).

## Not in ABI 1.0

`fork`/`exec`/`spawn`, `wait`, signals, `mmap`/`brk`, `seek`, `stat`, directory listing, `unlink`/`mkdir`/`rename`, pipes, time of day, threads, futexes, `SYSCALL`/`SYSRET`, ELF auxiliary vectors or `argv`/`envp` (a program receives no arguments yet).
