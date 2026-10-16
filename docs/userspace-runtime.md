# Userspace runtime and sample programs

Status: **implemented and QEMU-tested** (`ultrium test programs`, 6 cases). Component: **Ultrium OS** (`os/`), consuming only the shared syscall ABI (`shared/abi/ultrium_syscall.h`); it links against nothing from the kernel.

## Layout

```text
os/runtime/ultrium.h   syscall wrappers, string helpers, console helpers (no C library)
os/runtime/runtime.c   implementations + _start (calls main, then exit)
os/link.ld             compact two-segment layout: R+X text/rodata, R+W data/bss on its own page
os/programs/*.c        hello, counter, filedemo, fpudemo
os/GNUmakefile         builds os/bin/*.elf (clang+lld with TOOLCHAIN=llvm)
kernel/src/processes/programs.c   embeds os/bin/*.elf and installs them as /bin/<name> at boot
```

Build: `make TOOLCHAIN=llvm` at the repository root builds `os/` first (the kernel makefile's `user-programs` target) and embeds the ELF files. `make -C os TOOLCHAIN=llvm` builds the programs alone. The embedding is only a delivery mechanism: the programs are copied into RAMFS (`/bin`) at boot and every launch loads them from the VFS through the [ELF loader](elf-loader.md); a program copied elsewhere (including onto an UltriumFS volume) runs identically. RAMFS files are limited to 4096 bytes, so the programs are linked compactly (stripped, one text segment, no padding) and are 2.8–3.5 KB.

## Program environment

- Entry `_start`; the process starts with all registers zero, `rsp` = stack top − 8 (so `_start` sees a normal call alignment), no `argv`/`envp`.
- The stack is 8 pages; there is no heap yet (no `brk`/`mmap`): programs use the stack and static data.
- Return value of `main` becomes the exit status. A fault exits with `-1000 - vector` (see [process-model.md](process-model.md)).
- User code may use SSE/x87 (`fpudemo`); the kernel saves and restores that state on every switch between user tasks. AVX is not enabled.
- The runtime exports `memcpy`, `memset`, `memcmp` because compilers may call them even in freestanding code.

## Sample programs

| Program | What it shows | Checked by the test |
| --- | --- | --- |
| `hello` | console syscall, `get_pid`, `abi_version` | exact output and exit 0 |
| `counter` | five CPU-bound steps with no syscall inside; two copies interleave | both finish; their tick ranges overlap; the scheduler counted preemptions |
| `filedemo` | `open(CREATE)`, `write`, `close`, `open`, `read`, compare. Uses `/volumes/disk/filedemo.txt` when that path accepts a file (a mounted persistent volume), otherwise `/filedemo.txt` | the file exists with the exact bytes; descriptors were released (the file can be removed) |
| `fpudemo` | double-precision sum depending on the pid, with yields and preemption | three copies at once all compute the exact result: no FP state leak or corruption |

## Not provided

A C library, `printf`, `malloc`, argument passing, environment, signals, threads, dynamic linking, a program that reads the keyboard. Adding any of these is a separate ABI/runtime change.
