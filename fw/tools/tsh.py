#!/usr/bin/env python3
# minimal telnet exec against busybox telnetd -l /bin/sh on the HT802 root port
import socket, sys, time
import os
HOST, PORT = os.environ.get("TSH_HOST", "192.168.1.172"), int(os.environ.get("TSH_PORT", "2323"))
def strip_iac(d):
    o=bytearray(); i=0
    while i<len(d):
        if d[i]==255 and i+1<len(d):
            if d[i+1] in (251,252,253,254) and i+2<len(d): i+=3; continue
            i+=2; continue
        o.append(d[i]); i+=1
    return bytes(o)
def run(cmds, settle=3.0, timeout=2.0):
    s=socket.create_connection((HOST,PORT),10,source_address=(os.environ["TSH_SRC"],0) if os.environ.get("TSH_SRC") else None); s.settimeout(timeout)
    def pump():
        out=b""
        try:
            while True:
                d=s.recv(8192)
                if not d: break
                out+=strip_iac(d)
        except socket.timeout: pass
        return out
    time.sleep(0.8); pump()
    # suppress pty echo so large heredoc transfers don't bounce back
    s.sendall(b"stty -echo 2>/dev/null\n"); time.sleep(0.4); pump()
    s.sendall((cmds+"\necho __TSH_DONE__\n").encode())
    time.sleep(settle)
    data=pump()
    s.close()
    return data.decode('latin1')
if __name__=="__main__":
    settle=float(sys.argv[1]) if len(sys.argv)>1 else 3.0
    cmds=sys.stdin.read()
    sys.stdout.write(run(cmds, settle=settle))
