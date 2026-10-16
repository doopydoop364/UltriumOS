#!/usr/bin/env python3
"""Multi-boot persistence test: data written in one boot must be read back in the next.

Default: UltriumFS 2. `--v1` runs the compatibility test: a UltriumFS 1 volume must mount
read-only through the shell and the disk image must stay byte-identical.

Every boot is an independent QEMU process using the same disposable image under
test-images/. Between boots the image file is decoded on the host by an independent
reader (ufs_image.py), so the evidence is the disk file, not kernel memory.

  Boot 1  refuse to mount a blank disk, decline then confirm an explicit format,
          mount, create folders and files through the shell, copy/rename/remove,
          write multi-block files, verify, sync, `shutdown`
  Boot 2  mount WITHOUT formatting, verify boot-one data, update/grow/remove, sync, `shutdown`
  Boot 3  mount, verify boot-two changes, unmount, `shutdown`
  Boot 4  mount, write a file, then kill QEMU (an abrupt stop, NOT a guest shutdown)
  Boot 5  mount, require an unclean-session report and the data written before the stop
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ultrium_qemu as U
import ufs2_image as I
import hashlib

IMAGE = "persistence.img"


def expected_boot1():
    return {
        "/projects": None, "/projects/ultrium": None,
        "/projects/notes.txt": b"Persistent data",
        "/projects/ultrium/readme": b"Boot one",
        "/projects/ultrium/odd.bin": I.pattern(5000, 13),
        "/empty.txt": b"", "/old-notes.txt": b"Persistent data",
        "/big.bin": I.pattern(20000, 11), "/medium.bin": I.pattern(4096, 12),
    }


def expected_boot2():
    t = expected_boot1()
    t["/projects/notes.txt"] = b"PERSISTENT data"
    del t["/old-notes.txt"]
    t["/boot2.txt"] = b"Second boot"
    t["/big.bin"] = I.pattern(30000, 21)
    return t


def host_check(path, expected, state, label):
    img = I.Image(path)
    if img.pending_transactions():
        raise RuntimeError("%s: %d committed journal transaction(s) left unapplied" % (label, img.pending_transactions()))
    tree = img.tree()
    if tree != expected:
        missing = sorted(set(expected) - set(tree)); extra = sorted(set(tree) - set(expected))
        bad = sorted(k for k in expected if k in tree and tree[k] != expected[k])
        raise RuntimeError("%s: host-decoded image differs (missing %s, extra %s, wrong %s)" % (label, missing, extra, bad))
    problems = img.findings()
    if problems:
        raise RuntimeError("%s: the independent checker found problems: %s" % (label, problems[:5]))
    if img.state() != state:
        raise RuntimeError("%s: superblock state %d, expected %d" % (label, img.state(), state))
    print("  host check (%s): %d items, content identical, independent consistency check clean, journal empty, state %s" %
          (label, len(tree), "clean" if state == 0 else "mounted (abrupt stop)"), flush=True)


def mount(vm):
    vm.line("new folder /volumes")
    vm.line("new folder /volumes/disk")
    vm.line("ultrium storage mount vda /volumes/disk")


def barrier(vm):
    vm.run_test("ultrium test interrupts", 2)   # serial checkpoint: input queue drained


def v1_compat():
    path = U.make_image("persistence-v1.img", 64)
    print("v1 boot 1: build an UltriumFS 1 volume (explicit v1 format), shutdown", flush=True)
    vm = U.VM(image=path)
    try:
        vm.run_test("ultrium test persist v1fixture", 1)
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    def digest():
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    before = digest()
    with open(path, "rb") as f:
        if f.read(8) != b"ULTRIUMF":
            raise RuntimeError("the fixture is not an UltriumFS 1 image")
    print("v1 boot 2: mount through the shell (must be read-only), read, refuse writes, unmount", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        barrier(vm)
        vm.run_test("ultrium test persist v1verify", 1)
        vm.line("ultrium storage check vda")        # the v2 checker must refuse a v1 volume without touching it
        vm.line("ultrium storage unmount /volumes/disk")
        barrier(vm)
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    if digest() != before:
        raise RuntimeError("the UltriumFS 1 image changed after a read-only mount")
    print("PASS v1 compatibility: mounted read-only, data readable, writes refused, image byte-identical")
    return 0


def main():
    if "--v1" in sys.argv:
        return v1_compat()
    path = U.make_image(IMAGE, 64)
    print("boot 1: format, populate, shutdown", flush=True)
    vm = U.VM(image=path)
    try:
        vm.run_test("ultrium test persist blank", 1)
        vm.line("ultrium storage format vda")
        vm.line("n")                                    # decline the confirmation
        barrier(vm)
        vm.run_test("ultrium test persist blank", 1)    # still blank: nothing was formatted
        vm.line("new folder /volumes")
        vm.line("new folder /volumes/disk")
        vm.line("ultrium storage format vda")
        vm.line("yes")                                  # explicit confirmation
        barrier(vm)
        vm.line("ultrium storage mount vda /volumes/disk")
        for text in ["new folder /volumes/disk/projects",
                     "new folder /volumes/disk/projects/ultrium",
                     'write /volumes/disk/projects/notes.txt "Persistent data"',
                     "new file /volumes/disk/empty.txt",
                     'write /volumes/disk/projects/ultrium/readme "Boot one"',
                     "copy /volumes/disk/projects/notes.txt /volumes/disk/backup.txt",
                     "rename /volumes/disk/backup.txt /volumes/disk/old-notes.txt",
                     'write /volumes/disk/tmp.txt "to be removed"',
                     "remove /volumes/disk/tmp.txt",
                     "new folder /volumes/disk/trash",
                     "new file /volumes/disk/trash/a",
                     "remove /volumes/disk/trash -r -y"]:
            vm.line(text)
        barrier(vm)
        vm.run_test("ultrium test persist populate", 1)
        vm.run_test("ultrium test persist stage1", 4)
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, expected_boot1(), 0, "after boot 1")

    print("boot 2: mount without format, verify, modify, shutdown", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        barrier(vm)
        vm.run_test("ultrium test persist clean", 1)
        vm.run_test("ultrium test persist stage1", 4)
        vm.line('write /volumes/disk/boot2.txt "Second boot"')
        vm.run_test("ultrium test persist modify", 1)
        vm.run_test("ultrium test persist stage3", 2)
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, expected_boot2(), 0, "after boot 2")

    print("boot 3: verify boot-two changes, unmount, shutdown", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        barrier(vm)
        vm.run_test("ultrium test persist clean", 1)
        vm.run_test("ultrium test persist stage3", 2)
        vm.line("ultrium storage unmount /volumes/disk")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, expected_boot2(), 0, "after boot 3")

    print("boot 4: write, then abrupt stop (killed, NOT a guest shutdown)", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        vm.line('write /volumes/disk/crash.txt "written before the stop"')
        barrier(vm)
    finally:
        vm.close()                       # terminates QEMU: this is explicitly not a shutdown
    crashed = expected_boot2()
    crashed["/crash.txt"] = b"written before the stop"
    host_check(path, crashed, 1, "after abrupt stop")

    print("boot 5: mount after the abrupt stop", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        barrier(vm)
        vm.run_test("ultrium test persist unclean", 2)
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, crashed, 0, "after boot 5")
    print("PASS persistence: UltriumFS 2 data survived 3 clean shutdown/boot cycles and an abrupt stop")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, I.FormatError) as error:
        print("QEMU persistence test failed:", error, file=sys.stderr)
        sys.exit(1)
