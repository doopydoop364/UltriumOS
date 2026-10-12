# Ultrium CLI command namespace (v0.0.5)

Ultrium is CLI-first; graphical desktops and AI remain optional.

## Routing
- ultrium <command>: built-in OS commands.
- ultrium ai <command>: explicit AI features; only status is currently implemented.
- Unprefixed names: reserved for future external installed programs.
- ultrium alone displays help.
- Available built-ins: ultrium help, ultrium clear, ultrium version, ultrium system info, ultrium ai status.
- Old unprefixed help and clear are intentionally no longer built-ins.

The command registry currently resides in kernel/src/shell.c. This is a kernel-resident prototype, not userspace.

## Current limitations
Keyboard input uses polling via emulated PS/2 set-1 scan codes with a US layout. Caps Lock and left/right Shift are supported. There is no history, arrow-key editing, interrupt handling, external process execution, or AI runtime yet.

Future executable lookup may use PATH-like conventions and system APIs for OS commands.