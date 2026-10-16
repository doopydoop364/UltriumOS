#!/usr/bin/env python3
"""Multi-boot persistence test: data written in one boot must be read back in the next.

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
import ufs_image as I

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
    tree = img.tree()
    if tree != expected:
        missing = sorted(set(expected) - set(tree)); extra = sorted(set(tree) - set(expected))
        bad = sorted(k for k in expected if k in tree and tree[k] != expected[k])
        raise RuntimeError("%s: host-decoded image differs (missing %s, extra %s, wrong %s)" % (label, missing, extra, bad))
    if img.state != state:
        raise RuntimeError("%s: superblock state %d, expected %d" % (label, img.state, state))
    print("  host check (%s): %d items, content identical, superblock state %s" %
          (label, len(tree), "clean" if state == 0 else "modified (unclean)"), flush=True)


def mount(vm):
    vm.line("new folder /volumes")
    vm.line("new folder /volumes/disk")
    vm.line("ultrium storage mount vda /volumes/disk")


def barrier(vm):
    vm.run_test("ultrium test interrupts", 2)   # serial checkpoint: input queue drained


def main():
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
    print("PASS persistence: data survived 3 clean shutdown/boot cycles and an abrupt stop")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, I.FormatError) as error:
        print("QEMU persistence test failed:", error, file=sys.stderr)
        sys.exit(1)
