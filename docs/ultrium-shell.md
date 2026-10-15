# Ultrium Shell (USH 0.5)

**Simple actions should be short. Complex operations should be structured. Everything should be discoverable.**

USH is Ultrium's native, freestanding kernel shell. The kernel remains 0.4.0; the shell has its own component version (`ultrium shell version`). It works without graphics applications, networking or AI. PULSE's black console, warm-white text, signal-red accents, VGA 8×16 font and boot presentation remain unchanged.

## Native command reference

| Command | Behavior |
|---|---|
| `list [path]` | List a folder; defaults to cwd |
| `go path` | Change current folder |
| `where` | Print cwd |
| `new file path` | Create an empty file; refuse existing items |
| `new folder path` | Create one folder; parent must exist |
| `view path` | Display file contents |
| `write path text...` | Create a new file with text; refuse existing items |
| `remove path [-r] [-y]` | Remove an item under the safety contract below |
| `copy source destination` | Copy one regular file |
| `move source destination` | Relocate a file or folder |
| `rename source destination` | Rename/relocate to an exact destination path |
| `info path` | Show canonical path, stored type and file byte length |

`write` joins its text arguments with one space; quote a single text argument to preserve internal spacing. `new file` then `write` to that same path is an error: writing is deliberately create-only. An empty quoted text argument creates an empty file. No ownership, permissions or timestamps are invented.

## Structured namespaces and help

`file new|view|write|copy|move|rename|remove|info` and `folder new|list|go|remove|info` share the everyday handlers. These are discovery groups: shared item operations still accept the actual VFS item types. OS commands retain `ultrium system`, `ultrium test` and `ultrium ai` (AI services unavailable). `clear`, `clear history`, `reboot` and existing memory, mapping, interrupt and timer diagnostics retain their behavior.

Every command and namespace supports `<command path> help`. `help <command path>` is equivalent. A lone namespace displays its help. Parent help lists immediate children. For example:

```text
help
new help
file copy help
folder help
ultrium system memory help
ultrium shell compatibility help
```

Routing, help and command completion share one static registry: names, parents, descriptions, usage, handler, argument bounds and alias availability. Unknown command suggestions compare only immediate available children using a bounded edit-distance calculation (maximum distance two, names up to 31 bytes). They never execute a suggested command. Use `./help` for an item literally named `help`, since a single trailing `help` is reserved for command discovery.

## Optional compatibility

Compatibility is **disabled at boot**. `ultrium shell compatibility status|enable|disable` controls selected aliases for this session only. Nothing persists across reboot.

| Alias | Shared native operation |
|---|---|
| `ls` | `list` |
| `cd` | `go` |
| `pwd` | `where` |
| `mkdir` | `new folder` |
| `touch` | `new file` |
| `cat` | `view` |
| `rm` | `remove` |
| `cp` | `copy` |
| `mv` | `move` |

Disabled aliases explain their native replacement. Aliases support the same help and argument rules as native commands. This is not POSIX or Bash compatibility: `touch` is create-only, and no Linux options are implied.

## Quoted argument grammar

Input is bounded to 70 printable ASCII characters and 16 arguments. Command words are case-insensitive; paths and text retain their exact case. Unquoted spaces separate arguments; spaces inside single or double quotes are literal. Adjacent quoted and unquoted segments form one argument. Backslash quotes the next printable character both inside and outside quotes, including quote characters and backslash itself. There are no variable expansions, wildcard expansions, comment syntax, command substitutions, pipelines or multiline continuations.

Unterminated quotes and trailing escapes reject the whole command before execution. Empty quoted strings are real empty arguments; empty paths are invalid, while empty write text is allowed. Parser limits produce errors. If interactive typing exceeds 70 characters, Enter rejects the entire input; Ctrl+U/C or replacing it with history clears the overflow. Completion that would exceed the limit preserves the original input and reports the limit.

```text
new folder "My Projects"
go "My Projects"
write "project notes.txt" "Hello Ultrium"
view "project notes.txt"
write quotes "A quote: \"yes\""
write empty ""
```

## Removal safety contract

| Input | Behavior |
|---|---|
| `remove file.txt` | Remove a file |
| `remove empty-folder` | Remove an empty folder |
| `remove nonempty-folder` | Refuse |
| `remove folder -r` / `--recursive` | Ask before recursive deletion |
| `remove folder -y` / `--yes` | Still nonrecursive |
| `remove folder -r -y` | Recurse without confirmation |

Options may precede or follow the single path. Unknown/combined options and extra paths are rejected. Use `./-name` for a name beginning with a dash. Only `y` or `yes` (case-insensitive, no extra whitespace) confirms. Empty input, any other answer, or Ctrl+C cancels. Confirmation reuses normal line editing; answers never enter command history. `-y` never enables recursion or grants permissions.

Root, cwd and every cwd ancestor are protected. Actions check every traversed intermediate directory, including components followed by `..`: `missing/../victim` and `file/../victim` never redirect deletion to `victim`. Low-level VFS calls require canonical absolute paths. `vfs_normalize` remains a lexical utility; shell actions use `vfs_resolve_path`.

Recursive removal uses one path buffer and an iterative postorder walk bounded to 3×128 steps. Iterators close before mutation. It prints each successfully removed path, then reports the number removed, the remaining subtree and blocked path on failure. Busy files can cause partial deletion; there are no transactions or atomic removal promises. One root mount exists, with no symbolic links or permissions. New mounts/links require a traversal policy before implementation: never cross mount boundaries or follow links implicitly. Future permission checks belong in the VFS/backend; no shell flag may bypass them.

## Copy, move and rename

Transfers use exact destination paths. They refuse existing destinations (including the source itself), missing parents and incompatible types; they never implicitly append a basename to a destination folder. Directory copy is unsupported. Move and rename share one allocation-free RAMFS metadata operation. Directory cycles, moves making descendant paths too long and shell moves of cwd/ancestors are rejected before mutation.

Copy allocates an unlinked node and complete data buffer before committing. Exhausted node/data budgets or allocation failures leave no destination and preserve the source. Failed create-only writes clean up their new destination. Rename within this single-CPU RAMFS is atomic at the namespace mutation boundary; there are no cross-filesystem operations or cross-filesystem atomicity promises.

## Completion and editing

Tab completes command names, immediate namespace children, files and folders. Paths are case-sensitive; `go` and `list` complete directories. Unique matches complete immediately; directories end in `/` and other unique matches get a separating space when appropriate. Ambiguous matches extend their longest common prefix; a second consecutive Tab lists candidates in registry or RAMFS insertion order and restores the input. Quoted paths are safely re-encoded; completion within a token preserves later arguments. A horizontal viewport keeps the caret visible on short rows or after long confirmation prompts.

Left/Right, Home/End, Backspace/Delete, Up/Down history and Ctrl+C/U/W/L remain available. Tab does not modify confirmation answers. History is session-only and stores at most 4096 commands; persistence and scripting remain future work.

## Examples

```text
new folder projects
write projects/notes.txt "Hello Ultrium"
file copy projects/notes.txt projects/backup.txt
rename projects/backup.txt projects/archive.txt
folder list projects
info projects/archive.txt
remove projects -r
```

## Limits and future work

RAMFS is volatile: names 31 bytes, paths 127 bytes, files 4096 bytes, total data-buffer capacity 65536 bytes, 128 nodes including root, 64 file handles and 64 directory iterators. There is no persistence, symbolic link, multi-mount dispatch, process loader, isolated userspace, enforced multiuser permission system, scripting, user aliases or plugin execution. Filesystem and completion operations run in single-CPU shell context, never in IRQ handlers.

The registry and shared VFS APIs provide extension points for future discovery, applications, completion metadata and structured execution. Capability requirements, structured results and scripting need explicit designs when implemented; existing string handlers do not claim those features.

Milestones: USH 0.1 native commands/compatibility; 0.2 registry/help; 0.3 safe operations; 0.4 quoting/completion; 0.5 authorization architecture. See [validation](ush-validation.md) and [kernel testing](kernel-testing.md).

## USH 0.5 authorization design

`auth` requests temporary action-specific authorization; `as` executes one command under another authorized identity. **Both are planned and unavailable.** No identity-changing command or password prompt exists. The kernel lacks the necessary userspace/process/security boundaries. USH 0.5 denotes the completed architecture milestone, not implemented privilege isolation. See [authorization architecture](authorization-architecture.md) for capability scopes, trusted human approval, agent restrictions, auditing, revocation and kernel prerequisites.
