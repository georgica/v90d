#!/usr/bin/env python3
"""Summary of a line recording made by `v90d -w`: per 200 ms the level heard
   and sent (dBm0) and the strongest tones (Goertzel on a list)."""
import sys, math
d = open(sys.argv[1], "rb").read()
step = float(sys.argv[2]) if len(sys.argv) > 2 else 0.2
t0 = float(sys.argv[3]) if len(sys.argv) > 3 else 0; t1 = float(sys.argv[4]) if len(sys.argv) > 4 else 1e9
def ulaw(u):
    u = ~u & 0xFF; s = u & 0x80; e = (u >> 4) & 7; m = u & 15
    v = ((m << 3) + 0x84) << e; v -= 0x84
    return -v if s else v
T = [ulaw(i) for i in range(256)]
rx = []; tx = []
for b in range(len(d) // 160):
    rx += [T[x] for x in d[b*160:b*160+80]]; tx += [T[x] for x in d[b*160+80:b*160+160]]
F = [350, 440, 480, 620, 697, 770, 852, 941, 980, 1180, 1209, 1336, 1477, 1200, 1300, 1650, 1850, 1800, 2100, 2225, 2400, 600, 3000]
def lvl(x):
    p = sum(v*v for v in x) / max(1, len(x))
    return 10*math.log10(p / (8031.0**2/2) + 1e-12) + 3.17
def tones(x):
    n = len(x); tot = sum(v*v for v in x) + 1e-9; out = []
    for f in F:
        w = 2*math.pi*f/8000; c = 2*math.cos(w); a = b = 0.0
        for v in x: a, b = v + c*a - b, a
        p = (a*a + b*b - c*a*b) * 2 / n
        if p / tot > 0.15: out.append("%d(%d%%)" % (f, 100*p/tot))
    return " ".join(out)
n = int(step*8000)
for i in range(0, len(rx) - n + 1, n):
    t = i/8000.0
    if t < t0 or t > t1: continue
    r = rx[i:i+n]; x = tx[i:i+n]
    print("%6.2f  rx %6.1f %-34s tx %6.1f %s" % (t, lvl(r), tones(r) if lvl(r) > -50 else "", lvl(x), tones(x) if lvl(x) > -50 else ""))
