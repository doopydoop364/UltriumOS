# Ultrium Shell (USH 0.2)

Simple actions should be short. Complex operations should be structured. Everything should be discoverable.

USH is the native, freestanding kernel shell. Its component version is separate from kernel 0.4.0. PULSE colors, VGA 8×16 glyphs, boot presentation, history and line editing remain in place.

Native commands: `list [path]`, `go path`, `where`, `new file path`, `new folder path`, `view path`, `write path text`, `remove path`, `info path`. `write` creates a new file and refuses existing destinations. `remove` only deletes files or empty folders; root and the current directory and its ancestors are protected. `info` reports stored type and file byte length, never invented timestamps, owners or permissions. RAMFS is volatile.

Use `<command> help`. OS diagnostics remain under `ultrium system` and `ultrium test`; `ultrium test shell` exercises native commands and compatibility.

`ultrium shell version` reports the component version. `ultrium shell compatibility status|enable|disable` controls session-only aliases, disabled at boot: `ls`, `cd`, `pwd`, `mkdir`, `touch`, `cat`, `rm`. They invoke the native operations. There is no POSIX/Bash compatibility; `touch` uses create-only semantics. Disabled aliases explain their native replacement. No setting persists across reboot.

Example: `new folder projects`, `write projects/notes Hello Ultrium`, `list projects`, `view projects/notes`, `info projects/notes`, `remove projects/notes`, `remove projects`.

Limits: input 70 characters, 16 arguments, names 31 bytes, paths 127 bytes, files 4096 bytes, 128 nodes. Quoting, structured file/folder namespaces, copy/move/rename, recursive deletion and completion arrive in subsequent USH milestones. See [RAMFS](ramfs.md) for storage limits.

## Registry and structured commands

Routing and hierarchical help share static registry metadata: name, parent namespace, description, usage, handler, argument bounds and alias availability. `help path` and `path help` are equivalent. Parent menus show immediate children. All existing `ultrium system` diagnostics and `ultrium test` suites are registered.

`file new|view|write|remove|info` and `folder new|list|go|remove|info` use the same handlers as everyday commands. `file`/`folder` describe discovery groups; shared item operations still report actual VFS types. Copy, move and rename are added in USH 0.3.
