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
    keyboard_regression = command_text == "--keyboard-regression"
    expected_count = {
        "ultrium system memory test": 1,
        "ultrium test memory": 14,
        "ultrium test interrupts": 2,
        "ultrium test filesystem": 4,
        "ultrium test all": 22,
        "ultrium test shell": 2,
    }.get(command_text)
    if not keyboard_regression and command_text not in (
            "ultrium test memory", "ultrium test all", "ultrium test interrupts",
            "ultrium test filesystem", "ultrium test shell",
            "ultrium system memory test"):
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

            def wait_for_summary(summary, count):
                deadline = time.monotonic() + 30
                while output.decode(errors="replace").count(summary) < count and time.monotonic() < deadline:
                    for key, _ in selector.select(.5):
                        chunk = os.read(key.fileobj.fileno(), 4096)
                        if chunk:
                            output.extend(chunk)
                        elif process.poll() is not None:
                            break
                if output.decode(errors="replace").count(summary) < count:
                    tail = output.decode(errors="replace")[-2000:]
                    raise RuntimeError("timed out waiting for " + repr(summary) +
                                       " occurrence " + str(count) + "; serial output tail:\n" + tail)

            def send_key(key):
                qmp({"execute": "human-monitor-command",
                     "arguments": {"command-line": "sendkey " + key + " 20"}})
                time.sleep(.035)

            def type_text(value):
                for character in value:
                    send_key("spc" if character == " " else character)

            if keyboard_regression:
                memory_summary = "TEST SUMMARY passed=1 failed=0"
                edited_command = "ultrium system memory test"

                # Exercise Home/End, both cursor directions, insertion,
                # backspace, and forward Delete, correcting a deliberate typo.
                type_text("ultrium system memorry test")
                for key in ("home", "right", "left", "end"):
                    send_key(key)
                for _ in range(6):
                    send_key("left")
                send_key("backspace")
                send_key("r")
                send_key("delete")
                send_key("y")
                send_key("left")
                send_key("backspace")
                send_key("ret")
                wait_for_summary(memory_summary, 1)

                # Up must restore the previous command and execute it again.
                send_key("up")
                send_key("ret")
                wait_for_summary(memory_summary, 2)

                # Each shortcut is followed by a command whose test summary
                # proves that input cancellation/editing did not poison the line.
                type_text("discard")
                send_key("ctrl-c")
                type_text(edited_command)
                send_key("ret")
                wait_for_summary(memory_summary, 3)

                type_text("discard")
                send_key("ctrl-u")
                type_text(edited_command)
                send_key("ret")
                wait_for_summary(memory_summary, 4)

                type_text(edited_command + " discard")
                send_key("ctrl-w")
                send_key("ret")
                wait_for_summary(memory_summary, 5)

                type_text(edited_command)
                send_key("ctrl-l")
                send_key("ret")
                wait_for_summary(memory_summary, 6)
                decoded = output.decode(errors="replace")
                print(decoded, end="")
                if decoded.count(memory_summary) != 6:
                    raise RuntimeError(
                        "keyboard regression did not execute the edited and replayed commands"
                    )
                return 0

            expected = "TEST SUMMARY passed=" + str(expected_count) + " failed=0"
            # Repeat in one boot so allocator accounting and ownership cleanup
            # are checked against the prior run's resulting state.
            for repetition in range(2):
                for key in command_text:
                    send_key("spc" if key == " " else key)
                send_key("ret")
                wait_for_summary(expected, repetition + 1)
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
