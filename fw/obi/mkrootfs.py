#!/usr/bin/env python3
"""Build v90d's root file system for the Obihai OBi200: the flash part the
maker's root file system had (type 4, at 0x480000, 0x6C0000 octets; /scratch follows), as a
squashfs 4.0 with gzip that both kernels read (the maker's 2.6.30, which
U-Boot boots, and v90d's own 6.6, whose oinit mounts it).

    mkrootfs.py BASE.fw OUT.sqsh --busybox BB [--links BB.links] [--add SRC:DEST ...]

What is in it:
  - our static BusyBox, with a link for each of its applets;
  - our start files (fw/obi/rootfs: inittab, rcS, passwd, group);
  - the fixed device nodes the maker's system has, under /lib/dev (rcS copies
    them to /dev; mdev makes the rest);
  - from the owner's own firmware file BASE, as the modules are taken into
    /obi (mkobifw.py): the maker's swcfg and unicfg (they make the LAN port
    under the maker's kernel) and iptables with its extensions (NAT under the
    maker's kernel), with the libraries they need. v90d ships none of them;
  - whatever --add names: v90d, obiline and obinv (the daemon and its line
    and flash tools: /obi keeps only the start, the kernel and the lists), the
    archive gateway, OpenVPN.
Under v90d's own kernel only BusyBox and the --add programs are used.
Needs mksquashfs and unsquashfs (squashfs-tools 4.x) and readelf."""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkobifw  # noqa: E402

ROOTFS_TYPE = 4
ROOTFS_PART = 0x6C0000          # 0x480000 .. 0xB40000; /scratch (the settings) follows at 0xB40000
HERE = os.path.dirname(os.path.abspath(__file__))
FROM_BASE = ("bin/swcfg", "bin/unicfg", "usr/sbin/iptables")
FROM_BASE_DIRS = ("usr/libexec/xtables",)
LIB_DIRS = ("lib", "usr/lib")
DIRS = ("bin", "sbin", "usr/bin", "usr/sbin", "lib", "usr/lib", "etc/init.d", "proc", "sys", "var",
        "dev", "obi", "scratch", "root", "mnt")


def needed(path):
    out = subprocess.run(["readelf", "-d", path], capture_output=True, text=True).stdout
    return re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", out)


def interp(path):
    out = subprocess.run(["readelf", "-l", path], capture_output=True, text=True).stdout
    m = re.search(r"Requesting program interpreter: (/[^\]]+)\]", out)
    return m.group(1) if m else None


def copy_rel(src_root, dst_root, rel):
    """copy src_root/rel to dst_root/rel; a link is copied as a link and what it points to as well"""
    s, d = os.path.join(src_root, rel), os.path.join(dst_root, rel)
    if os.path.lexists(d):
        return
    os.makedirs(os.path.dirname(d), exist_ok=True)
    if os.path.islink(s):
        target = os.readlink(s)
        os.symlink(target, d)
        nxt = os.path.normpath(os.path.join(os.path.dirname(rel), target)) if not target.startswith("/") else target.lstrip("/")
        copy_rel(src_root, dst_root, nxt)
    elif os.path.isdir(s):
        os.makedirs(d, exist_ok=True)
    else:
        shutil.copy2(s, d)


def find_lib(src_root, name):
    for ld in LIB_DIRS:
        if os.path.lexists(os.path.join(src_root, ld, name)):
            return os.path.join(ld, name)
    sys.exit("BASE's root file system has no %s" % name)


def take_with_libs(src_root, dst_root, rels):
    todo, seen = list(rels), set()
    while todo:
        rel = todo.pop()
        if rel in seen:
            continue
        seen.add(rel)
        if not os.path.lexists(os.path.join(src_root, rel)):
            sys.exit("BASE's root file system has no /%s" % rel)
        copy_rel(src_root, dst_root, rel)
        real = os.path.realpath(os.path.join(src_root, rel))
        if not os.path.isfile(real) or open(real, "rb").read(4) != b"\x7fELF":
            continue
        for n in needed(real):
            todo.append(find_lib(src_root, n))
        i = interp(real)
        if i:
            todo.append(i.lstrip("/"))


def dev_nodes(base_img):
    """the fixed nodes and links of the maker's /dev, as mksquashfs pseudo lines under lib/dev"""
    out = subprocess.run(["unsquashfs", "-lls", base_img], capture_output=True, text=True).stdout
    pseudo, links = [], []
    for line in out.splitlines():
        m = re.match(r"([cb])[-rwx]{9} \S+\s+(\d+),\s*(\d+) \S+ \S+ squashfs-root/dev/(\S+)$", line)
        if m:
            pseudo.append("lib/dev/%s %s 600 0 0 %s %s" % (m.group(4), m.group(1), m.group(2), m.group(3)))
            continue
        m = re.match(r"l[-rwx]{9} \S+\s+\d+ \S+ \S+ squashfs-root/dev/(\S+) -> (\S+)$", line)
        if m:
            links.append((m.group(1), m.group(2)))
    if not pseudo:
        sys.exit("BASE's root file system lists no device nodes")
    return pseudo, links


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("base")
    ap.add_argument("out")
    ap.add_argument("--busybox", required=True)
    ap.add_argument("--links", help="busybox.links (default: next to the busybox binary)")
    ap.add_argument("--add", action="append", default=[], metavar="SRC:DEST")
    a = ap.parse_args()

    base = open(a.base, "rb").read()
    rootfs = [p for t, _, p in [(t, f, s[0x40:]) for t, f, s in mkobifw.segments(base)] if t == ROOTFS_TYPE]
    if len(rootfs) != 1:
        sys.exit("BASE has no root file system part")
    with tempfile.TemporaryDirectory() as tmp:
        img = os.path.join(tmp, "base.sqsh")
        open(img, "wb").write(rootfs[0])
        src = os.path.join(tmp, "base")
        # as an ordinary user the device nodes cannot be made: they come from the listing instead
        subprocess.run(["unsquashfs", "-q", "-n", "-d", src, img], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not os.path.isfile(os.path.join(src, "bin", "swcfg")):
            sys.exit("BASE's root file system did not unpack")
        root = os.path.join(tmp, "root")
        for d in DIRS:
            os.makedirs(os.path.join(root, d), exist_ok=True)
        os.symlink("var/tmp", os.path.join(root, "tmp"))
        os.symlink("../var/tmp/resolv.conf", os.path.join(root, "etc", "resolv.conf"))
        open(os.path.join(root, "scratch", "not_mounted"), "w").close()

        # BusyBox and its applets
        shutil.copy2(a.busybox, os.path.join(root, "bin", "busybox"))
        links = a.links or os.path.join(os.path.dirname(a.busybox), "busybox.links")
        for l in open(links).read().split():
            rel = l.lstrip("/")
            if rel == "bin/busybox" or os.path.lexists(os.path.join(root, rel)):
                continue
            os.makedirs(os.path.dirname(os.path.join(root, rel)), exist_ok=True)
            os.symlink(os.path.relpath("bin/busybox", os.path.dirname(rel)), os.path.join(root, rel))
        if not os.path.lexists(os.path.join(root, "linuxrc")):
            os.symlink("bin/busybox", os.path.join(root, "linuxrc"))

        # ours
        for rel in ("etc/inittab", "etc/init.d/rcS", "etc/passwd", "etc/group"):
            shutil.copy2(os.path.join(HERE, "rootfs", rel), os.path.join(root, rel))
        os.chmod(os.path.join(root, "etc/init.d/rcS"), 0o755)
        for spec in a.add:
            s, d = spec.split(":", 1)
            dst = os.path.join(root, d.lstrip("/"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(s, dst)
            os.chmod(dst, 0o755)

        # the maker's, from BASE
        take_with_libs(src, root, FROM_BASE)
        for d in FROM_BASE_DIRS:
            for f in sorted(os.listdir(os.path.join(src, d))):
                take_with_libs(src, root, [os.path.join(d, f)])

        # /dev
        pseudo, dlinks = dev_nodes(img)
        os.makedirs(os.path.join(root, "lib", "dev", "pts"), exist_ok=True)
        os.makedirs(os.path.join(root, "lib", "dev", "shm"), exist_ok=True)
        for name, target in dlinks:
            os.symlink(target, os.path.join(root, "lib", "dev", name))
        # and in /dev itself the two the kernel opens before anything runs
        # (the console for init: without it init has no console at all)
        pseudo += ["dev/console c 600 0 0 5 1", "dev/null c 666 0 0 1 3"]
        pf = os.path.join(tmp, "pseudo")
        open(pf, "w").write("\n".join(pseudo) + "\n")

        if os.path.exists(a.out):
            os.unlink(a.out)
        subprocess.run(["mksquashfs", root, a.out, "-comp", "gzip", "-b", "131072", "-all-root", "-no-xattrs",
                        "-noappend", "-quiet", "-no-progress", "-pf", pf], check=True, stdout=subprocess.DEVNULL)
    n = os.path.getsize(a.out)
    if n > ROOTFS_PART:
        sys.exit("%s: %d octets, more than the %d of the partition" % (a.out, n, ROOTFS_PART))
    print("%s: %d of %d octets (%d%%)" % (a.out, n, ROOTFS_PART, n * 100 // ROOTFS_PART))


if __name__ == "__main__":
    main()
