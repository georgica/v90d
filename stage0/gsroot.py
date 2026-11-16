#!/usr/bin/env python3
"""Drive the Grandstream restricted SSH shell to a root shell via gssu, run a
command, print its output. Stdlib only (pty + select).

usage: gsroot.py HOST PASSWORD 'shell commands...'   # runs as root, prints output
       gsroot.py HOST PASSWORD --probe               # just show the login + gssu dialog
Env: GSROOT_TIMEOUT (seconds, default 30).
"""
import os, pty, select, sys, time, hashlib, binascii

def gsresponse(password, challenge):
    key = challenge + ":GrandstreamTX2013lZpRsFzCbM:" + password
    return binascii.hexlify(hashlib.md5(key.encode()).digest()[:8]).decode()

def main():
    if len(sys.argv) < 4:
        print(__doc__); return 2
    host, pw, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    probe = cmd == "--probe"
    timeout = float(os.environ.get("GSROOT_TIMEOUT", "30"))
    argv = ["sshpass", "-p", pw, "ssh", "-tt",
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "PreferredAuthentications=password", "-o", "PubkeyAuthentication=no",
            "-o", "ConnectTimeout=8", "admin@" + host]
    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("sshpass", argv)
        os._exit(127)

    buf = b""
    transcript = b""
    state = "login"          # login -> sent_gssu -> sent_resp -> root -> done
    begin, end = b"__V90B__", b"__V90E__"
    deadline = time.time() + timeout
    out = b""

    def send(s):
        os.write(fd, s if isinstance(s, bytes) else s.encode())

    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 1.0)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            transcript += chunk
            deadline = time.time() + timeout

        if probe:
            if b"GS>" in buf and b"gssu" not in transcript:
                send("gssu\n"); buf = b""
            continue

        if state == "login" and b"GS>" in buf:
            send("gssu\n"); buf = b""; state = "sent_gssu"
        elif state == "sent_gssu" and b"Challenge:" in buf:
            # Challenge: <hex>
            after = buf.split(b"Challenge:", 1)[1]
            tok = after.strip().split()[0].decode(errors="replace") if after.strip().split() else ""
            if len(tok) >= 8:
                send(gsresponse(pw, tok) + "\n"); buf = b""; state = "sent_resp"
        elif state == "sent_resp" and (b"\n# " in buf or buf.strip().endswith(b"#")):
            send(b"echo " + begin + b"; " + cmd.encode() + b"; echo " + end + b"\n")
            buf = b""; state = "root"
        elif state == "root" and end in buf:
            out = transcript.split(begin, 1)[1].split(end, 1)[0] if begin in transcript else b""
            send("exit\n"); time.sleep(0.2); send("exit\n")
            state = "done"
            break

    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except OSError:
        pass

    if probe or state != "done":
        sys.stderr.write("---- transcript ----\n")
        sys.stderr.write(transcript.decode(errors="replace"))
        sys.stderr.write("\n---- state=%s ----\n" % state)
        if state != "done" and not probe:
            return 1
        return 0
    sys.stdout.write(out.decode(errors="replace").replace("\r\n", "\n").strip("\n") + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
