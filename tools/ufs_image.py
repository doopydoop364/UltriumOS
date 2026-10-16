"""Independent host-side reader for UltriumFS v1 images (standard library only).

Written from docs/ultriumfs-format.md, not from the kernel source, so it also
checks that the document is accurate. Read-only: it never opens an image for writing.
"""
import struct, zlib

BLOCK = 4096


class FormatError(Exception):
    pass


def crc(data):
    return zlib.crc32(data) & 0xffffffff


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        sb = self.block(0)
        if sb[:8] != b"ULTRIUMF":
            raise FormatError("bad magic")
        if struct.unpack_from("<I", sb, 124)[0] != crc(sb[:124]):
            raise FormatError("superblock checksum")
        (self.version, self.block_size, self.total, self.inode_count, self.inode_start,
         self.inode_blocks, self.data_start, self.root, self.state) = struct.unpack_from("<IIQIIIIII", sb, 8)
        self.label = sb[56:72].rstrip(b"\0").decode()
        self.features = struct.unpack_from("<I", sb, 80)[0]
        if self.version != 1 or self.block_size != BLOCK or self.features != 0:
            raise FormatError("unsupported version or features")

    def block(self, n):
        return self.data[n * BLOCK:(n + 1) * BLOCK]

    def inode(self, ino):
        raw = self.block(self.inode_start + (ino - 1) // 32)[((ino - 1) % 32) * 128:][:128]
        if not any(raw):
            return None
        if struct.unpack_from("<I", raw, 124)[0] != crc(raw[:124]):
            raise FormatError("inode %d checksum" % ino)
        kind, = struct.unpack_from("<B", raw, 0)
        size, = struct.unpack_from("<Q", raw, 8)
        direct = list(struct.unpack_from("<12I", raw, 16))
        indirect, count = struct.unpack_from("<II", raw, 64)
        return {"type": kind, "size": size, "direct": direct, "indirect": indirect, "count": count}

    def file_bytes(self, node):
        n = (node["size"] + BLOCK - 1) // BLOCK
        blocks = node["direct"][:min(n, 12)]
        if n > 12:
            ind = self.block(node["indirect"])
            blocks += list(struct.unpack_from("<%dI" % (n - 12), ind, 0))
        return b"".join(self.block(b) for b in blocks)[:node["size"]]

    def entries(self, node):
        raw = self.file_bytes(node)
        for i in range(0, len(raw), 64):
            e = raw[i:i + 64]
            ino, kind, length = struct.unpack_from("<IBB", e, 0)
            if ino:
                yield e[8:8 + length].decode(), ino, kind

    def tree(self):
        """Map absolute path -> None (directory) or bytes (file)."""
        out, stack = {}, [("", self.root)]
        seen = set()
        while stack:
            prefix, ino = stack.pop()
            if ino in seen:
                raise FormatError("inode %d reachable twice" % ino)
            seen.add(ino)
            node = self.inode(ino)
            if node is None or node["type"] != 2:
                raise FormatError("inode %d is not a directory" % ino)
            for name, child, kind in self.entries(node):
                path = prefix + "/" + name
                cn = self.inode(child)
                if cn is None or cn["type"] != kind:
                    raise FormatError("entry %s does not match its inode" % path)
                if kind == 2:
                    out[path] = None
                    stack.append((path, child))
                else:
                    out[path] = self.file_bytes(cn)
        return out


def pattern(size, seed):
    return bytes((seed + o * 31 + (o >> 9)) & 255 for o in range(size))
