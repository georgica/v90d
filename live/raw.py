#!/usr/bin/env python3
"""raw.py OUT [AT-setup ...]: dial, and after CONNECT keep what the modem hands over for SECS seconds."""
import os, sys, termios, time, select
out = sys.argv[1]; setup = sys.argv[2:]; SECS = float(os.environ.get("SECS", "8"))
fd = os.open("/dev/ttyACM0", os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[0] = 0; a[1] = 0; a[3] = 0
a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL | termios.HUPCL; a[4] = a[5] = termios.B115200
termios.tcsetattr(fd, termios.TCSANOW, a)
def rd(limit, stop=None):
    buf = b""; t0 = time.time()
    while time.time() - t0 < limit:
        if select.select([fd], [], [], 0.1)[0]:
            try: buf += os.read(fd, 4096)
            except BlockingIOError: pass
            if stop and any(s in buf for s in stop) and buf.endswith(b"\r\n"): break
    return buf
for c in ["ATZ", "ATE0", "ATX4"] + setup:
    os.write(fd, c.encode() + b"\r"); r = rd(3, [b"OK", b"ERROR"]); print(c, "->", r.decode("latin1").strip().replace("\r\n", " "))
time.sleep(1); os.write(fd, b"ATDT" + os.environ.get("NUM", "5551234").encode() + b"\r")
t0 = time.time(); r = rd(80, [b"CONNECT", b"NO CARRIER", b"BUSY", b"NO DIAL", b"ERROR"])
print("dial result after %.1f s: %s" % (time.time() - t0, r.decode("latin1").strip().replace("\r\n", " | ")))
if b"CONNECT" in r:
    send = os.environ.get("SEND")
    if send: os.write(fd, send.encode() * 20)
    data = rd(SECS); open(out, "wb").write(data)
    print("%d octets in %.0f s; first 96: %s" % (len(data), SECS, data[:96].hex(" ")))
    time.sleep(1.2); os.write(fd, b"+++"); time.sleep(1.5); os.write(fd, b"ATH0\r"); rd(3, [b"OK"])
os.write(fd, b"AT#UD\r"); d = rd(4, [b"OK\r\n"]).decode("latin1")
print(" ".join(x for x in d.split() if x.startswith(("20=", "21=", "26=", "27=", "34=", "35=", "40=", "42=", "60="))))
os.close(fd)
