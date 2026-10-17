# Ultrium process and thread model

Status: **specification plus the implementation status table at the end**. Rows marked implemented were built and QEMU-tested in the stacked PRs listed there; everything else in this file is design. Do not describe a row as working unless the table says so.

Scope: single CPU, ring 0 and ring 3, one thread per process initially. The design leaves room for several threads per process and several CPUs without requiring either now. See [component-architecture.md](component-architecture.md) for the Kernel / OS / GUI split: processes, the scheduler and system calls belong to the **Kernel**; programs and the runtime belong to **OS**.

## Vocabulary

| Term | Meaning |
| --- | --- |
| **Task** | A schedulable thread of execution: saved CPU state, a kernel stack, a scheduling state. The scheduler only knows tasks. |
| **Process** | The unit of resource ownership: an address space (userspace only), open files, a parent, an exit status. Owns one or more tasks. |
| **Kernel process** | PID 0. Owns the kernel threads (the boot/shell task, the idle task, test threads). It has no private address space: it runs on the kernel page tables. |
| **User process** | PID ≥ 1. Owns a private address space and runs in ring 3. Its task enters ring 0 only through a system call, an interrupt or an exception. |

Moving a source file into another directory does not create a process. A process exists only when the kernel has a process-table entry, a scheduled task and (for user processes) a separate address space.

## Identifiers

- **PID**: 32-bit, allocated from a monotonically increasing counter starting at 1. PID 0 is the kernel process. PIDs are **not reused** during a boot; the kernel refuses to create a process when the counter would wrap. An invalid or stale PID is always an error (`ULTRIUM_E_NO_PROCESS`), never a pointer dereference.
- **TID**: 32-bit, independent counter, starting at 1 for the idle task. The first (main) task of a user process has its own TID; `tid == pid` is not promised.
- Table slots are internal; callers hold PIDs/TIDs and the kernel looks them up and validates state on every use (generation-style handle checks, consistent with PMM handles).

Static limits (bounded allocations): **16 processes**, **16 tasks**, 16 open files per process, a 4 KiB per-process console capture buffer. Exhaustion returns an error and leaves no partial state.

## Lifecycle

Task and process share one state set:

```text
CREATED -> READY <-> RUNNING
              ^         |
              |         v
              +----- BLOCKED
RUNNING/READY/BLOCKED -> TERMINATED -> (reaped: slot free)
```

| State | Meaning |
| --- | --- |
| `CREATED` | Resources allocated, never run. Not in the run queue. Becomes `READY` only when creation fully succeeded. |
| `READY` | In the run queue, waiting for the CPU. |
| `RUNNING` | The one task executing on the CPU (single CPU: exactly one). |
| `BLOCKED` | Waiting for an event (child exit, tick deadline). Not in the run queue. Only the matching wake-up or termination leaves this state. |
| `TERMINATED` | Will never run again. Exit status retained for the parent. Resources are released only by the **reaper**, never by the dying task itself. |

Illegal transitions (for example waking a task that is not `BLOCKED`, or running a `TERMINATED` task) are rejected and counted; the scheduler never acts on them.

### Exit and cleanup

1. A task ends by calling `exit(status)`, by being terminated from the shell, or by a **process fault** (an exception taken in ring 3, which ends only that process).
2. The dying task becomes `TERMINATED` and the process stores `exit_status` (a fault stores a negative fault code).
3. The task switches away for the last time. It does not free its own kernel stack: it is still executing on it.
4. After the next context switch completes, the **reaper** (run by the incoming task on its own stack) releases, in order: open files, user pages and page tables (the address space), the kernel stack, the task slot. The address space is never freed while its page tables are the active `CR3`: the kernel switches to the kernel root first.
5. A terminated process stays in the table as a **zombie** (status only) until its parent collects it with `wait` or the shell reports it. Orphans (parent gone) are reaped immediately.
6. The kernel process and the idle task are never terminated.

## Parent and child

Every process records its parent PID. The parent of a process started from the shell is PID 0 (the kernel process, i.e. the shell). `wait(pid)` is kernel-internal today (used by the shell's foreground `run`); a userspace `wait` syscall is not part of ABI v1. Termination of a parent does not kill children in this version; they are re-parented to PID 0.

## Resource ownership

| Resource | Owner | Freed by |
| --- | --- | --- |
| Kernel stack (4 pages, VMM region) | task | reaper |
| User page frames, user page tables, PML4 | address space (PMM handles held in the aspace) | `aspace_destroy` |
| Open files | process fd table | process cleanup (each handle closed through the VFS) |
| Console capture buffer | process | with the process slot |
| Process / task slots | tables | reaper |

All physical frames stay behind PMM ownership handles (move-only, one releasing owner, see `physical-memory.md`). A frame mapped into a user address space is owned by exactly one aspace; two address spaces never share a frame in this version.

## Address spaces

- User processes get a new top-level page table whose **lower half (PML4 slots 0–255) is private and starts empty** and whose **upper half (slots 256–511) is copied from the kernel root** at creation, so the kernel, the direct map and the scratch/region mappings are present on every entry to ring 0 without switching `CR3`. The bootloader's own top-level entries can carry the User bit (the direct map's does), so each copy **clears it** - x86 user access needs the User bit at every level, so nothing under the kernel half is reachable from ring 3. Upper-half slots are populated before the first process is created; the kernel does not add new top-level slots later (the creation code checks this).
- User mappings carry the User bit; kernel mappings never do. W^X is enforced: a user page is writable or executable, never both. Data pages are `NX`.
- Valid user addresses are canonical lower-half addresses in `[0x0000000000400000, 0x00007fffffff0000)`. Page 0 and everything below 4 MiB is never mapped. A guard page below each user stack stays unmapped.
- The kernel never dereferences a user pointer directly. User memory is read and written by walking the process's page tables in software and copying through the direct map, validating Present/User/Write bits per page. This avoids relying on SMAP/exception fix-ups and cannot fault in the kernel.
- SMEP is enabled when the CPU supports it. SMAP is enabled only if the software walk is the sole user-access path (it is).
- Not provided: demand paging, copy-on-write, shared memory, `mmap`, address-space layout randomization, swapping.

## CPU context and switching

- A task has an independent **kernel stack** (4 pages + a canary word at the bottom checked on every switch; the VMM region API has no guard-page primitive yet, so overflow detection is by canary, not hardware). User tasks additionally have a **user stack** inside their address space.
- A voluntary switch saves exactly the callee-saved registers (`rbx, rbp, r12–r15`), `rflags` and `rsp` on the outgoing kernel stack and restores them from the incoming one; the instruction pointer is the return address. Everything else is already saved by the C calling convention or by the interrupt/exception entry stubs. A brand-new task starts at a trampoline that finishes the previous switch, enables interrupts and calls its entry function; user tasks then `iretq` to ring 3.
- Stack alignment: `rsp` is 16-byte aligned before every `call` made from the scheduler and trampoline.
- On every switch the scheduler sets `TSS.rsp0` to the incoming task's kernel stack top and loads `CR3` only if it differs. The double-fault IST stack (IST1) is unchanged and never reused by tasks.
- **Floating-point/SIMD policy:** the kernel is built with `-mno-sse -mno-mmx -mno-red-zone` and never uses SIMD or x87. User tasks may. Each user task owns a 512-byte `FXSAVE` area saved and restored **eagerly on every switch between user tasks** (`CR4.OSFXSR`/`OSXMMEXCPT` set by the kernel; `XSAVE`/AVX are not enabled, so AVX use faults with `#UD`). Kernel threads do not own FP state.

## Scheduling

- Algorithm: **round robin** over the READY queue (FIFO, bounded by the task table), time slice **5 ticks at 100 Hz = 50 ms**.
- **Preemption policy** (single CPU, kernel allocators are not reentrant):
  - A task executing in **ring 3** is preempted when its slice expires (timer interrupt, taken from user mode).
  - Kernel code is **not preemptible**: PMM, heap, VMM, VFS and filesystems are unsynchronised single-CPU code, and the scheduler does not claim otherwise. Kernel tasks switch only at explicit points (`yield`, `block`, `exit`, waiting for input). A kernel thread marked `PREEMPTIBLE` (used only by scheduler tests that touch no shared kernel state) may also be preempted by the timer.
  - System calls and exceptions run with interrupts disabled (interrupt gates); the keyboard IRQ is delayed for their duration.
- The **idle task** runs `sti; hlt` when nothing is READY. The boot/shell task yields while waiting for keyboard input, so background user processes run while the prompt is idle.
- Blocking and waking: a task blocks with a reason; `wake` moves exactly a `BLOCKED` task to READY. A wake-up that arrives before the block is recorded so it is not lost (the shell's `wait` checks the child state before blocking).
- Interrupt reentrancy: the scheduler's queues are modified only with interrupts disabled. The timer handler sends the APIC EOI **before** it may switch tasks.

### Why this is not SMP

Disabling interrupts protects the run queue against the timer on one CPU only. Real SMP needs per-CPU run queues and current-task pointers, per-CPU `TSS`/IST stacks and GS-based per-CPU data, IPIs for TLB shootdown and rescheduling, spinlocks with ordering rules around the PMM, heap, VMM and VFS, and APIC bring-up of application processors. None of this exists. Ultrium must not be described as SMP-capable.

## Future permissions

Processes will carry a credential/capability set checked by the kernel (see [authorization-architecture.md](authorization-architecture.md)). Today every process has the same, unprivileged-by-hardware but filesystem-unrestricted access: any user process can open any VFS path. This will be documented in `docs/syscall-abi.md` (added by the syscalls PR) and is **not** a security boundary for files. No `auth`/`as` mechanism exists.

## Implementation status

| Milestone | PR / branch | Status |
| --- | --- | --- |
| Process and thread model specification | `feature/process-model` | this document |
| Context switching | `feature/context-switching` | **implemented, QEMU-tested**: task table, 16 KiB kernel stacks with canary, register-level `task_switch_to`, TSS.RSP0 update, cooperative only (`ultrium test tasks`, 3 cases). No queues or preemption yet |
| Scheduler | `feature/scheduler` | **implemented, QEMU-tested**: round robin, 50 ms slices, yield/block/wake/sleep, idle task, reaper, timer preemption of ring-3 and `PREEMPTIBLE` kernel tasks only (`ultrium test scheduler`, 9 cases). Kernel code is not preemptible. Single CPU only |
| Per-process address spaces | `feature/user-address-spaces` | **implemented, QEMU-tested** (`memory/aspace.c`, `ultrium test aspace`, 9 cases): private PML4, shared kernel half with the User bit cleared, W^X, bounded mappings with rollback, software-walk validated copies, frame limit, destroy-while-active. No ring 3 code runs yet. SMEP/SMAP are enabled only if the CPU reports them (QEMU's default CPU model does not) |
| Ring 3 execution | `feature/ring3` | **implemented, QEMU-tested** (`processes/process.c`, `ultrium test ring3`, 6 cases): GDT with ring-3 code/data (0x23/0x1b) and TSS at 0x28, `iretq` entry, per-task `CR3` and eager FXSAVE switching, `int 0x80` gate (DPL 3) with only `exit`/`yield`, user faults end only the faulting process (exit status `-1000 - vector`), `EFER.SCE` cleared so `SYSCALL` raises #UD. Programs are hand-assembled images; no loader yet |
| System calls | `feature/syscalls` | **implemented, QEMU-tested** (`docs/syscall-abi.md`, `shared/abi/ultrium_syscall.h`, `ultrium test syscalls`, 4 cases): `int 0x80`, 11 calls incl. VFS-backed open/read/write/close, validated all-or-nothing user buffers, per-process descriptors released at exit |
| ELF64 loader | `feature/elf-loader` | **implemented, QEMU-tested** (`docs/elf-loader.md`, `ultrium test elf`, 6 cases): static `ET_EXEC` x86-64 only, 55 header/segment mutations, 4000-image fuzz, loading from the VFS |
| Userspace runtime and programs | `feature/userspace-runtime` | **implemented, QEMU-tested** (`os/`, `docs/userspace-runtime.md`, `ultrium test programs`, 6 cases): hello, counter, filedemo, fpudemo run in ring 3 concurrently, loaded from `/bin` through the VFS |
| Shell process commands | `feature/process-shell` | **implemented, QEMU-tested** (USH 0.6: `run [-b]`, `jobs`, `terminate`, `ultrium system processes`; `ultrium test processshell` 5 cases + `--process-regression` through real PS/2 events incl. Ctrl+C). `docs/ultrium-shell.md` |
| Integration tests | `feature/process-integration` | **implemented, QEMU-tested** (`ultrium test integration`, 4 cases; `tools/qemu-process-test.py`): kernel tasks + eight user programs together incl. a fault and a kill inside a syscall, process/task exhaustion, userspace file I/O on UltriumFS 2 (memory-backed and virtio), shutdown with live processes, abrupt stop during userspace file writes, host-verified |

## Ring 3 facts verified by test

- Faults from ring 3 that end only the process: #GP (cli, hlt, in, `mov cr3`, `int 0x21`, `int3` because the breakpoint gate is DPL 0, non-canonical access), #UD (`ud2`, `syscall`), #DE, #PF for the kernel image, the direct map, null, another process's page, a write to a code page (W^X) and execution of data or stack (NX).
- NMI, double fault, machine check and any exception taken in ring 0 remain fatal.
- The bootloader's top-level entries for the kernel half carry the User bit; each address space clears it in its own copy. Without that, ring 3 would not fault on kernel reads.
- SMEP and SMAP are enabled whenever the CPU offers them. QEMU's default CPU model offers neither, so CI also runs every user-mode suite with `ULTRIUM_QEMU_CPU=max` (TCG exposes both): all pass with SMEP and SMAP on, which also confirms that the kernel never touches user memory except through the software-walk copies. Not covered: real hardware, `SYSCALL`/`SYSRET` (unsupported, `EFER.SCE` cleared), I/O-port permission bitmaps (the TSS has none, so `in`/`out` fault).
- Kernel stack depth: a user task has a 16 KiB kernel stack with a canary at the bottom (no hardware guard page). Writing to UltriumFS through system calls was measured at about 2 KiB (`integration` asserts it stays below half the stack). The canary catches overflows only if they hit it; deep paths not exercised by the tests (for example extent-map promotion from a syscall) are not measured.
- `ultrium_exception_common`, the IRQ stub and the syscall stub all execute `cld`: ring 3 can set the direction flag.
