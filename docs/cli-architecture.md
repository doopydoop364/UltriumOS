# Ultrium CLI and AI command design (draft)

## Product rules

- A CLI-only installation is a first-class, permanent mode of Ultrium OS.
- The graphical desktop is optional and may be started and stopped independently when those services exist.
- Disabling AI must not prevent normal OS administration, shell use, or desktop use.
- Ordinary commands must not implicitly send prompts or system data to cloud models.
- AI commands must operate through user-approved, permission-checked capabilities (once implemented).
- CLI and GUI should ultimately share public system APIs.

## Proposed command namespace

The shell prompt and the command executable are distinct:

```text
ultrium> ultrium version
ultrium> ultrium system info
ultrium> ultrium desktop start
ultrium> ultrium ai status
ultrium> ultrium ai "Create a dark floating desktop"
```

`ultrium <subcommand>` controls ordinary system features.
`ultrium ai <subcommand-or-request>` explicitly accesses AI functionality.
The exact CLI grammar remains subject to design/testing.

## Implementation status

The kernel has an interactive framebuffer shell with IRQ-backed PS/2 input, cursor editing, history, a static command registry, hierarchical help and a volatile RAMFS. Native USH commands, quoted arguments and Tab completion are documented in [Ultrium Shell](ultrium-shell.md). The kernel and shell have separate versions. Existing memory/interrupt tests and OS administration stay under `ultrium system` and `ultrium test`.

External application loading, userspace processes, desktop services, persistent configuration and AI execution remain future work. Current AI status reports unavailable. Prefer an explicitly selected CLI boot mode over booting a desktop by default.
