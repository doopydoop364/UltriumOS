# Ultrium CLI command categories

The shell has three command categories:

1. **General shell commands** have no prefix: `help`, `clear`, `clear history`, `reboot` and native filesystem verbs (`list`, `go`, `where`, `new file/folder`, `view`, `write`, `remove`, `copy`, `move`, `rename`, `info`). These are implemented built-ins; structured `file` and `folder` namespaces share their handlers. Linux-style aliases are optional and disabled at boot. See [Ultrium Shell](ultrium-shell.md).
2. **Built-in OS management commands** use `ultrium <subcommand>`, such as `ultrium version`, `ultrium system info`, and `ultrium help`. AI commands use the explicit namespace `ultrium ai <subcommand>`; `ultrium ai status` currently reports that no AI services are installed.
3. **Installed applications** use their own executable names, for example `neofetch`, `git status`, or `python script.py`. These are examples for the future; Ultrium currently has no executable loader, filesystem-backed program lookup, or external processes. Such inputs report that external execution is unavailable.

A shell prompt (currently `ultrium>`) is not a prefix users must type; it is just a prompt. This design keeps the CLI usable without a desktop or AI.

The `ultrium test` namespace runs kernel diagnostics; see `kernel-testing.md`. Later external programs will require process loading, command arguments, permissions, and executable discovery. Naming collisions will be resolved by prioritizing shell built-ins and reserving the `ultrium` namespace.
