#!/usr/bin/env python3
"""The bulletin board list in the image (lists/bbs.txt): Netstalgia's signed
list as it is today, checked against the keys the daemon is built with
(srv/bbskeys.h) before it is written.

  bbssnap.py                     fetch, check, write lists/bbs.txt
  bbssnap.py --file F            check F (no network) and write it
  bbssnap.py --set-keys HEX [HEX]  put Netstalgia's public keys (64 hex
                                 digits each) into srv/bbskeys.h

Needs Python's "cryptography" package (Ed25519)."""
import argparse, os, re, sys, urllib.request

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
KEYS = os.path.join(TOP, "srv", "bbskeys.h")
LIST = os.path.join(TOP, "lists", "bbs.txt")
URL = "http://api.netstalgia.online/v1/bbs.txt"
TAG = b"# sig ed25519 "
HEAD = """/* Netstalgia's public keys for the bulletin board list (bbssig.h), as the
 * rows of bbs_sig_keys; written by fw/tools/bbssnap.py --set-keys. All
 * zero: no key there. */
"""


def keys():
    rows = re.findall(r"\{([^}]*)\}", open(KEYS).read())
    out = []
    for r in rows:
        b = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", r))
        if len(b) == 32 and any(b):
            out.append(b)
    return out


def set_keys(hexes):
    rows = []
    for h in hexes + ["00" * 32] * (2 - len(hexes)):
        if not re.fullmatch(r"[0-9a-f]{64}", h):
            sys.exit("a key is 64 lowercase hex digits: %r" % h)
        b = bytes.fromhex(h)
        rows.append("{ " + ", ".join("0x%02x" % x for x in b[:16]) + ",\n  " + ", ".join("0x%02x" % x for x in b[16:]) + " },\n")
    open(KEYS, "w").write(HEAD + "".join(rows))


def check(data):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    body = data[:-1] if data.endswith(b"\n") else data
    start = body.rfind(b"\n") + 1
    last = body[start:]
    if not (last.startswith(TAG) and re.fullmatch(rb"[0-9a-f]{128}", last[len(TAG):])):
        sys.exit("the list has no signature line")
    sig = bytes.fromhex(last[len(TAG):].decode())
    ks = keys()
    if not ks:
        sys.exit("srv/bbskeys.h has no key: bbssnap.py --set-keys first")
    for k in ks:
        try:
            Ed25519PublicKey.from_public_bytes(k).verify(sig, data[:start])
            break
        except InvalidSignature:
            pass
    else:
        sys.exit("no key in srv/bbskeys.h signed this list")
    lines = [l for l in data[:start].split(b"\n") if l and not l.startswith(b"#")]
    if not lines or lines[0].rstrip(b"\r") != b"v90d-bbs 2":
        sys.exit("signed, but not a list")
    return sum(1 for l in lines if l.startswith(b"bbs "))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--file")
    ap.add_argument("--url", default=URL)
    ap.add_argument("--set-keys", nargs="+")
    a = ap.parse_args()
    if a.set_keys:
        set_keys(a.set_keys)
        print("srv/bbskeys.h: %d key(s)" % len(a.set_keys))
        return
    if a.file:
        data = open(a.file, "rb").read()
    else:
        req = urllib.request.Request(a.url, headers={"User-Agent": "Wget/1.21 (v90d bbssnap)"})
        data = urllib.request.urlopen(req, timeout=30).read(512 * 1024 + 1)
    if len(data) > 256 * 1024:
        sys.exit("the list is larger than a box takes")
    n = check(data)
    open(LIST, "wb").write(data)
    print("lists/bbs.txt: %d boards, signed" % n)


if __name__ == "__main__":
    main()
