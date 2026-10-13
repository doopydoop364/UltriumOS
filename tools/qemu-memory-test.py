#!/usr/bin/env python3
"""Boot Ultrium in QEMU, type the memory suite, and check its serial report."""
import json
import os
import selectors
import socket
import subprocess
import sys
import tempfile
import time
import errno

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    command_text = sys.argv[1] if len(sys.argv) > 1 else "ultrium test memory"
    expected_count = {
        "ultrium system memory test": 1,
        "ultrium test memory": 14,
        "ultrium test interrupts": 2,
        "ultrium test all": 16,
    }.get(command_text)
    if command_text not in ("ultrium test memory", "ultrium test all",
                            "ultrium test interrupts", "ultrium system memory test"):
        raise RuntimeError("unsupported test command")
    with tempfile.TemporaryDirectory(prefix="ultrium-qmp-") as temporary:
        qmp_path = os.path.join(temporary, "qmp.sock")
        command = [
            "qemu-system-x86_64", "-M", os.environ.get("ULTRIUM_QEMU_MACHINE", "q35"), "-m", "2G",
            "-drive", "if=pflash,unit=0,format=raw,file=" +
            os.path.join(ROOT, "edk2-ovmf-bins/ovmf-code-x86_64.fd") + ",readonly=on",
            "-cdrom", os.path.join(ROOT, "template-x86_64.iso"),
            "-display", "none", "-serial", "stdio", "-monitor", "none",
            "-qmp", "unix:" + qmp_path + ",server=on,wait=off",
            "-no-reboot",
        ]
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, cwd=ROOT)
        try:
            deadline = time.monotonic() + 10
            while not os.path.exists(qmp_path) and time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("QEMU exited during startup: " +
                                       process.stderr.read().decode(errors="replace"))
                time.sleep(.05)
            connect_deadline = time.monotonic() + 10
            while True:
                monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                monitor.settimeout(1)
                try:
                    monitor.connect(qmp_path)
                    break
                except OSError as error:
                    monitor.close()
                    if error.errno not in (errno.ENOENT, errno.ECONNREFUSED) or \
                            time.monotonic() >= connect_deadline:
                        raise
                    if process.poll() is not None:
                        stderr = process.stderr.read().decode(errors="replace")
                        raise RuntimeError(
                            "QEMU exited before QMP became ready (status " +
                            str(process.returncode) + "): " + stderr
                        ) from error
                    time.sleep(.05)
            stream = monitor.makefile("rwb", buffering=0)
            stream.readline()

            def qmp(payload):
                stream.write(json.dumps(payload).encode() + b"\n")
                while True:
                    response = json.loads(stream.readline())
                    if "return" in response:
                        return response["return"]
                    if "error" in response:
                        raise RuntimeError(str(response["error"]))

            qmp({"execute": "qmp_capabilities"})
            # Limine's default timeout is three seconds. Allow for firmware boot.
            time.sleep(7)
            selector = selectors.DefaultSelector()
            selector.register(process.stdout, selectors.EVENT_READ)
            output = bytearray()
            expected = "TEST SUMMARY passed=" + str(expected_count) + " failed=0"
            # Repeat in one boot so allocator accounting and ownership cleanup
            # are checked against the prior run's resulting state.
            for repetition in range(2):
                for key in command_text:
                    qmp({"execute": "human-monitor-command",
                         "arguments": {"command-line": "sendkey " + ("spc" if key == " " else key)}})
                    time.sleep(.04)
                qmp({"execute": "human-monitor-command",
                     "arguments": {"command-line": "sendkey ret"}})
                deadline = time.monotonic() + 30
                while output.decode(errors="replace").count(expected) < repetition + 1 and time.monotonic() < deadline:
                    for key, _ in selector.select(.5):
                        chunk = os.read(key.fileobj.fileno(), 4096)
                        if chunk:
                            output.extend(chunk)
                        elif process.poll() is not None:
                            break
            decoded = output.decode(errors="replace")
            print(decoded, end="")
            if decoded.count(expected) != 2:
                raise RuntimeError("expected two passing memory-test summaries on COM1")
            return 0
        finally:
            process.terminate()
            try:
                process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print("QEMU test failed:", error, file=sys.stderr)
        sys.exit(1)
