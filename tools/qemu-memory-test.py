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
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    command_text = sys.argv[1] if len(sys.argv) > 1 else "ultrium test memory"
    keyboard_regression = command_text == "--keyboard-regression"
    shell_regression = command_text == "--shell-regression"
    expected_count = {
        "ultrium system memory test": 1,
        "ultrium test memory": 14,
        "ultrium test interrupts": 2,
        "ultrium test filesystem": 8,
        "ultrium test all": 95,
        "ultrium test ultriumfs": 20,
        "ultrium test ufs2": 16,
        "ultrium test ufs2fs": 21,
        "ultrium test storage": 7,
        "ultrium test shell": 7,
    }.get(command_text)
    if not keyboard_regression and not shell_regression and command_text not in (
            "ultrium test memory", "ultrium test all", "ultrium test interrupts",
            "ultrium test filesystem", "ultrium test shell",
            "ultrium test storage", "ultrium test ultriumfs", "ultrium test ufs2", "ultrium test ufs2fs",
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
                        if re.search(rb"TEST SUMMARY passed=\d+ failed=[1-9]\d*", output):
                            raise RuntimeError("kernel test failed:\n" + output.decode(errors="replace")[-4000:])
                        if not chunk and process.poll() is not None:
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
                    keys = {" ": "spc", "/": "slash", ".": "dot", "-": "minus",
                            "_": "shift-minus", '"': "shift-apostrophe", "'": "apostrophe",
                            "\\": "backslash", "?": "shift-slash"}
                    key = keys.get(character, character)
                    if "A" <= character <= "Z": key = "shift-" + character.lower()
                    send_key(key)

            if shell_regression:
                def run_line(text):
                    type_text(text)
                    send_key("ret")

                for repetition in range(2):
                    run_line("ultrium test shell begin")
                    run_line("ls")
                    run_line("ultrium shell compatibility enable")
                    run_line("mkdir /keyboard")
                    run_line("ultrium shell compatibility disable")
                    run_line('new folder "/keyboard/My Projects"')
                    run_line('go "/keyboard/My Projects"')
                    run_line('write "project notes.txt" "Hello Ultrium"')
                    type_text('view "project n')
                    send_key("tab")
                    send_key("ret")
                    type_text("ultrium sh")
                    send_key("tab")
                    type_text("compatibility st")
                    send_key("tab")
                    send_key("ret")
                    type_text("wher")
                    send_key("tab")
                    send_key("ret")
                    run_line("new file apple")
                    run_line("new file apricot")
                    # A serial checkpoint drains the injected keyboard queue
                    # before observing completion and checks IRQ health here.
                    run_line("ultrium test interrupts")
                    wait_for_summary("TEST SUMMARY passed=2 failed=0", repetition + 1)
                    type_text("view a")
                    send_key("tab")
                    send_key("tab")
                    screenshot = os.environ.get("ULTRIUM_QEMU_SCREENSHOT")
                    if screenshot:
                        time.sleep(2)  # Let framebuffer rendering drain the injected key queue.
                        qmp({"execute": "screendump", "arguments": {"filename": os.path.abspath(screenshot)}})
                    send_key("ctrl-c")
                    run_line('new file "unfinished')
                    run_line("lst")
                    type_text("new file midx")
                    for key in ("left", "backspace", "d", "delete", "ret"):
                        send_key(key)
                    run_line("new folder history")
                    send_key("up")
                    send_key("ret")
                    boundary = "new file /keyboard/boundary"
                    run_line(boundary + " " * (70 - len(boundary)) + "x")
                    run_line("new folder /keyboard/remove")
                    run_line("new file /keyboard/remove/a")
                    type_text("remove /keyboard/remove -r")
                    send_key("ret")
                    send_key("ctrl-c")
                    run_line("remove /keyboard/remove -r")
                    send_key("ret")
                    run_line("remove /keyboard/remove -r")
                    run_line("no")
                    run_line("remove /keyboard/remove -y")
                    run_line("remove /keyboard/remove -r")
                    type_text("n")
                    send_key("backspace")
                    run_line("yes")
                    run_line("ultrium test shell interactive")
                    wait_for_summary("TEST SUMMARY passed=1 failed=0", repetition + 1)
                    print("USH keyboard session", repetition + 1, "passed", flush=True)
                print(output.decode(errors="replace"), end="")
                return 0

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
                type_text(command_text)
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
