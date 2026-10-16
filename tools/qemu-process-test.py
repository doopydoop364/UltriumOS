#!/usr/bin/env python3
"""Userspace + persistent filesystem on a real virtio disk, checked by the independent host reader.

  Boot 1  format and mount; a userspace program (filedemo) writes /filedemo.txt through system calls; two
          more programs are started in the background; `shutdown` with them still alive (clean guest shutdown)
  Boot 2  the in-kernel integration scenarios, then mount; userspace programs read the file back and survive
          hostile system calls on the mounted volume; clean shutdown
  Boot 3  a userspace disk writer streams a file; QEMU is stopped abruptly (NOT a power-loss test)
  Boot 4  mount (journal replay, unclean session reported); the file written before the stop is a valid prefix;
          clean shutdown
Between boots the image is decoded on the host without the kernel (tools/ufs2_image.py).
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ultrium_qemu as U
import ufs2_image as I

MESSAGE = b"Written by filedemo through system calls.\n"


def mount(vm):
    vm.line("new folder /volumes")
    vm.line("new folder /volumes/disk")
    vm.line("ultrium storage mount vda /volumes/disk")
    vm.run_test("ultrium test interrupts", 2)


def host_check(path, expected_files, state, label, prefix_file=None, prefix_seed=77):
    img = I.Image(path)
    tree = img.tree()
    extra = None
    if prefix_file:
        data = tree.pop(prefix_file, None)
        if data is None or not data or data != I.pattern(len(data), prefix_seed):
            raise RuntimeError("%s: %s is missing or not a valid prefix of what the program wrote" % (label, prefix_file))
        extra = len(data)
    if tree != expected_files:
        raise RuntimeError("%s: host-decoded image differs: %s vs %s" % (label, sorted(tree), sorted(expected_files)))
    problems = img.findings()
    if problems:
        raise RuntimeError("%s: the independent checker found problems: %s" % (label, problems[:5]))
    if state is not None and img.state() != state:
        raise RuntimeError("%s: superblock state %d, expected %d" % (label, img.state(), state))
    print("  host check (%s): consistent, %d item(s)%s, state %s, %d committed transaction(s) pending" %
          (label, len(tree), "" if extra is None else " + %s (%d valid bytes)" % (prefix_file, extra),
           "?" if state is None else ("clean" if state == 0 else "mounted (interrupted)"), img.pending_transactions()), flush=True)
    return extra


def main():
    path = U.make_image("process.img", 64)
    base = {"/filedemo.txt": MESSAGE}

    print("boot 1: userspace writes a persistent file; shutdown with programs still running", flush=True)
    vm = U.VM(image=path)
    try:
        vm.line("new folder /volumes")
        vm.line("new folder /volumes/disk")
        vm.line("ultrium storage format vda")
        vm.line("yes")
        vm.run_test("ultrium test interrupts", 2)
        vm.line("ultrium storage mount vda /volumes/disk")
        vm.run_test("ultrium test interrupts", 2)
        vm.run_test("ultrium test persist programwrite", 1)
        vm.line("run -b spin")
        vm.line("run -b counter")
        vm.run_test("ultrium test interrupts", 2)          # the shell and the timer work while both run
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, base, 0, "after boot 1")

    print("boot 2: integration scenarios, then userspace reads the file back", flush=True)
    vm = U.VM(image=path)
    try:
        vm.run_test("ultrium test integration", 4, timeout=240)
        mount(vm)
        vm.run_test("ultrium test persist programread", 1)
        vm.line("run filedemo")                              # a second run overwrites with identical content
        vm.run_test("ultrium test interrupts", 2)
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    host_check(path, base, 0, "after boot 2")

    print("boot 3: userspace disk writer, QEMU stopped abruptly", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        vm.run_test("ultrium test persist programspam", 1)
        time.sleep(3)
        vm.run_test("ultrium test interrupts", 2)
    finally:
        vm.close()                                           # terminates QEMU: explicitly not a shutdown
    n = host_check(path, base, 1, "after abrupt stop", prefix_file="/spam.bin")
    if n < 4096:
        raise RuntimeError("the disk writer had written less than one chunk")

    print("boot 4: recovery, then clean shutdown", flush=True)
    vm = U.VM(image=path)
    try:
        mount(vm)
        vm.run_test("ultrium test persist recovered", 1)
        vm.run_test("ultrium test persist programread", 1)
        vm.line("ultrium storage sync")
        vm.line("shutdown")
        vm.expect_guest_shutdown()
    finally:
        vm.close()
    m = host_check(path, base, 0, "after recovery", prefix_file="/spam.bin")
    if m != n:
        raise RuntimeError("replay changed the length of the file (%d -> %d)" % (n, m))
    print("PASS userspace + persistent storage: programs wrote and read files, shutdown with live processes was clean, an abrupt stop during userspace file I/O recovered to a valid prefix")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, I.FormatError) as error:
        print("process test failed:", error, file=sys.stderr)
        sys.exit(1)
