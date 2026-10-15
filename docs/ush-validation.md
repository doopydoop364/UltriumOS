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

Negative control for USH 0.3: deliberately flipped a copied byte in RAMFS, rebuilt the ISO and ran the filesystem harness. It reported `FAIL filesystem copy/rename rollback: copied bytes differ` and `passed=7 failed=1`; the Python runner exited 1. Restored the source, forced a full rebuild, and reran the complete required matrix above.

These are local QEMU results, not hosted Actions results. No PR is merged by this work.
