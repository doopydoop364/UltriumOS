#!/usr/bin/env python3
"""Headless QEMU power tests for Ultrium OS.

  --shutdown     `shutdown` must produce a QMP SHUTDOWN event with
                 reason guest-shutdown and QEMU must exit by itself, status 0.
  --reboot       `reboot` must produce reason guest-reset (QEMU runs with
                 -no-reboot so a reset surfaces as an exit, not a restart).
  --unsupported  With ACPI disabled in the machine, `shutdown` must fail,
                 produce no SHUTDOWN event, and leave the shell responsive.

A QEMU process that has to be terminated by this script never counts as a
guest shutdown. All waits are bounded.
"""
import json, os, select, socket, subprocess, sys, tempfile, time, errno, selectors, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "--shutdown"
    if mode not in ("--shutdown", "--reboot", "--unsupported"):
        raise RuntimeError("unsupported mode " + mode)
    machine = "q35,acpi=off" if mode == "--unsupported" else os.environ.get("ULTRIUM_QEMU_MACHINE", "q35")
    with tempfile.TemporaryDirectory(prefix="ultrium-power-") as tmp:
        sock = os.path.join(tmp, "qmp.sock")
        cmd = ["qemu-system-x86_64", "-M", machine, "-m", "2G",
               "-drive", "if=pflash,unit=0,format=raw,file=" +
               os.path.join(ROOT, "edk2-ovmf-bins/ovmf-code-x86_64.fd") + ",readonly=on",
               "-cdrom", os.path.join(ROOT, "template-x86_64.iso"),
               "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-qmp", "unix:" + sock + ",server=on,wait=off", "-no-reboot"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        forced = False
        try:
            end = time.monotonic() + 15
            while True:
                if proc.poll() is not None:
                    raise RuntimeError("QEMU exited during startup: " + proc.stderr.read().decode(errors="replace"))
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    s.connect(sock); break
                except OSError as e:
                    s.close()
                    if e.errno not in (errno.ENOENT, errno.ECONNREFUSED) or time.monotonic() > end: raise
                    time.sleep(.05)
            stream = s.makefile("rwb", buffering=0)
            stream.readline()
            events = []

            def qmp(payload):
                stream.write(json.dumps(payload).encode() + b"\n")
                while True:
                    line = stream.readline()
                    if not line: raise RuntimeError("QMP closed")
                    msg = json.loads(line)
                    if "event" in msg: events.append(msg); continue
                    if "return" in msg: return msg["return"]
                    if "error" in msg: raise RuntimeError(str(msg["error"]))

            qmp({"execute": "qmp_capabilities"})
            time.sleep(7)  # Limine timeout plus firmware boot
            sel = selectors.DefaultSelector(); sel.register(proc.stdout, selectors.EVENT_READ)
            serial = bytearray()

            def key(k):
                qmp({"execute": "human-monitor-command", "arguments": {"command-line": "sendkey " + k + " 20"}})
                time.sleep(.035)

            def line(text):
                for c in text: key(KEYS.get(c, c))
                key("ret")

            def shutdown_event(timeout):
                """Wait for a SHUTDOWN event or EOF; return its data or None."""
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    for e in events:
                        if e["event"] == "SHUTDOWN": return e["data"]
                    if not select.select([s], [], [], .5)[0]: continue
                    data = stream.readline()
                    if not data: break
                    msg = json.loads(data)
                    if "event" in msg: events.append(msg)
                for e in events:
                    if e["event"] == "SHUTDOWN": return e["data"]
                return None

            if mode == "--unsupported":
                line("shutdown")
                data = shutdown_event(12)
                if data is not None or proc.poll() is not None:
                    raise RuntimeError("shutdown unexpectedly powered off without ACPI: " + str(data))
                line("ultrium test interrupts")
                deadline = time.monotonic() + 30
                while not re.search(rb"TEST SUMMARY passed=2 failed=0", serial) and time.monotonic() < deadline:
                    for k, _ in sel.select(.5):
                        serial.extend(os.read(k.fileobj.fileno(), 4096))
                if not re.search(rb"TEST SUMMARY passed=2 failed=0", serial):
                    raise RuntimeError("shell unresponsive after failed shutdown; serial:\n" + serial.decode(errors="replace")[-1500:])
                print("PASS unsupported: shutdown failed without powering off and the shell stayed responsive")
                return 0

            shot = os.environ.get("ULTRIUM_QEMU_SCREENSHOT")
            if shot:
                line(os.environ.get("ULTRIUM_QEMU_SCREENSHOT_CMD", "ultrium system power status")); time.sleep(2)
                qmp({"execute": "screendump", "arguments": {"filename": os.path.abspath(shot)}})
            line("shutdown" if mode == "--shutdown" else "reboot")
            data = shutdown_event(30)
            try:
                rc = proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                forced = True
                raise RuntimeError("QEMU did not exit on its own (SHUTDOWN event: %r)" % (data,))
            want = "guest-shutdown" if mode == "--shutdown" else "guest-reset"
            if data is None:
                raise RuntimeError("no QMP SHUTDOWN event; exit status %s (possible crash)" % rc)
            if not data.get("guest") or data.get("reason") != want:
                raise RuntimeError("SHUTDOWN event %r is not %s" % (data, want))
            if rc != 0:
                raise RuntimeError("QEMU exited with status %d" % rc)
            print("PASS %s: SHUTDOWN %s, QEMU exited by itself with status 0" % (mode[2:], data))
            return 0
        finally:
            if proc.poll() is None:
                proc.terminate()
                try: proc.communicate(timeout=3)
                except subprocess.TimeoutExpired: proc.kill(); proc.communicate()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print("QEMU power test failed:", error, file=sys.stderr)
        sys.exit(1)
