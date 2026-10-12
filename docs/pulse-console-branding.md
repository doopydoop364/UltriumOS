# PULSE console branding (initial integration)

Source of truth: Ultrium PULSE Brand Kit v0.1 (2026-10-12), `ULTRIUM_PULSE_BRAND_GUIDE.md`. PULSE is the accepted visual direction, not a rename of Ultrium OS.

## Current boot console
- Background: #000000, intentionally classic TTY black. Graphite #121212 remains a **brand surface** token for future interfaces, not the framebuffer console background.
- Foreground: PULSE warm white #F3F1EC.
- Accent: proposed PULSE signal red #FF5A36. Applied only to the word ULTRIUM, CLI MODE, and the prompt/cursor; final contrast and accessibility testing remains pending.
- Muted copy: PULSE gray #8A8F98.
- Font: retain VGA 8x16 kernel console bitmap (see console-font.md for license information), not the Sora/Manrope/IBM Plex Mono marketing typography.
- The proposed tagline appears as muted boot copy: "BUILT TO EXPERIMENT. READY TO EVOLVE."

## Deliberate nonchanges
No fake agent indicators, premature graphical animations, or kernel-integrated SVG renderer. Do not use the Signal U SVG as a bitmap substitute before a proper boot splash/graphics asset pipeline exists. User desktops must not be forced to follow PULSE.

## Next steps
Store approved visual assets in a separate branding directory with their license/provenance, test legibility on varied display sizes, and build a dedicated optional graphical startup flow rather than overloading the early text console.
