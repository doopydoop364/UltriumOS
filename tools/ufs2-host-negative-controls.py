#!/usr/bin/env python3
"""Negative controls for the host-side reader (tools/ufs2_image.py).

Takes the fixture image written by tools/qemu-ufs2-recovery-test.py, makes a COPY under test-images/,
applies one deliberate fault at a time and requires the independent reader to notice it (an exception,
a finding, or a changed tree). The original image is never modified and each mutated copy is deleted.
"""
import os, shutil, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ultrium_qemu as U
import ufs2_image as I

BLOCK = 4096


def flip(data, offset, mask=0x01):
    data[offset] ^= mask


def mutations(img, raw):
    geo = img.geo
    def data_byte():
        at = bytes(I.pattern(64, 41)); pos = raw.find(at)
        if pos < 0: raise RuntimeError("fixture data not found")
        flip(raw, pos + 7)
    def fix_bitmap_crc():
        # Recompute the bitmap block checksums so the reader must catch the SEMANTIC fault, not just the checksum.
        for k in range(geo["bitmap_blocks"]):
            off = (geo["bitmap_start"] + k) * BLOCK
            crc = img._ctx_crc(struct.pack("<Q", geo["bitmap_start"] + k), bytes(raw[off:off + 4092]))
            struct.pack_into("<I", raw, off + 4092, crc)
    def bit_address(block):
        k, j = divmod(block, 32704)     # each bitmap block holds 32704 bits plus its checksum
        return (geo["bitmap_start"] + k) * BLOCK + j // 8, 1 << (j % 8)
    def bitmap_clear():
        for blk in range(geo["data_start"], geo["data_start"] + 64):
            at, mask = bit_address(blk)
            if raw[at] & mask:
                raw[at] &= ~mask & 0xFF; fix_bitmap_crc(); return
        raise RuntimeError("no used block found")
    def bitmap_set():
        for blk in range(img.total - 2, geo["data_start"], -1):
            at, mask = bit_address(blk)
            if not raw[at] & mask:
                raw[at] |= mask; fix_bitmap_crc(); return
        raise RuntimeError("no free block found")
    def bitmap_checksum_only():
        at, mask = bit_address(geo["data_start"])
        raw[at] ^= mask
    def inode_field():
        base = geo["inode_start"] * BLOCK
        for i in range(base + 256, base + 2 * 256):   # first inodes in use
            if raw[i]:
                flip(raw, i + 8); return
        raise RuntimeError("no inode found")
    def superblock_a():
        flip(raw, 40)
    def superblock_both():
        flip(raw, 40); flip(raw, (img.total - 1) * BLOCK + 40)
    def directory_block():
        # Corrupt the first block that looks like a directory block holding the name "deep".
        pos = raw.rfind(b"deep\0")      # the live directory block, not an old copy in the journal
        if pos < 0: raise RuntimeError("directory entry not found")
        flip(raw, pos, 0x20)
    return [("one data byte flipped in a file", data_byte), ("a used block cleared in the bitmap (checksum fixed up)", bitmap_clear),
            ("a free block marked used in the bitmap (checksum fixed up)", bitmap_set),
            ("a bitmap bit flipped without fixing the checksum", bitmap_checksum_only), ("an inode field flipped", inode_field),
            ("superblock copy A damaged", superblock_a), ("both superblock copies damaged", superblock_both),
            ("a directory entry name damaged", directory_block)]


def detect(path, expected):
    try:
        img = I.Image(path)
        tree = img.tree()
        problems = img.findings()
    except (I.FormatError, KeyError, IndexError, ValueError, OverflowError, MemoryError) as e:
        return "reader refused the image: %s" % e
    if problems: return "findings: %s" % problems[0]
    if tree != expected: return "tree differs"
    return None


def main():
    base = os.path.join(U.IMAGE_DIR, "recovery-base.img")
    U.check_image(base)
    import importlib.util
    spec = importlib.util.spec_from_file_location("rec", os.path.join(os.path.dirname(os.path.abspath(__file__)), "qemu-ufs2-recovery-test.py"))
    rec = importlib.util.module_from_spec(spec); spec.loader.exec_module(rec)
    expected = rec.base_tree()
    if detect(base, expected) is not None:
        raise RuntimeError("the unmodified fixture does not verify")
    print("control: the unmodified fixture verifies")
    img = I.Image(base)
    failures = 0
    count = len(mutations(img, bytearray(open(base, "rb").read())))
    for k in range(count):
        raw = bytearray(open(base, "rb").read())
        label, mutate = mutations(img, raw)[k]
        mutate()
        copy = os.path.join(U.IMAGE_DIR, "negative-control.img")
        U.check_image(copy, must_exist=False)
        with open(copy, "wb") as f: f.write(raw)
        try:
            result = detect(copy, expected)
        finally:
            os.remove(copy)
        if result is None:
            print("  FAIL %s: NOT DETECTED" % label); failures += 1
        else:
            print("  detected: %s -> %s" % (label, result))
    # The original must be untouched.
    if detect(base, expected) is not None:
        raise RuntimeError("the fixture changed")
    if failures:
        raise RuntimeError("%d mutation(s) went undetected" % failures)
    print("PASS host reader: all %d deliberate faults detected; fixture unchanged" % count)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, I.FormatError) as error:
        print("negative controls failed:", error, file=sys.stderr)
        sys.exit(1)
