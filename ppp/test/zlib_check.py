#!/usr/bin/env python3
"""PPP Deflate against an independent implementation: Python's zlib module,
used as a black box.

  zlib_check.py TOOL...     TOOL is the command that runs test/comp_tool
                            (for the ARM build: qemu-arm-static build/arm_comp_tool)
  zlib_check.py -q TOOL...  shorter sequences

1. Our compressor's packets are inflated by zlib (raw inflate, the window
   the packets were made for, the 00 00 FF FF put back after each packet,
   packets that went out in native form fed to zlib as stored blocks) and
   must give the original packets.
2. zlib's raw deflate output, flushed with Z_SYNC_FLUSH after every packet
   and with the 00 00 FF FF taken off, goes through our decompressor and
   must give the original packets; at every window size and at several
   levels and strategies so that stored, fixed and dynamic blocks all occur.
The data is generated here; nothing is read from files."""
import random, struct, subprocess, sys, zlib

TAIL = b"\x00\x00\xff\xff"
WORDS = ("the of and a to in is you that it he was for on are as with his they at be this have from or one "
         "had by word but not what all were we when your can said there use an each which she do how their "
         "if will up other about out many then them these so some her would make like him into time has look "
         "two more write go see number no way could people my than first water been call who oil its now find "
         "modem telephone exchange subscriber connection information network protocol compression").split()
TAGS = ['<p>', '</p>\r\n', '<a href="http://www.example.com/', '">', '</a>', '<td align="left" valign="top">', '</td>',
        '<tr>', '</tr>\r\n', '<table border="0" cellpadding="2" cellspacing="0" width="100%">\r\n', '</table>\r\n',
        '<font face="Arial, Helvetica" size="2">', '</font>', '<br>\r\n', '<b>', '</b>', '<img src="/images/',
        '.gif" width="', '" height="', '<li>', '</li>\r\n', '&nbsp;']


def payload(rng, kind, n):
    if kind == "random":
        return bytes(rng.getrandbits(8) for _ in range(n))
    if kind == "zeros":
        return bytes(n)
    if kind == "binary":
        out = bytearray()
        while len(out) < n:
            out += struct.pack("<HBBIHH", len(out) & 0xffff, 0x80, rng.randrange(4), rng.randrange(1 << 20), 0x45, rng.randrange(2))
        return bytes(out[:n])
    out = []
    size = 0
    while size < n:
        if kind == "html" and rng.random() < 0.4:
            s = rng.choice(TAGS)
        else:
            s = rng.choice(WORDS) + (" " if rng.random() < 0.9 else ". ")
        out.append(s)
        size += len(s)
    return "".join(out).encode()[:n]


def packets(rng, kind, count, sizes):
    """PPP packets: the protocol octet 0x21, a 40-octet header, payload."""
    res = []
    for i in range(count):
        k = kind if kind != "mixed" else rng.choice(["html", "text", "binary", "random", "zeros"])
        if sizes == "full":
            n = 1500
        elif sizes == "tiny":
            n = rng.randrange(1, 40)
        else:
            n = rng.randrange(40, 1501)
        if n >= 40:
            hdr = struct.pack(">BBHHHBBH4s4sHHIIHHHH", 0x45, 0, n, i & 0xffff, 0x4000, 62, 6, rng.randrange(65536),
                              bytes([192, 0, 2, 9]), bytes([10, 64, 64, 2]), 80, 1057, (i * 1460) & 0xffffffff, 0x5a103301,
                              0x5010, 0x16d0, rng.randrange(65536), 0)
            res.append(b"\x21" + hdr + payload(rng, k, n - 40))
        else:
            res.append(b"\x21" + payload(rng, k, n))
    return res


def run(tool, args, records):
    data = b"".join(struct.pack("<BH", kind, len(p)) + p for kind, p in records)
    out = subprocess.run(tool + args, input=data, stdout=subprocess.PIPE, check=True).stdout
    res, at = [], 0
    while at < len(out):
        kind, n = struct.unpack_from("<BH", out, at)
        res.append((kind, out[at + 3:at + 3 + n]))
        at += 3 + n
    return res


def stored(p):
    return b"\x00" + struct.pack("<HH", len(p), len(p) ^ 0xffff) + p


fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        if fails <= 20:
            print("  FAIL " + what)


def ours_to_zlib(tool, w, kind, count, sizes, seed, reset_at=None):
    rng = random.Random(seed)
    pk = packets(rng, kind, count, sizes)
    recs = []
    for i, p in enumerate(pk):
        if reset_at is not None and i == reset_at:
            recs.append((2, b""))
        recs.append((0, p))
    res = run(tool, ["dc", str(w)], recs)
    check(len(res) == len(pk), "record count")
    z = zlib.decompressobj(-w)
    nin = nout = native = 0
    for i, (p, (kind_out, c)) in enumerate(zip(pk, res)):
        if reset_at is not None and i == reset_at:
            z = zlib.decompressobj(-w)
        nin += len(p)
        try:
            if kind_out == 0:
                check(len(c) + 2 < len(p), "w %d %s packet %d: no gain but sent compressed" % (w, kind, i))
                got = z.decompress(c + TAIL)
                nout += len(c) + 2
            else:
                got = z.decompress(stored(p))
                native += 1
                nout += len(p)
        except zlib.error as e:
            check(False, "w %d %s packet %d: zlib says %s" % (w, kind, i, e))
            return nin, nout, native
        check(got == p, "w %d %s packet %d: zlib decodes something else" % (w, kind, i))
        if got != p:
            return nin, nout, native
    return nin, nout, native


def zlib_to_ours(tool, w, level, strategy, kind, count, sizes, seed):
    rng = random.Random(seed)
    pk = packets(rng, kind, count, sizes)
    z = zlib.compressobj(level, zlib.DEFLATED, -w, 8, strategy)
    recs = []
    for i, p in enumerate(pk):
        c = z.compress(p) + z.flush(zlib.Z_SYNC_FLUSH)
        check(c[-4:] == TAIL, "zlib's own tail")
        # every seventh packet in its native form, as a peer sends what does not shrink
        if i % 7 == 3:
            recs.append((1, p))
        else:
            recs.append((0, c[:-4]))
    res = run(tool, ["dd"], recs)
    want = [p for i, p in enumerate(pk) if i % 7 != 3]
    check(len(res) == len(want), "record count %d, want %d" % (len(res), len(want)))
    for i, (p, (kind_out, got)) in enumerate(zip(want, res)):
        check(kind_out == 0 and got == p, "zlib w %d level %d strategy %d %s: packet %d comes out wrong" % (w, level, strategy, kind, i))
        if not (kind_out == 0 and got == p):
            return


def main():
    args = sys.argv[1:]
    quick = False
    if args and args[0] == "-q":
        quick = True
        args = args[1:]
    if not args:
        print(__doc__)
        return 2
    tool = args
    n = 40 if quick else 400
    for kind in ("html", "text", "binary", "random", "zeros", "mixed"):
        nin, nout, native = ours_to_zlib(tool, 15, kind, n, "full", 1)
        print("  ours -> zlib, w 15, %-6s %d packets: %d -> %d octets, %d native" % (kind, n, nin, nout, native))
    for w in range(8, 16):
        nin, nout, native = ours_to_zlib(tool, w, "mixed", n * 2, "any", 100 + w)
        ours_to_zlib(tool, w, "html", n, "tiny", 200 + w)
        ours_to_zlib(tool, w, "text", n, "full", 300 + w, reset_at=n // 2)
        print("  ours -> zlib, w %2d, mixed sizes and kinds: %d -> %d octets, %d native" % (w, nin, nout, native))
    if not quick:
        # the window and the 16-bit positions wrap many times
        nin, nout, native = ours_to_zlib(tool, 15, "mixed", 20000, "any", 7)
        print("  ours -> zlib, w 15, 20000 packets: %d -> %d octets" % (nin, nout))
    for w in range(9, 16):      # zlib's deflate does not make a 256-octet window
        for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY), (1, zlib.Z_DEFAULT_STRATEGY), (6, zlib.Z_DEFAULT_STRATEGY),
                                (9, zlib.Z_DEFAULT_STRATEGY), (9, zlib.Z_FIXED), (6, zlib.Z_HUFFMAN_ONLY), (6, zlib.Z_RLE)):
            for kind in ("mixed", "html"):
                zlib_to_ours(tool, w, level, strategy, kind, n // 2, "any", 1000 + w + level)
            zlib_to_ours(tool, w, level, strategy, "text", n // 4, "tiny", 2000 + w)
    if not quick:
        zlib_to_ours(tool, 15, 6, zlib.Z_DEFAULT_STRATEGY, "mixed", 20000, "any", 9)
    print("zlib_check: %d checks, %d failures" % (checks, fails))
    return 1 if fails else 0


sys.exit(main())
