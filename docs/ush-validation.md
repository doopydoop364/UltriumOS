# USH validation record

Baseline: main `2cd116e`, kernel 0.4.0. Clean fresh clone; older open draft PRs #2–13 target earlier CLI/branding branches and were not changed.

Commands for every implemented kernel milestone:

```sh
make -B kernel TOOLCHAIN=llvm
make TOOLCHAIN=llvm
python3 tools/qemu-memory-test.py 'ultrium test all'
python3 tools/qemu-memory-test.py 'ultrium test filesystem'
python3 tools/qemu-memory-test.py --keyboard-regression
```

| Revision | Rebuild + ISO | Full suite (twice) | Filesystem (twice) | Keyboard |
|---|---|---|---|---|
| baseline | pass | 20/0 each | 4/0 each | 6 summaries, 1/0 each |
| USH 0.1 | pass, no compiler warnings | 22/0 each | 4/0 each | 6 summaries, 1/0 each |
| USH 0.2 | pass, no compiler warnings | 23/0 each | 4/0 each | 6 summaries, 1/0 each |
| USH 0.3 | pass, no compiler warnings | 28/0 each | 8/0 each | 6 summaries, 1/0 each |

| USH 0.4 | pass, no compiler warnings | 31/0 each | 8/0 each | original 6×1/0; USH 2×1/0 and IRQ 2×2/0 |

| USH 0.5 | pass, no compiler warnings | 31/0 each | 8/0 each | original 6×1/0; USH 2×1/0 and IRQ 2×2/0 |

Negative control for USH 0.3: deliberately flipped a copied byte in RAMFS, rebuilt the ISO and ran the filesystem harness. It reported `FAIL filesystem copy/rename rollback: copied bytes differ` and `passed=7 failed=1`; the Python runner exited 1. Restored the source, forced a full rebuild, and reran the complete required matrix above.

These are local QEMU results, not hosted Actions results. No PR is merged by this work.

USH 0.4 adds `python3 tools/qemu-memory-test.py --shell-regression` to local validation and Actions. A second negative control disabled keyboard Tab translation: the new keyboard session failed its output contract and the harness exited 1. Restored Tab, rebuilt, and reran all required commands plus the new regression successfully. QEMU framebuffer inspection verified the candidate list and restored input/cursor, plus preserved PULSE boot/color/font presentation at 1280×800. Other framebuffer geometries and physical hardware remain manual checks.

Hosted Actions confirmed passing `llvm-qemu` for [USH 0.1](https://github.com/doopydoop364/UltriumOS/actions/runs/38795925751), [USH 0.2](https://github.com/doopydoop364/UltriumOS/actions/runs/38796077404), and [USH 0.3](https://github.com/doopydoop364/UltriumOS/actions/runs/38796781535). Later milestones require their own hosted checks.

The final USH 0.5 revision also passed `python3 tools/qemu-memory-test.py 'ultrium system memory test'`: two summaries of 1 passed/0 failed. USH 0.5 adds the authorization specification and unavailable/planned help text; no privilege-changing command exists. Runtime command behavior remains the tested 0.4 implementation.
