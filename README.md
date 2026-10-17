# Ultrium OS

Ultrium OS is an experimental, from-scratch hobby operating system.

The x86-64 kernel (0.7.0) boots through Limine and provides a PULSE framebuffer terminal, interrupt-driven keyboard, memory manager, ACPI shutdown, a virtio block driver, the volatile RAMFS and UltriumFS 2 (journaled persistent storage on a scratch virtual disk). Since 0.7.0 it also has a round-robin scheduler, per-process address spaces, ring-3 execution, a versioned system-call ABI, an ELF64 loader and sample userspace programs that run concurrently and use the VFS through system calls (the shell itself still runs inside the kernel). Ultrium Shell (USH 0.6) adds native commands, hierarchical help, safe file operations, quoting, completion and `run`/`jobs`/`terminate`. This is an experimental single-CPU system: no SMP, no file permissions, no network. `auth` and `as` remain unavailable.

See [Ultrium Shell](docs/ultrium-shell.md) for commands, [kernel testing](docs/kernel-testing.md) for LLVM/QEMU validation, and [the development guide](AGENTS.md) for contribution rules.

Development is experimental; no releases are ready for installation on hardware.
