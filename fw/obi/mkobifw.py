#!/usr/bin/env python3
"""Build the v90d firmware file for the Obihai OBi200 from the owner's own
OBi firmware file.

    mkobifw.py BASE.fw OUT.fw [--files DIR] [--full]

BASE.fw is an OBi2xx firmware file the box already runs or accepts (the
community firmware, e.g. OBi2xx-3-2-2-8680EX-arrynrob13.fw, whose update page
takes unsigned images). Only its /obi segment (type 3, the maker's
application, flashed at 0xC00000) is replaced: our files, plus the maker's
telephony, LED and watchdog kernel modules copied out of BASE's own /obi, so that we
ship none of the maker's code. Kernel, U-Boot, root file system and the
recovery images stay as they are on the box: OUT.fw carries the one segment
(with --full, all of BASE's segments, ours in place of its /obi).

The file format, as read off the published files and the update page's
checks (no vendor code):
  0x00  "OBi2XX_FW_PACK00"; 0x28: the file's check (below) over its first
        0x400 octets; 0x40: version text, "3.2.2 (Build: NNNNEX-...)";
        0x80: number of segments, 0x84: length of the file (le32);
        0xC0: 64-octet records { le32 type, file offset, flash address,
        size; md5(payload) }; 0x280..0x400: signature material that the
        community firmware's update page does not look at
  segment: 64-octet header { be32 0x000001ba, 12 zero octets,
        md5(payload), the segment's check (below), le32 type, file offset,
        flash address, segment size } and the payload.
  A check is the MD5 of a fixed string of the maker's application followed
  by the data with the check's own 16 octets zeroed. The string is not in
  this file: it is found in BASE's own application, as the string that
  reproduces BASE's own file check.
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

MAGIC = b"OBi2XX_FW_PACK00"
SEG_MAGIC = b"\x00\x00\x01\xba"
OBI_TYPE = 3
OBI_FLASH = 0xC00000
OBI_PART = 0x240000             # size of the /obi partition (mtd7)
FIRST_SEG = 0x400
KEEP_FROM_BASE = ("ffxav_mp.ko", "ffxav_led.ko", "ffxav_wdt.ko")
OURS = ("obi", "udhcpc.script", "obiline", "obinv", "v90d", "v90d.conf", "VERSION")
EXEC = ("obi", "udhcpc.script", "obiline", "obinv", "v90d")


def segments(fw):
    if fw[:16] != MAGIC:
        sys.exit("not an OBi2xx firmware file (no %r)" % MAGIC)
    if len(fw) < FIRST_SEG:
        sys.exit("not a whole OBi2xx firmware file")
    n = struct.unpack_from("<I", fw, 0x80)[0]
    if not 1 <= n <= 7:
        sys.exit("the file lists %d parts" % n)
    out = []
    for i in range(n):
        t, off, flash, size = struct.unpack_from("<IIII", fw, 0xC0 + i * 0x40)
        if off < FIRST_SEG or size < 0x40 or off + size > len(fw):
            sys.exit("part %d lies outside the file" % i)
        seg = fw[off:off + size]
        if seg[:4] != SEG_MAGIC or hashlib.md5(seg[0x40:]).digest() != seg[16:32]:
            sys.exit("segment %d (type %d) is damaged" % (i, t))
        out.append((t, flash, seg))
    return out


def check(key, data, at):
    z = bytearray(data)
    z[at:at + 16] = bytes(16)
    return hashlib.md5(key + bytes(z)).digest()


def find_key(app, base):
    """the string of the maker's application that reproduces BASE's file check"""
    want = base[0x28:0x38]
    run = bytearray()
    for c in app + b"\0":
        if 32 <= c < 127:
            run.append(c)
            continue
        if 8 <= len(run) <= 64 and check(bytes(run), base[:FIRST_SEG], 0x28) == want:
            return bytes(run)
        run = bytearray()
    sys.exit("BASE's application does not reproduce BASE's own check: not a firmware this builder knows")


def make_segment(key, t, flash, payload, file_off):
    h = bytearray(0x40)
    h[0:4] = SEG_MAGIC
    h[16:32] = hashlib.md5(payload).digest()
    struct.pack_into("<IIII", h, 0x30, t, file_off, flash, 0x40 + len(payload))
    seg = h + payload
    seg[32:48] = check(key, seg, 32)
    return bytes(seg)


def pack(key, base, segs, version):
    """segs: (type, flash address, payload)"""
    assert 1 <= len(segs) <= 7, "a header holds seven parts at most"
    hdr = bytearray(FIRST_SEG)
    hdr[0:16] = MAGIC
    # the signature material, as the base file has it (not checked by the
    # community firmware's update page; we cannot make a valid one)
    hdr[0x20:0x40] = base[0x20:0x40]
    hdr[0x280:FIRST_SEG] = base[0x280:FIRST_SEG]
    v = version.encode()[:0x3f]
    hdr[0x40:0x40 + len(v)] = v
    struct.pack_into("<I", hdr, 0x80, len(segs))
    body = bytearray()
    off = FIRST_SEG
    for i, (t, flash, payload) in enumerate(segs):
        seg = make_segment(key, t, flash, payload, off)
        struct.pack_into("<IIII", hdr, 0xC0 + i * 0x40, t, off, flash, len(seg))
        hdr[0xC0 + i * 0x40 + 16:0xC0 + i * 0x40 + 32] = seg[16:32]
        body += seg
        off += len(seg)
    struct.pack_into("<I", hdr, 0x84, FIRST_SEG + len(body))     # length of the whole file
    hdr[0x28:0x38] = check(key, hdr, 0x28)
    return bytes(hdr) + bytes(body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--files", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "image"),
                    help="directory with our files (%s)" % ", ".join(OURS))
    ap.add_argument("--full", action="store_true", help="all of BASE's segments, not only /obi")
    a = ap.parse_args()

    base = open(a.base, "rb").read()
    segs = segments(base)
    obi = [s for s in segs if s[0] == OBI_TYPE]
    if len(obi) != 1 or obi[0][1] != OBI_FLASH:
        sys.exit("BASE has no /obi segment at 0x%x" % OBI_FLASH)
    for f in OURS:
        if not os.path.isfile(os.path.join(a.files, f)):
            sys.exit("missing %s in %s" % (f, a.files))
    version = open(os.path.join(a.files, "VERSION")).read().strip()

    with tempfile.TemporaryDirectory() as tmp:
        sq = os.path.join(tmp, "base.sqfs")
        open(sq, "wb").write(obi[0][2][0x40:])
        x = os.path.join(tmp, "base")
        subprocess.run(["unsquashfs", "-q", "-n", "-d", x, sq], check=True, stdout=subprocess.DEVNULL)
        root = os.path.join(tmp, "root")
        os.mkdir(root)
        key = find_key(open(os.path.join(x, "obiapp"), "rb").read(), base)
        for f in KEEP_FROM_BASE:
            src = os.path.join(x, f)
            if not os.path.isfile(src):
                sys.exit("BASE's /obi has no %s" % f)
            shutil.copyfile(src, os.path.join(root, f))
        for f in OURS:
            shutil.copyfile(os.path.join(a.files, f), os.path.join(root, f))
            os.chmod(os.path.join(root, f), 0o755 if f in EXEC else 0o644)
        out_sq = os.path.join(tmp, "obi.sqfs")
        # as the maker's: squashfs 4.0, zlib, 128 KiB blocks
        subprocess.run(["mksquashfs", root, out_sq, "-comp", "gzip", "-b", "131072", "-noappend",
                        "-all-root", "-no-xattrs", "-no-progress", "-quiet"], check=True, stdout=subprocess.DEVNULL)
        payload = open(out_sq, "rb").read()
    if len(payload) > OBI_PART:
        sys.exit("the /obi image is %d octets, the partition %d" % (len(payload), OBI_PART))

    new = [(OBI_TYPE, OBI_FLASH, payload)]
    if a.full:
        new = [(t, fl, payload if t == OBI_TYPE else seg[0x40:]) for t, fl, seg in segs]
    # the update page reads the version and build number off this text and
    # refuses a lower build: keep BASE's, name ours after the dash
    base_ver = base[0x40:0x80].split(b"\0")[0].decode(errors="replace")
    m = re.match(r"(\d+\.\d+\.\d+) \(Build: (\d+)([A-Za-z]*)", base_ver)
    if not m:
        sys.exit("BASE's version text %r is not one this builder knows" % base_ver)
    vtext = "%s (Build: %s%s-v90d-%s)" % (m.group(1), m.group(2), m.group(3), version)
    fw = pack(key, base, new, vtext)
    open(a.out, "wb").write(fw)
    print("%s: %d octets, %s, /obi %d of %d octets (%d%%), md5 %s" % (
        a.out, len(fw), vtext, len(payload), OBI_PART, 100 * len(payload) // OBI_PART, hashlib.md5(fw).hexdigest()))


if __name__ == "__main__":
    main()
