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

The current prototype only renders text into the Limine framebuffer. The displayed
`ULTRIUM>` prompt is static, **not** a working command interpreter.

Near-term steps:
1. Terminal subsystem with scroll/wrap/color (this branch).
2. Font assets/interface; JetBrains Mono preferred, subject to suitable licensed bitmap conversion.
3. Keyboard input, line editing, and command dispatch.
4. A minimal `ultrium` command with `version` and `system info`.
5. An `ultrium ai status` command that clearly reports unavailable until an agent is actually present.
6. A simple, versioned, manually editable desktop configuration and framebuffer renderer.
7. A host-assisted AI generator producing validated desktop configurations, without executing models in kernel mode.

Prefer an explicitly selected CLI boot mode over booting a desktop by default.
