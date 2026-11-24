#!/usr/bin/env python3
"""Tests of obinv (the OBi200's flash tool) and mkobifw.py on the host.
(V90D.fw is the whole image: our /obi and our root file system.)

    test_obinv.py OBINV BASE.fw V90D.fw

OBINV is obinv built for the host; BASE.fw the OBi firmware file the v90d file
V90D.fw was built from. A 16 MiB file stands for the flash, laid out as a box
on which BASE was installed: each part at its place, BASE's header at
0x470000, the rest erased. Then: the v90d file writes /obi only; writing BASE
back gives that same flash, octet for octet; files obinv must refuse are
refused and leave the flash alone.
"""
import hashlib
import importlib.util
import os
import struct
import subprocess
import sys
import tempfile

OBINV, BASE, V90D = sys.argv[1:4]
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("mkobifw", os.path.join(here, "mkobifw.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


def flash_after(fw):
    fl = bytearray(b"\xff" * 0x1000000)
    for t, at, seg in mk.segments(fw):
        fl[at:at + len(seg) - 0x40] = seg[0x40:]
    fl[0x470000:0x470400] = fw[:0x400]
    return fl


def run(tmp, flash, args, stdin=None):
    env = dict(os.environ, OBINV_FLASH=flash, OBINV_NO_REBOOT="1", OBINV_LOCK=os.path.join(tmp, "lock"),
               V90NV_STATE=os.path.join(tmp, "state"), V90NV_RESTART_NOTE=os.path.join(tmp, "restart"))
    p = subprocess.run([OBINV] + args, env=env, input=stdin, capture_output=True)
    return p.returncode, p.stdout.decode(errors="replace")


def upload(tmp, flash, data):
    return run(tmp, flash, ["serve-upgrade", str(len(data))], data)


base = open(BASE, "rb").read()
v90d = open(V90D, "rb").read()
with tempfile.TemporaryDirectory() as tmp:
    flash = os.path.join(tmp, "flash")
    fresh = flash_after(base)
    open(flash, "wb").write(fresh)

    r, out = run(tmp, flash, ["where"])
    check(r == 0 and "scheme=obi" in out and "image_name=" in out, "where: %r" % out)

    r, out = run(tmp, flash, ["check", V90D])
    # the v90d file carries our /obi and our root file system
    check(r == 0 and "would write: /obi, root file system\n" in out, "check v90d: %r" % out)
    r, out = run(tmp, flash, ["check", BASE])
    check(r == 0 and "nothing differs" in out, "check base on its own flash: %r" % out)

    # install v90d: the root file system, /obi and the header only
    r, out = upload(tmp, flash, v90d)
    check(r == 0 and out.startswith("HTTP/1.0 200"), "upload v90d: %d %r" % (r, out[:60]))
    check(os.path.exists(os.path.join(tmp, "restart")), "restart note left")
    now = open(flash, "rb").read()
    for t, at, seg in mk.segments(v90d):
        check(now[at:at + len(seg) - 0x40] == seg[0x40:], "part type %d in the flash" % t)
    changed = [a for a in range(0, len(now), 0x10000) if now[a:a + 0x10000] != fresh[a:a + 0x10000]]
    check(all(a == 0x470000 or 0x480000 <= a < 0xE40000 for a in changed),
          "only the root file system, /obi and the header changed: %s" % [hex(a) for a in changed if not 0x480000 <= a < 0xE40000])
    check(any(0x480000 <= a < 0xB40000 for a in changed), "the root file system was written")
    check(not any(0xB40000 <= a < 0xC00000 for a in changed), "/scratch (the settings) untouched")
    hdr = bytearray(now[0x470000:0x470400]); want = bytearray(v90d[:0x400])
    for i in range(struct.unpack_from("<I", v90d, 0x80)[0]):
        hdr[0xC0 + i * 0x40 + 2] = want[0xC0 + i * 0x40 + 2] = 0
    check(hdr == want, "the header at 0x470000 is the file's")
    check(now[0x470400:0x480000] == fresh[0x470400:0x480000], "the rest of the header's sector kept")

    # back to BASE: the flash as it was, octet for octet (the written marks aside)
    r, out = upload(tmp, flash, base)
    back = bytearray(open(flash, "rb").read())
    for i in range(struct.unpack_from("<I", base, 0x80)[0]):
        back[0x470000 + 0xC0 + i * 0x40 + 2] = fresh[0x470000 + 0xC0 + i * 0x40 + 2]
    check(r == 0 and back == fresh, "back to BASE restores the flash")

    # refused, and nothing written
    def refused(data, what):
        before = open(flash, "rb").read()
        r, out = upload(tmp, flash, data)
        check(r == 1 and out.startswith("HTTP/1.0 4"), "%s refused: %d %r" % (what, r, out[:80]))
        check(open(flash, "rb").read() == before, "%s: flash untouched" % what)

    refused(b"not a firmware file" * 100, "garbage")
    refused(v90d[:-1000], "a cut file")
    bad = bytearray(v90d); bad[-5] ^= 0xff
    refused(bytes(bad), "a damaged part")
    key = None
    one = None
    with tempfile.TemporaryDirectory() as x:
        sq = os.path.join(x, "s")
        open(sq, "wb").write([s for s in mk.segments(base) if s[0] == 3][0][2][0x40:])
        subprocess.run(["unsquashfs", "-q", "-n", "-d", os.path.join(x, "o"), sq], check=True, stdout=subprocess.DEVNULL)
        key = mk.find_key(open(os.path.join(x, "o", "obiapp"), "rb").read(), base)
    segs = mk.segments(v90d)
    one = mk.pack(key, base, [(t, at, seg[0x40:]) for t, at, seg in segs if t == 3], "x")
    refused(one, "a one-part file")
    uboot = [(t, at, (seg[0x40:-1] + b"\x00") if t == 2 else seg[0x40:]) for t, at, seg in segs]
    refused(mk.pack(key, base, uboot, "x"), "a different boot loader")
    dup = [(t, at, seg[0x40:]) for t, at, seg in segs] + [(3, 0xC00000, segs[[s[0] for s in segs].index(3)][2][0x40:])]
    try:
        refused(mk.pack(key, base, dup, "x"), "two /obi parts")
    except AssertionError:
        check(True, "the builder refuses an eighth part")

# v90d with its own root file system (mkrootfs.py), when given: the root
# file system, /obi and the header are written, nothing else; BASE takes the
# box back, octet for octet
if len(sys.argv) > 4:
    own = open(sys.argv[4], "rb").read()
    with tempfile.TemporaryDirectory() as tmp:
        flash = os.path.join(tmp, "flash")
        fresh = flash_after(base)
        open(flash, "wb").write(fresh)
        r, out = run(tmp, flash, ["check", sys.argv[4]])
        check(r == 0 and "root file system" in out and "/obi" in out, "check own root file system: %r" % out)
        r, out = upload(tmp, flash, own)
        check(r == 0 and out.startswith("HTTP/1.0 200"), "upload own root file system: %d %r" % (r, out[:60]))
        now = open(flash, "rb").read()
        for t, at, seg in mk.segments(own):
            check(now[at:at + len(seg) - 0x40] == seg[0x40:], "part type %d in the flash (own root file system)" % t)
        changed = [a for a in range(0, len(now), 0x10000) if now[a:a + 0x10000] != fresh[a:a + 0x10000]]
        check(all(a == 0x470000 or 0x480000 <= a < 0xE40000 for a in changed),
              "only the root file system, /obi and the header changed: %s" % [hex(a) for a in changed if not 0x480000 <= a < 0xE40000])
        check(any(0x480000 <= a < 0xB40000 for a in changed), "the root file system was written")
        check(not any(0xB40000 <= a < 0xC00000 for a in changed), "/scratch (the settings) untouched")
        r, out = upload(tmp, flash, base)
        back = bytearray(open(flash, "rb").read())
        for i in range(struct.unpack_from("<I", base, 0x80)[0]):
            back[0x470000 + 0xC0 + i * 0x40 + 2] = fresh[0x470000 + 0xC0 + i * 0x40 + 2]
        check(r == 0 and back == fresh, "back to BASE from the own root file system restores the flash")

print("obinv: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
