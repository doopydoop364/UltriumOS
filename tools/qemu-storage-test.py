#!/usr/bin/env python3
"""Block-device tests against disposable virtio disks (never a host disk).

  --diskless       boot with no disk: all existing suites and the memory-backed
                   storage cases must pass and the shell must report no devices
  --disk           512-byte blocks, read-write
  --disk-4k        4096-byte logical blocks
  --disk-readonly  read-only disk: writes must be refused and the image unchanged
"""
import hashlib, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ultrium_qemu as U


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--disk"
    if mode == "--diskless":
        vm = U.VM()
        try:
            vm.run_test("ultrium test storage", 7)
            vm.run_test("ultrium test all", 146)
            vm.line("ultrium test storage disk run")
            vm.wait_for(r"FAIL virtio disk identity and capacity: no scratch virtio disk attached", 20)
        finally:
            vm.close()
        print("PASS diskless: storage and full suites pass; disk tests refuse without a scratch disk")
        return 0
    kwargs = {"--disk": {}, "--disk-4k": {"block_size": 4096}, "--disk-readonly": {"readonly": True}}[mode]
    img = U.make_image("block-" + mode.strip("-") + ".img", 64)
    before = sha(img)
    vm = U.VM(image=img, **kwargs)
    try:
        vm.run_test("ultrium test storage disk run", 4)
        vm.run_test("ultrium test storage disk run", 4)  # repeat in the same boot
        vm.run_test("ultrium test all", 146)
    finally:
        vm.close()
    after = sha(img)
    if mode == "--disk-readonly" and before != after:
        raise RuntimeError("read-only image changed on the host")
    if mode != "--disk-readonly" and before == after:
        raise RuntimeError("image unchanged: writes did not reach the disk file")
    print("PASS %s: disk tests passed twice; host image %s" %
          (mode[2:], "unchanged" if before == after else "modified as expected"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print("QEMU storage test failed:", error, file=sys.stderr)
        sys.exit(1)
