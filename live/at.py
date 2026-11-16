#!/usr/bin/env python3
"""Send AT commands to the test modem and print what comes back.
   at.py [-t SECONDS] 'ATI3' 'ATDT5551234' ...   (wait is per command; a
   dial waits until a result code or the timeout)"""
import os, sys, termios, time, select
DEV = os.environ.get("MODEM", "/dev/ttyACM0")
args = sys.argv[1:]; wait = 3.0
if args and args[0] == "-t": wait = float(args[1]); args = args[2:]
fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL
a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
END = (b"OK\r\n", b"ERROR\r\n", b"NO CARRIER\r\n", b"BUSY\r\n", b"NO DIALTONE\r\n", b"NO ANSWER\r\n", b"DELAYED\r\n")
def rd(limit, final):
    buf = b""; t0 = time.time()
    while time.time() - t0 < limit:
        if select.select([fd], [], [], 0.1)[0]:
            try: buf += os.read(fd, 4096)
            except BlockingIOError: pass
            if final and (buf.endswith(END) or (b"CONNECT" in buf and buf.endswith(b"\r\n"))): break
    return buf
rd(0.2, False)
for c in args:
    os.write(fd, c.encode() + b"\r")
    out = rd(wait, True).decode("latin1").replace("\r", "")
    print("\n".join(l for l in out.split("\n") if l.strip()))
    sys.stdout.flush()
os.close(fd)
