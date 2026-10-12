# Console font provenance

The early Ultrium framebuffer terminal uses a VGA 8x16 bitmap glyph set for ASCII 0x20–0x7E.

Source: [Linux kernel `lib/fonts/font_8x16.c`](https://github.com/torvalds/linux/blob/master/lib/fonts/font_8x16.c), licensed GPL-2.0. The extracted glyph data in `kernel/src/terminal/vga8x16.h` is marked `SPDX-License-Identifier: GPL-2.0`.

**Distribution consideration:** This inclusion introduces GPL-2.0-licensed material into the kernel image. Before publishing binaries or selecting a final project license, evaluate and comply with the GPL-2.0 obligations for the combined work. Consider replacing the font with a suitable permissively licensed, separately sourced VGA-compatible font if a permissively licensed kernel is preferred.

The terminal retains a black framebuffer background and mint accent for Ultrium. The font renderer supports printable ASCII only; extended code pages and Unicode are separate tasks.
