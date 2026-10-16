"""Independent host-side reader and verifier for UltriumFS v2 images (standard library only).

Written from docs/ultriumfs-v2-format.md, not from the kernel source, so it also checks that the
specification is accurate. It never opens an image for writing. Typical use:

    img = Image(path)            # picks the newest valid superblock copy
    img.journal()                # committed-but-unapplied transactions, torn tail info
    img.tree()                   # {path: None | bytes}, applying pending committed transactions virtually
    img.findings()               # list of (class, text); empty list = consistent
"""
import struct

BLOCK = 4096
POLY = 0x82F63B78
_table = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (POLY if _c & 1 else 0)
    _table.append(_c)


def crc32c(data, crc=0xFFFFFFFF):
    for b in data:
        crc = _table[(crc ^ b) & 0xFF] ^ (crc >> 8)
    return crc


def crc_final(crc):
    return crc ^ 0xFFFFFFFF


class FormatError(Exception):
    pass


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = bytearray(f.read())
        self.path = path
        blocks = len(self.data) // BLOCK
        self.capacity = min(blocks, 1 << 20)
        a = self._decode_sb(0)
        b_loc = (a["total"] - 1) if a else self.capacity - 1
        b = self._decode_sb(b_loc)
        if b and b["total"] - 1 != b_loc:
            b = None
        self.copies = {"A": a, "B": b}
        if not a and not b:
            raise FormatError("both superblock copies are invalid")
        if a and b:
            for key in ("version", "total", "compat", "ro_compat", "incompat", "uuid", "label", "geo"):
                if a[key] != b[key]:
                    raise FormatError("superblock copies disagree on %s" % key)
            self.sb = a if a["gen"] >= b["gen"] else b
        else:
            self.sb = a or b
        self.uuid = self.sb["uuid"]
        self.geo = self.sb["geo"]
        self.total = self.sb["total"]
        if self.sb["incompat"] & ~3:
            raise FormatError("unsupported incompatible feature bits")

    # ---- primitives ----
    def block(self, n):
        if n < 0 or (n + 1) * BLOCK > len(self.data):
            raise FormatError("block %d outside the image" % n)
        return bytes(self.data[n * BLOCK:(n + 1) * BLOCK])

    def _decode_sb(self, n):
        if (n + 1) * BLOCK > len(self.data):
            return None
        raw = self.block(n)
        if raw[:8] != b"ULTRIUM2":
            return None
        if u32(raw, 252) != crc_final(crc32c(raw[:252], crc32c(struct.pack("<Q", n)))):
            return None
        total = u64(raw, 24)
        if u32(raw, 8) != 2 or u32(raw, 12) != BLOCK or not (512 <= total <= 1 << 20):
            return None
        if n != 0 and n != total - 1:
            return None
        geo = dict(zip(("journal_start", "journal_blocks", "max_txn", "bitmap_start", "bitmap_blocks", "inode_start",
                        "inode_count", "inode_blocks", "data_start"), struct.unpack_from("<9I", raw, 96)))
        return {"version": 2, "gen": u64(raw, 16), "total": total, "compat": u32(raw, 32), "ro_compat": u32(raw, 36),
                "incompat": u32(raw, 40), "state": u32(raw, 44), "uuid": raw[48:64],
                "label": raw[64:96].rstrip(b"\0").decode("ascii", "replace"), "geo": geo,
                "tail_seq": u64(raw, 144), "tail_pos": u32(raw, 152), "block": n}

    def _ctx_crc(self, ctx, data):
        return crc_final(crc32c(data, crc32c(ctx, crc32c(self.uuid))))

    # ---- journal ----
    def journal(self):
        """Committed transactions found from the tail: list of dicts. Also reports an incomplete tail."""
        g = self.geo
        j = g["journal_blocks"]
        pos, expected = self.sb["tail_pos"], self.sb["tail_seq"]
        txns, incomplete = [], False

        def jb(p):
            return self.block(g["journal_start"] + p % j)
        for _ in range(j // 3 + 1):
            d = jb(pos)
            if u32(d, 0) != 0x32444A55:
                break
            if u32(d, 4092) != self._ctx_crc(struct.pack("<I", pos), d[:4092]):
                incomplete = True
                break
            if u64(d, 8) != expected:
                break
            n = u32(d, 4)
            if not (1 <= n <= g["max_txn"]) or n + 2 > j or any(d[16:24]):
                incomplete = True
                break
            targets = list(struct.unpack_from("<%dI" % n, d, 24))
            if len(set(targets)) != n or any(t == 0 or t >= self.total - 1 or g["journal_start"] <= t < g["journal_start"] + j for t in targets):
                incomplete = True
                break
            payload = [jb(pos + 1 + i) for i in range(n)]
            c = jb(pos + n + 1)
            pcrc = crc_final(crc32c(b"".join([d] + payload), crc32c(struct.pack("<Q", expected), crc32c(self.uuid))))
            if (u32(c, 0) != 0x32434A55 or u32(c, 4) != n or u64(c, 8) != expected or u32(c, 16) != pcrc or any(c[20:4092]) or
                    u32(c, 4092) != self._ctx_crc(struct.pack("<I", (pos + n + 1) % j), c[:4092])):
                incomplete = True
                break
            txns.append({"seq": expected, "targets": targets, "payload": payload})
            pos = (pos + n + 2) % j
            expected += 1
        return {"tail_pos": self.sb["tail_pos"], "tail_seq": self.sb["tail_seq"], "committed": txns, "incomplete_tail": incomplete}

    def overlay(self):
        """Block overrides implied by committed-but-unapplied transactions (what recovery will write)."""
        out = {}
        for t in self.journal()["committed"]:
            for tgt, img in zip(t["targets"], t["payload"]):
                out[tgt] = img
        return out

    def read(self, n):
        o = self.overlay_cache.get(n) if hasattr(self, "overlay_cache") else None
        return o if o is not None else self.block(n)

    def apply_journal_virtually(self):
        self.overlay_cache = self.overlay()

    # ---- metadata ----
    def inode(self, ino):
        g = self.geo
        raw = self.read(g["inode_start"] + (ino - 1) // 16)[((ino - 1) % 16) * 256:][:256]
        if not any(raw):
            return {"type": 0, "gen": 0, "pristine": True}
        if u32(raw, 252) != self._ctx_crc(struct.pack("<I", ino), raw[:252]):
            raise FormatError("inode %d checksum" % ino)
        t, depth = raw[0], raw[2]
        node = {"type": t, "depth": depth, "gen": u32(raw, 4), "size": u64(raw, 8), "parent": u32(raw, 16),
                "extent_count": u32(raw, 24), "block_count": u32(raw, 28), "txn_seq": u64(raw, 32),
                "root_entries": struct.unpack_from("<H", raw, 40)[0], "pristine": False}
        node["slots"] = [struct.unpack_from("<4I", raw, 64 + 16 * i) for i in range(11)][:node["root_entries"]]
        return node

    def extents(self, node):
        if node["depth"] == 0:
            return [s for s in node["slots"]]
        out = []
        for first, leaf, count, _ in node["slots"]:
            raw = self.read(leaf)
            if u32(raw, 0) != 0x4C545845 or u32(raw, 4092) != self._ctx_crc(struct.pack("<Q", leaf), raw[:4092]):
                raise FormatError("extent leaf %d invalid" % leaf)
            n = struct.unpack_from("<H", raw, 12)[0]
            out += [struct.unpack_from("<4I", raw, 32 + 16 * i) for i in range(n)]
        return out

    def file_bytes(self, node):
        blocks = []
        for logical, physical, length, flags in self.extents(node):
            blocks += [self.read(physical + k) for k in range(length)]
        return b"".join(blocks)[:node["size"]]

    def entries(self, ino, node):
        raw = b"".join(
            self.read(p + k) for (_, p, ln, _) in self.extents(node) for k in range(ln))
        for bi in range(len(raw) // BLOCK):
            blk = raw[bi * BLOCK:(bi + 1) * BLOCK]
            if u32(blk, 4092) != self._ctx_crc(struct.pack("<Q", self._phys(node, bi)), blk[:4092]):
                raise FormatError("directory block checksum (inode %d, block %d)" % (ino, bi))
            for i in range(63):
                e = blk[i * 64:(i + 1) * 64]
                child = u32(e, 0)
                if child:
                    yield e[12:12 + e[9]].decode(), child, u32(e, 4), e[8]

    def _phys(self, node, logical):
        for lg, p, ln, _ in self.extents(node):
            if lg <= logical < lg + ln:
                return p + logical - lg
        raise FormatError("logical block %d unmapped" % logical)

    def tree(self):
        """{path: None (directory) | bytes}; applies committed-but-unapplied transactions virtually."""
        self.apply_journal_virtually()
        out, stack, seen = {}, [("", 1)], set()
        while stack:
            prefix, ino = stack.pop()
            if ino in seen:
                raise FormatError("inode %d reachable twice" % ino)
            seen.add(ino)
            node = self.inode(ino)
            if node["type"] != 2:
                raise FormatError("inode %d is not a directory" % ino)
            for name, child, gen, kind in self.entries(ino, node):
                cn = self.inode(child)
                if cn["type"] != kind or cn["gen"] != gen:
                    raise FormatError("entry %s/%s does not match its inode" % (prefix, name))
                path = prefix + "/" + name
                if kind == 2:
                    out[path] = None
                    stack.append((path, child))
                else:
                    out[path] = self.file_bytes(cn)
        return out

    def findings(self):
        """Independent consistency check. Returns a list of strings (empty = consistent)."""
        self.apply_journal_virtually()
        g, problems = self.geo, []
        for name, copy in self.copies.items():
            if copy is None:
                problems.append("superblock copy %s is damaged (the other copy is being used)" % name)
        own = bytearray((self.total + 7) // 8 + 8)

        def claim(b, what):
            if own[b >> 3] & (1 << (b & 7)):
                problems.append("block %d owned twice (%s)" % (b, what))
            own[b >> 3] |= 1 << (b & 7)
        for b in range(g["data_start"]):
            claim(b, "metadata")
        claim(self.total - 1, "superblock B")
        referenced = {1}
        live = {}
        for ino in range(1, g["inode_count"] + 1):
            try:
                node = self.inode(ino)
            except FormatError as e:
                problems.append(str(e)); continue
            if node["type"] == 0:
                continue
            live[ino] = node
            try:
                ex = self.extents(node)
            except FormatError as e:
                problems.append(str(e)); continue
            expect, data = 0, 0
            if node["depth"] == 1:
                for _, leaf, _, _ in node["slots"]:
                    claim(leaf, "leaf of inode %d" % ino)
            for lg, p, ln, fl in ex:
                if fl or lg != expect or ln < 1 or p < g["data_start"] or p + ln > self.total - 1:
                    problems.append("inode %d has an invalid extent" % ino); break
                for k in range(ln):
                    claim(p + k, "inode %d" % ino)
                expect += ln; data += ln
            want = node["size"] // BLOCK if node["type"] == 2 else (node["size"] + BLOCK - 1) // BLOCK
            leaves = len(node["slots"]) if node["depth"] == 1 else 0
            if data != want or node["extent_count"] != len(ex) or node["block_count"] != data + leaves:
                problems.append("inode %d counts are inconsistent" % ino)
        for ino, node in live.items():
            if node["type"] != 2:
                continue
            try:
                for name, child, gen, kind in self.entries(ino, node):
                    if child not in live or live[child]["gen"] != gen or live[child]["type"] != kind:
                        problems.append("dangling entry %s in inode %d" % (name, ino)); continue
                    if child in referenced:
                        problems.append("inode %d referenced twice" % child)
                    referenced.add(child)
                    if live[child]["parent"] != ino:
                        problems.append("inode %d has the wrong parent" % child)
            except FormatError as e:
                problems.append(str(e))
        for ino in live:
            if ino not in referenced:
                problems.append("orphan inode %d" % ino)
        # bitmap
        for k in range(g["bitmap_blocks"]):
            raw = self.read(g["bitmap_start"] + k)
            if u32(raw, 4092) != self._ctx_crc(struct.pack("<Q", g["bitmap_start"] + k), raw[:4092]):
                problems.append("bitmap block %d checksum" % k); continue
            for j in range(32704):
                b = k * 32704 + j
                on = bool(raw[j >> 3] & (1 << (j & 7)))
                if b >= self.total:
                    if not on:
                        problems.append("bitmap padding bit %d clear" % b); break
                    continue
                if on != bool(own[b >> 3] & (1 << (b & 7))):
                    problems.append("bitmap bit %d (%s)" % (b, "leaked" if on else "owned but free")); break
        return problems

    def pending_transactions(self):
        return len(self.journal()["committed"])

    def state(self):
        return self.sb["state"]


def pattern(size, seed):
    return bytes((seed + o * 31 + (o >> 9)) & 255 for o in range(size))
