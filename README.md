# Ultrium OS

Ultrium OS is an experimental, from-scratch hobby operating system.

The x86-64 kernel (0.5.0) boots through Limine and provides a PULSE framebuffer terminal, interrupt-driven keyboard, memory manager, ACPI shutdown, a virtio block driver and UltriumFS (persistent storage on a scratch virtual disk, no journal) alongside the volatile RAMFS. Ultrium Shell (USH 0.5) adds native commands, hierarchical help, safe file operations, quoting and completion. The 0.5 authorization milestone is design only; `auth` and `as` remain unavailable.

See [Ultrium Shell](docs/ultrium-shell.md) for commands, [kernel testing](docs/kernel-testing.md) for LLVM/QEMU validation, and [the development guide](AGENTS.md) for contribution rules.

Development is experimental; no releases are ready for installation on hardware.
