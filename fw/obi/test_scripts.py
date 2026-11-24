#!/usr/bin/env python3
"""The OBi200's start scripts against the programs the box has.

    test_scripts.py ROOTFS.sqsh BUSYBOX

Every command that /obi/obi, /obi/kinit, /obi/udhcpc.script and the root file
system's rcS run must be there: a shell builtin, a function of the script, a
file of /obi (mkobifw.py's OURS and the maker's modules), or a program of the
root file system (mkrootfs.py) on the scripts' PATH. And the commands whose
options differ between BusyBox versions (one already left a box without its
LAN: udhcpc's -H) are run with the scripts' own options under qemu-arm, on
an interface that does not exist, to see that the options are understood.
A first box would otherwise be the test."""
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkobifw  # noqa: E402

SCRIPTS = {"obi": os.path.join(HERE, "obi"), "kinit": os.path.join(HERE, "kinit"),
           "udhcpc.script": os.path.join(HERE, "udhcpc.script"),
           "rcS": os.path.join(HERE, "rootfs", "etc", "init.d", "rcS")}
PATH = ("sbin", "usr/sbin", "bin", "usr/bin")
BUILTINS = set("""
. : [ [[ alias bg break cd command continue echo eval exec exit export false fg getopts hash
jobs kill let local printf pwd read readonly return set shift test times trap true type ulimit
umask unalias unset wait if then else elif fi for while until do done case esac in function
""".split())
# run with the scripts' options (harmless: they act on a device or file that is not there)
OPTION_CHECKS = ("udhcpc", "hexdump", "logger")
# a line for the maker's older BusyBox only (a fallback after ||): not looked at
OLD_ONLY = "maker's BusyBox only"

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL:", what)


def rootfs_files(img):
    out = subprocess.run(["unsquashfs", "-lls", img], capture_output=True, text=True, check=True).stdout
    return {m.group(1) for m in re.finditer(r" squashfs-root/(\S+)", out)}


def commands(text):
    """(line number, command word, the rest of its simple command) for each simple command"""
    funcs = set(re.findall(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(\)", text, re.M))
    found = []
    lines, start = [], []
    for n, line in enumerate(text.split("\n"), 1):     # continued lines as one, numbered by the first
        if lines and lines[-1].endswith("\\"):
            lines[-1] = lines[-1][:-1] + " " + line
        else:
            lines.append(line); start.append(n)
    for n, line in zip(start, lines):
        if OLD_ONLY in line:
            continue
        code = re.sub(r"(^|\s)#.*$", "", line)
        code = re.sub(r"'[^']*'", "''", code)
        # a quoted string: only the commands of its $(...) are commands
        code = re.sub(r'"([^"]*)"', lambda m: " ; ".join("$(" + x + ")" for x in re.findall(r"\$\(([^)]*)\)", m.group(1))) or '""', code)
        code = re.sub(r"\$\{[^}]*\}", "x", code)
        code = re.sub(r"^\s*[^\s()]+\)", "", code)      # a case pattern
        # the starts of simple commands
        for part in re.split(r"\$\(|;;|&&|\|\||[;|&(){}]|\bthen\b|\bdo\b|\belse\b|\b!\s", code):
            words = part.split()
            while words and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", words[0]):
                words = words[1:]           # assignments before a command
            if not words:
                continue
            w = words[0]
            if w in BUILTINS or w in funcs or w.startswith(("$", '"', "'", ">", "<", "2>", "[")) or w.endswith(")"):
                continue
            if re.match(r"^[0-9]", w) or not re.match(r"^[A-Za-z_./]", w) or "*" in w:
                continue                    # a number, an operator, a pattern: no command
            found.append((n, w, " ".join(words[1:])))
    return found


def main():
    img, busybox = sys.argv[1:3]
    files = rootfs_files(img)
    obi_files = set(mkobifw.OURS) | set(mkobifw.KEEP_FROM_BASE)
    for name, path in SCRIPTS.items():
        for n, w, rest in commands(open(path).read()):
            where = "%s:%d: %s" % (name, n, w)
            if w.startswith(("$D/", "/obi/", "./")):
                check(os.path.basename(w) in obi_files, where + " is not a file of /obi")
            elif w.startswith("/"):
                check(w.lstrip("/") in files, where + " is not in the root file system")
            else:
                check(any("%s/%s" % (d, w) in files for d in PATH), where + " is not on the PATH of the root file system")

    qemu = shutil.which("qemu-arm-static") or shutil.which("qemu-arm")
    if not qemu:
        print("  (no qemu-arm: the option checks are left out)")
    else:
        for name, path in SCRIPTS.items():
            for n, w, rest in commands(open(path).read()):
                if w not in OPTION_CHECKS:
                    continue
                args = re.sub(r"\$\{?[A-Za-z_][A-Za-z0-9_]*\}?", "x", rest)
                args = re.split(r"\s(?:>|2>|<)", " " + args)[0].split()
                if w == "udhcpc":
                    args = [a if a != "x" else "nosuch0" for a in args] + ["-n", "-t", "1", "-T", "1", "-i", "nosuch0"]
                p = subprocess.run([qemu, busybox, w] + args, capture_output=True, text=True, timeout=30,
                                   stdin=subprocess.DEVNULL)
                said = p.stdout + p.stderr
                check(not re.search(r"invalid option|unrecognized option|Usage:", said),
                      "%s:%d: %s %s: %s" % (name, n, w, " ".join(args), said.strip().split("\n")[0]))
    print("scripts: %d checks, %d failures" % (checks, failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
