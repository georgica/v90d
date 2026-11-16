#!/usr/bin/env python3
"""spec.py RECORDING T0 T1 [STEP]: strongest spectral peaks heard (rx) and sent (tx) per step."""
import sys, math, cmath
d = open(sys.argv[1], "rb").read(); t0 = float(sys.argv[2]); t1 = float(sys.argv[3]); step = float(sys.argv[4]) if len(sys.argv) > 4 else 0.1
def ulaw(u):
    u = ~u & 0xFF; s = u & 0x80; e = (u >> 4) & 7; m = u & 15
    v = (((m << 3) + 0x84) << e) - 0x84
    return -v if s else v
T = [ulaw(i) for i in range(256)]
def get(off, i0, n): return [T[d[(i // 80) * 160 + off + i % 80]] for i in range(i0, i0 + n)]
N = 400  # 20 Hz bins
W = [0.5 - 0.5 * math.cos(2 * math.pi * k / N) for k in range(N)]
E = [[cmath.exp(-2j * math.pi * b * k / N) for k in range(N)] for b in range(N // 2)]
def peaks(x):
    x = [a * w for a, w in zip(x, W)]
    P = [abs(sum(a * e for a, e in zip(x, E[b]))) ** 2 for b in range(N // 2)]
    tot = sum(P) + 1e-9; out = []
    for b in sorted(range(2, N // 2 - 1), key=lambda b: -P[b])[:40]:
        if P[b] >= P[b - 1] and P[b] >= P[b + 1] and (P[b - 1] + P[b] + P[b + 1]) / tot > 0.04: out.append((b * 20, 100 * (P[b - 1] + P[b] + P[b + 1]) / tot))
    return " ".join("%d(%d%%)" % (f, p) for f, p in sorted(out)[:8]), 10 * math.log10(tot / N / N + 1e-9)
t = t0
while t < t1:
    i = int(t * 8000); r, rl = peaks(get(0, i, N)); x, xl = peaks(get(80, i, N))
    print("%7.3f rx %5.1f dB %-44s | tx %5.1f dB %s" % (t, rl, r, xl, x)); t += step
