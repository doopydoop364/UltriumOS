# Ultrium Shell (USH 0.3)

Simple actions should be short. Complex operations should be structured. Everything should be discoverable.

USH is the native, freestanding kernel shell. Its component version is separate from kernel 0.4.0. PULSE colors, VGA 8×16 glyphs, boot presentation, history and line editing remain in place.

Native commands: `list [path]`, `go path`, `where`, `new file path`, `new folder path`, `view path`, `write path text`, `remove path`, `info path`. `write` creates a new file and refuses existing destinations. `remove` only deletes files or empty folders; root and the current directory and its ancestors are protected. `info` reports stored type and file byte length, never invented timestamps, owners or permissions. RAMFS is volatile.

Use `<command> help`. OS diagnostics remain under `ultrium system` and `ultrium test`; `ultrium test shell` exercises native commands and compatibility.

`ultrium shell version` reports the component version. `ultrium shell compatibility status|enable|disable` controls session-only aliases, disabled at boot: `ls`, `cd`, `pwd`, `mkdir`, `touch`, `cat`, `rm`. They invoke the native operations. There is no POSIX/Bash compatibility; `touch` uses create-only semantics. Disabled aliases explain their native replacement. No setting persists across reboot.

Example: `new folder projects`, `write projects/notes Hello Ultrium`, `list projects`, `view projects/notes`, `info projects/notes`, `remove projects/notes`, `remove projects`.

Limits: input 70 characters, 16 arguments, names 31 bytes, paths 127 bytes, files 4096 bytes, 128 nodes. Quoting and completion arrive in USH 0.4. See [RAMFS](ramfs.md) for storage limits.

## Registry and structured commands

Routing and hierarchical help share static registry metadata: name, parent namespace, description, usage, handler, argument bounds and alias availability. `help path` and `path help` are equivalent. Parent menus show immediate children. All existing `ultrium system` diagnostics and `ultrium test` suites are registered.

`file new|view|write|remove|info` and `folder new|list|go|remove|info` use the same handlers as everyday commands. `file`/`folder` describe discovery groups; shared item operations still report actual VFS types. Copy, move and rename are available in USH 0.3.

## USH 0.3 safe operations

`copy source destination`, `move source destination`, and `rename source destination` use exact destination paths. Their `file` namespace equivalents share handlers. `cp` and `mv` join the optional aliases. Copy accepts regular files only. All transfers reject existing destinations (including the source itself), missing parents and incompatible types; they never implicitly append a basename to a destination folder. Move and rename both use one allocation-free RAMFS metadata operation. Directory cycles and moves that would exceed descendant path limits are rejected before mutation. The shell also refuses moving cwd or its ancestors. No cross-filesystem transfer is supported.

Copy allocates an unlinked node and complete data buffer before committing it. Exhausted node/data budgets or allocation failure leave no destination and preserve the source. Create-only write cleans up its new destination if writing fails. Metadata remains limited to type and file byte length.

| Removal input | Behavior |
|---|---|
| `remove path` | File or empty folder only |
| `remove path -y` / `--yes` | Still nonrecursive |
| `remove folder -r` / `--recursive` | Confirm before recursively deleting |
| `remove folder -r -y` | Recurse without confirmation |

Options may precede or follow the single path; unknown/combined options are rejected. Use `./-name` for a name beginning with a dash. Only `y` or `yes` (case-insensitive, no extra whitespace) confirms. Empty input, any other answer, or Ctrl+C cancels. The existing line editor handles confirmation answers; they do not enter command history. `-y` never enables recursion or grants permissions.

Root, cwd and every cwd ancestor are protected. Path traversal checks every existing intermediate directory, including those followed by `..`; `missing/../victim` never resolves to `victim`. The backend accepts only canonical absolute paths, so callers cannot create literal dot entries. Lexical `vfs_normalize` remains available to kernel callers but is not the shell's action resolver.

Tree removal walks iteratively with one bounded path buffer and at most 3×128 steps, closes iterators before mutation, and stops on the first failure. It reports the number already removed, the remaining subtree and the blocked path; there are no transactions or rollback promises. Busy open files can cause partial deletion. There is one mount and no symbolic links or permissions today. Introducing mounts or links requires a new traversal policy first: do not cross mount boundaries or follow links implicitly. Future permissions must be enforced by VFS/backend operations, with no shell flag bypass.
