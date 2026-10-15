"""Shared headless-QEMU helper for the Ultrium storage tests (standard library only).

Disk safety: the only disk a VM can be given is a regular, non-symlink file that
lives directly inside <repo>/test-images/. Anything else raises before QEMU starts.
"""
import errno, json, os, re, select, socket, subprocess, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMAGE_DIR = os.path.join(ROOT, "test-images")
KEYS = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
        '"': "shift-apostrophe", "'": "apostrophe", "\\": "backslash", "?": "shift-slash",
        ",": "comma", ":": "shift-semicolon", "=": "equal"}


def make_image(name, size_mib):
    """Create a fresh zero-filled (sparse) disposable image and return its path."""
    os.makedirs(IMAGE_DIR, exist_ok=True)
    path = os.path.join(IMAGE_DIR, name)
    check_image(path, must_exist=False)
    with open(path, "wb") as image:
        image.truncate(size_mib * 1024 * 1024)
    return path


def check_image(path, must_exist=True):
    real_dir = os.path.realpath(IMAGE_DIR)
    if os.path.dirname(os.path.abspath(path)) != real_dir or not path.endswith(".img"):
        raise RuntimeError("refusing disk outside test-images/: " + path)
    if os.path.islink(path):
        raise RuntimeError("refusing symlinked disk image: " + path)
    if os.path.exists(path):
        if not os.path.isfile(path):
            raise RuntimeError("refusing non-regular disk image: " + path)
    elif must_exist:
        raise RuntimeError("disk image does not exist: " + path)


class VM:
    def __init__(self, image=None, machine="q35", serial="ULTRIUM-SCRATCH", block_size=None,
                 readonly=False, extra=()):
        self.tmp = tempfile.TemporaryDirectory(prefix="ultrium-vm-")
        self.sock_path = os.path.join(self.tmp.name, "qmp.sock")
        cmd = ["qemu-system-x86_64", "-M", machine, "-m", "2G",
               "-drive", "if=pflash,unit=0,format=raw,file=" +
               os.path.join(ROOT, "edk2-ovmf-bins/ovmf-code-x86_64.fd") + ",readonly=on",
               "-cdrom", os.path.join(ROOT, "template-x86_64.iso"),
               "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-qmp", "unix:" + self.sock_path + ",server=on,wait=off", "-no-reboot"]
        if image:
            check_image(image)
            dev = "virtio-blk-pci,drive=ud0,serial=" + serial + ",disable-legacy=on,disable-modern=off"
            if block_size:
                dev += ",logical_block_size=%d,physical_block_size=%d" % (block_size, block_size)
            cmd += ["-drive", "if=none,id=ud0,format=raw,file=" + image +
                    (",readonly=on" if readonly else ""), "-device", dev]
        cmd += list(extra)
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=ROOT)
        self.output = bytearray()
        self.events = []
        self.forced = False
        self._connect()
        time.sleep(7)  # Limine timeout plus firmware boot

    def _connect(self):
        end = time.monotonic() + 15
        while True:
            if self.proc.poll() is not None:
                raise RuntimeError("QEMU exited during startup: " + self.proc.stderr.read().decode(errors="replace"))
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(self.sock_path)
                break
            except OSError as e:
                s.close()
                if e.errno not in (errno.ENOENT, errno.ECONNREFUSED) or time.monotonic() > end:
                    raise
                time.sleep(.05)
        self.sock = s
        self.stream = s.makefile("rwb", buffering=0)
        self.stream.readline()
        self.qmp({"execute": "qmp_capabilities"})

    def qmp(self, payload):
        self.stream.write(json.dumps(payload).encode() + b"\n")
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:
                self.events.append(msg)
            elif "return" in msg:
                return msg["return"]
            elif "error" in msg:
                raise RuntimeError(str(msg["error"]))

    def key(self, k):
        self.qmp({"execute": "human-monitor-command", "arguments": {"command-line": "sendkey " + k + " 20"}})
        time.sleep(.035)

    def type(self, text):
        for c in text:
            k = KEYS.get(c, c)
            if "A" <= c <= "Z":
                k = "shift-" + c.lower()
            self.key(k)

    def line(self, text):
        self.type(text)
        self.key("ret")

    def _pump(self, timeout):
        for _ in range(1):
            ready = select.select([self.proc.stdout], [], [], timeout)[0]
            if ready:
                chunk = os.read(self.proc.stdout.fileno(), 4096)
                if chunk:
                    self.output.extend(chunk)

    def wait_for(self, pattern, timeout=40, count=1):
        end = time.monotonic() + timeout
        rx = re.compile(pattern)
        while time.monotonic() < end:
            if len(rx.findall(self.output.decode(errors="replace"))) >= count:
                return True
            if re.search(rb"TEST SUMMARY passed=\d+ failed=[1-9]", self.output):
                raise RuntimeError("kernel test failed:\n" + self.output.decode(errors="replace")[-3000:])
            self._pump(.3)
        raise RuntimeError("timed out waiting for %r; serial tail:\n%s" %
                           (pattern, self.output.decode(errors="replace")[-2500:]))

    def run_test(self, command, passed, timeout=60):
        """Type a kernel test command and require its exact passing summary."""
        mark = len(self.output)
        self.line(command)
        end = time.monotonic() + timeout
        want = "TEST SUMMARY passed=%d failed=0" % passed
        while time.monotonic() < end:
            text = self.output[mark:].decode(errors="replace")
            if want in text:
                return text
            if re.search(r"TEST SUMMARY passed=\d+ failed=[1-9]", text) or \
                    re.search(r"TEST SUMMARY passed=(\d+) failed=0", text):
                raise RuntimeError("unexpected summary for %r:\n%s" % (command, text[-3000:]))
            self._pump(.3)
        raise RuntimeError("timed out for %r; serial tail:\n%s" %
                           (command, self.output[mark:].decode(errors="replace")[-2500:]))

    def shutdown_event(self, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for e in self.events:
                if e["event"] == "SHUTDOWN":
                    return e["data"]
            if not select.select([self.sock], [], [], .5)[0]:
                continue
            data = self.stream.readline()
            if not data:
                break
            msg = json.loads(data)
            if "event" in msg:
                self.events.append(msg)
        for e in self.events:
            if e["event"] == "SHUTDOWN":
                return e["data"]
        return None

    def expect_guest_shutdown(self, timeout=30):
        """Require a guest-initiated power-off with QEMU exiting by itself (status 0)."""
        data = self.shutdown_event(timeout)
        try:
            rc = self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.forced = True
            raise RuntimeError("QEMU did not exit on its own (SHUTDOWN event: %r)" % (data,))
        if not data or not data.get("guest") or data.get("reason") != "guest-shutdown":
            raise RuntimeError("not a clean guest shutdown: %r (exit %s)" % (data, rc))
        if rc != 0:
            raise RuntimeError("QEMU exited with status %d" % rc)

    def screenshot(self, path):
        time.sleep(2)
        self.qmp({"execute": "screendump", "arguments": {"filename": os.path.abspath(path)}})

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.communicate()
        self.tmp.cleanup()
