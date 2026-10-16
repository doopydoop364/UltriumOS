#!/usr/bin/env python3
"""UltriumFS 2 recovery test on a real (virtio) disk, checked by an independent host-side reader.

What each part proves, and what it does NOT:
  * Deterministic power cuts: the kernel arms a cut at a named journal phase (block_arm_power_cut).
    From that point every transfer and flush to the virtio disk fails, so the image file holds exactly
    the writes that completed before the cut. This models a device that lost power at a phase boundary
    with all earlier writes durable. It does NOT model a volatile cache dropping or reordering writes
    (that is the in-kernel recovery matrix, `ultrium test ufs2crash`) and it is NOT physical power loss.
  * Mid-stream QEMU kill: QEMU is stopped with SIGTERM/SIGKILL while a 40 MiB write is running. Writes the
    guest already completed are in the host's page cache and survive; the host did not lose power. This is
    an abrupt termination of the emulator, not a power-loss test.
After every interruption the host decodes the image WITHOUT the kernel (tools/ufs2_image.py, virtually
applying committed journal transactions), requires a consistent state that equals the expected old or new
tree, then boots the kernel, lets it replay, shuts down cleanly, and requires the host-predicted tree.

  boot 1   format, build a fixture (file > 4 MiB, fragmented file, nested folders, renames, block reuse)
  per cut  copy the fixture image, boot, cut power in the middle of one operation, kill QEMU, host check,
           boot (replay), shut down, host check
  stream   same with a QEMU kill during a 40 MiB write
"""
import os, shutil, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ultrium_qemu as U
import ufs2_image as I

LARGE, FRAG, REUSE = 6 * 1024 * 1024, 40 * 4096, 163840
STREAM_TOTAL = 40 * 1024 * 1024


def base_tree():
    return {
        "/deep": None, "/deep/a": None, "/deep/b2": None, "/deep/b2/c": None,
        "/deep/a/moved.txt": b"nested",
        "/large.bin": I.pattern(LARGE, 41),
        "/frag1.bin": I.pattern(FRAG, 51),
        "/reuse.bin": I.pattern(REUSE, 53),
    }


def moved_tree():
    t = base_tree()
    t["/deep/frag1.bin"] = t.pop("/frag1.bin")
    return t


def grown_tree():
    t = base_tree()
    t["/reuse.bin"] = I.pattern(REUSE + 20000, 53)
    return t


# phase -> (kernel command, tree after replay, committed transactions visible on the crashed image)
CUTS = [
    ("dataflush", "cut-dataflush", base_tree, 0),     # data written, nothing committed: old state
    ("journal", "cut-journal", base_tree, 0),         # nothing reached the journal: old state
    ("commit", "cut-commit", base_tree, 0),           # payload durable, no commit record: old state, NOT replayed
    ("home", "cut-home", moved_tree, 1),              # committed, home blocks not written: replay gives the new state
    ("checkpoint", "cut-checkpoint", moved_tree, 1),  # home blocks written, checkpoint missing: replay is idempotent
]


def diff(expected, tree):
    missing = sorted(set(expected) - set(tree)); extra = sorted(set(tree) - set(expected))
    bad = sorted(k for k in expected if k in tree and tree[k] != expected[k])
    return "missing %s, extra %s, wrong %s" % (missing, extra, bad)


def host_check(path, expected, state, label, pending=None):
    img = I.Image(path)
    if pending is not None and img.pending_transactions() != pending:
        raise RuntimeError("%s: %d committed journal transaction(s), expected %d" % (label, img.pending_transactions(), pending))
    tree = img.tree()
    if tree != expected:
        raise RuntimeError("%s: host-decoded image differs (%s)" % (label, diff(expected, tree)))
    problems = img.findings()
    if problems:
        raise RuntimeError("%s: the independent checker found problems: %s" % (label, problems[:5]))
    if img.state() != state:
        raise RuntimeError("%s: superblock state %d, expected %d" % (label, img.state(), state))
    print("  host check (%s): %d items identical, consistent, %d committed transaction(s) pending, state %s" %
          (label, len(tree), img.pending_transactions(), "clean" if state == 0 else "mounted (interrupted)"), flush=True)


def mount(vm):
    vm.line("new folder /volumes")
    vm.line("new folder /volumes/disk")
    vm.line("ultrium storage mount vda /volumes/disk")
    vm.run_test("ultrium test interrupts", 2)


def finish(vm):
    vm.line("ultrium storage sync")
    vm.line("ultrium storage unmount /volumes/disk")
    vm.run_test("ultrium test interrupts", 2)
    vm.line("shutdown")
    vm.expect_guest_shutdown()


def copy_image(src, name):
    dst = U.make_image(name, 1)
    shutil.copyfile(src, dst)
    return dst


def main():
    base = U.make_image("recovery-base.img", 128)
    print("boot 1: format, build the fixture, verify, clean shutdown", flush=True)
    vm = U.VM(image=base)
    try:
        vm.line("new folder /volumes")
        vm.line("new folder /volumes/disk")
        vm.line("ultrium storage format vda")
        vm.line("yes")
        vm.run_test("ultrium test interrupts", 2)
        vm.line("ultrium storage mount vda /volumes/disk")
        vm.run_test("ultrium test interrupts", 2)
        vm.run_test("ultrium test persist big", 1, timeout=180)
        vm.run_test("ultrium test persist bigverify", 1, timeout=120)
        finish(vm)
    finally:
        vm.close()
    host_check(base, base_tree(), 0, "fixture", 0)

    for name, command, expected_tree, pending in CUTS:
        print("power cut '%s' on the virtio disk" % name, flush=True)
        path = copy_image(base, "recovery-%s.img" % name)
        vm = U.VM(image=path)
        try:
            mount(vm)
            vm.run_test("ultrium test persist " + command, 1, timeout=120)
        finally:
            vm.close()      # QEMU is stopped; the cut already stopped every write to the image
        old = base_tree()
        expected = expected_tree()
        host_check(path, expected, 1, "after cut", pending)
        vm = U.VM(image=path)
        try:
            mount(vm)
            vm.run_test("ultrium test persist recovered", 1)
            finish(vm)
        finally:
            vm.close()
        host_check(path, expected, 0, "after replay and clean shutdown", 0)
        print("  PASS cut at '%s': %s state, nothing lost or invented" % (name, "old" if expected == old else "new"), flush=True)

    print("abrupt QEMU stop during a 40 MiB write", flush=True)
    path = copy_image(base, "recovery-stream.img")
    vm = U.VM(image=path)
    try:
        mount(vm)
        vm.line("ultrium test persist stream")
        vm.wait_for(r"STREAM 3", timeout=120)
    finally:
        vm.close()
    img = I.Image(path)
    tree = img.tree()
    data = tree.pop("/stream.bin", None)
    if data is None or data != I.pattern(len(data), 61) or len(data) > STREAM_TOTAL:
        raise RuntimeError("stream.bin is not a prefix of the data that was written")
    if tree != base_tree():
        raise RuntimeError("an unrelated file changed during the stream: " + diff(base_tree(), tree))
    problems = img.findings()
    if problems:
        raise RuntimeError("independent checker after the stream kill: %s" % problems[:5])
    size = len(data)
    print("  host check (after kill): stream.bin %d bytes (prefix of the written data), other files identical, consistent" % size, flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        vm.run_test("ultrium test persist recovered", 1)
        vm.run_test("ultrium test persist streamverify", 1, timeout=180)
        finish(vm)
    finally:
        vm.close()
    final = dict(base_tree()); final["/stream.bin"] = I.pattern(size, 61)
    host_check(path, final, 0, "after replay and clean shutdown", 0)
    print("  PASS stream kill: %d of %d bytes survived, all of them valid" % (size, STREAM_TOTAL), flush=True)
    print("PASS ufs2 recovery on a virtio disk: 5 deterministic power cuts and an abrupt QEMU stop, each checked by the independent host reader")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, I.FormatError) as error:
        print("UltriumFS 2 recovery test failed:", error, file=sys.stderr)
        sys.exit(1)
