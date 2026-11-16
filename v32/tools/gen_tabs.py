#!/usr/bin/env python3
# Generates v32_tabs.c: the V.32bis signal space tables, read out of the
# Recommendation's own figures (word positions in the PDF), and the transmit
# pulse and receive filter bank. Run from v32/:  python3 tools/gen_tabs.py
# The output is checked in; a normal build does not need this.
import re, subprocess, math, sys

PDF = "../refs/itu/T-REC-V.32bis-199102-I.pdf"

def words(page):
    html = subprocess.run(["pdftotext", "-f", str(page), "-l", str(page), "-bbox", PDF, "-"],
                          capture_output=True, text=True, check=True).stdout
    out = []
    for x0, y0, x1, y1, w in re.findall(
            r'<word xMin="([\d.]+)" yMin="([\d.]+)" xMax="([\d.]+)" yMax="([\d.]+)">([^<]*)</word>', html):
        out.append(((float(x0) + float(x1)) / 2, (float(y0) + float(y1)) / 2, w))
    return out

def cluster(vals, tol):
    vals = sorted(vals); groups = [[vals[0]]]
    for v in vals[1:]:
        if v - groups[-1][-1] <= tol: groups[-1].append(v)
        else: groups.append([v])
    return [sum(g) / len(g) for g in groups]

def figure(page, nbits, ylo, yhi, xunit_cols):
    """points of one figure: labels of nbits binary digits between ylo and yhi
    (page coordinates). Rows are equally spaced in the signal space, one unit
    apart when rows are staggered (xunit_cols = True) or two apart otherwise."""
    pts = [(x, y, w) for x, y, w in words(page) if re.fullmatch("[01]{%d}" % nbits, w) and ylo < y < yhi]
    assert len(pts) == 1 << nbits, (page, nbits, len(pts))
    rows = cluster([p[1] for p in pts], 3.0)
    nr = len(rows)
    xs = cluster([p[0] for p in pts], 4.0)
    xc = (xs[0] + xs[-1]) / 2
    ystep = 1 if xunit_cols else 2
    # horizontal unit: columns are one unit apart when rows are staggered
    xu = (xs[-1] - xs[0]) / (len(xs) - 1) / (1 if xunit_cols else 2)
    tab = {}
    for x, y, w in pts:
        r = min(range(nr), key=lambda i: abs(rows[i] - y))
        py = ((nr - 1) / 2 - r) * ystep
        px = (x - xc) / xu
        ix, iy = round(px), round(py)
        assert abs(px - ix) < 0.25 and iy == py, (w, px, py)
        assert w not in tab
        tab[int(w, 2)] = (ix, iy)
    return [tab[i] for i in range(1 << nbits)]

c144 = figure(6, 7, 240, 560, True)
c120 = figure(7, 6, 130, 370, False)
c96  = figure(8, 5, 130, 340, True)
c72  = figure(8, 4, 530, 680, False)
# 9600, 7200 and 4800 are drawn at half the scale of 14400 and 12000 (the
# synchronising states A B C D sit at (-3,-1).. there and (-6,-2).. here)
c96 = [(2 * x, 2 * y) for x, y in c96]
c72 = [(2 * x, 2 * y) for x, y in c72]
c48 = [(-6, -2), (2, -6), (-2, 6), (6, 2)]      # Y1Y2 = 00 A, 01 B, 10 D, 11 C

# ---------------- filters ----------------
ALPHA = 0.25
def rrc(t, a=ALPHA):
    if abs(t) < 1e-9: return 1 - a + 4 * a / math.pi
    if abs(abs(t) - 1 / (4 * a)) < 1e-9:
        return a / math.sqrt(2) * ((1 + 2 / math.pi) * math.sin(math.pi / (4 * a)) + (1 - 2 / math.pi) * math.cos(math.pi / (4 * a)))
    return (math.sin(math.pi * t * (1 - a)) + 4 * a * t * math.cos(math.pi * t * (1 + a))) / (math.pi * t * (1 - (4 * a * t) ** 2))

SPS = 10.0 / 3            # samples per symbol
WC = 2 * math.pi * 1800 / 8000

# transmit pulse: TXL symbols long, 10 phases (thirds of a sample), complex
# pass-band (carrier folded in). Index [phase][symbol age].
import os
TXL = int(os.environ.get("TXL", 12))
BETA = float(os.environ.get("BETA", 2.0))
def i0(x):
    s = t = 1.0; k = 1
    while t > 1e-12 * s:
        t *= (x / (2 * k)) ** 2; s += t; k += 1
    return s
def win(u):               # Kaiser, u in -1..1
    return i0(BETA * math.sqrt(1 - u * u)) / i0(BETA) if abs(u) < 1 else 0.0
txh = []
for p in range(10):
    row = []
    for j in range(TXL):
        tau = p / 3.0 + j * SPS - TXL * SPS / 2      # samples from the pulse centre
        g = rrc(tau / SPS) * win(tau / (TXL * SPS / 2))
        ph = WC * (p / 3.0 + j * SPS)             # carrier phase counted from the symbol's start
        row.append((g * math.cos(ph), g * math.sin(ph)))
    txh.append(row)
# echo canceller reference pulse: the same symbols on a pulse whose spectrum
# is flat over the whole transmit band (300..3300 Hz) and falls off outside
# it (to nothing by -300 and 3900 Hz), so that the canceller's reference is
# white wherever the echo has power
ECF = float(os.environ.get("ECF", 1800.0))
ECB = float(os.environ.get("ECB", 4.5))
def kais(u, beta):
    return i0(beta * math.sqrt(1 - u * u)) / i0(beta) if abs(u) < 1 else 0.0
def sinc(x):
    return 1.0 if abs(x) < 1e-12 else math.sin(math.pi * x) / (math.pi * x)
ech = []
for p in range(10):
    row = []
    for j in range(TXL):
        tau = p / 3.0 + j * SPS - TXL * SPS / 2
        g = sinc(2 * ECF * tau / 8000) * kais(tau / (TXL * SPS / 2), ECB)
        ph = WC * (p / 3.0 + j * SPS)
        row.append((g * math.cos(ph), g * math.sin(ph)))
    ech.append(row)
epw = sum(c[0] ** 2 + c[1] ** 2 for row in ech for c in row)
tpw = sum(c[0] ** 2 + c[1] ** 2 for row in txh for c in row)
ECS = math.sqrt(tpw / epw)        # same RMS as the line signal
peak = max(abs(v) for row in txh for c in row for v in c)
TXS = 16384 / max(peak, rrc(0))
# line RMS for unit symbol gain and mean |symbol|^2 = 40: Re{} halves the power
pw = sum(c[0] ** 2 + c[1] ** 2 for row in txh for c in row) / 10 / 2 * 40
tx_rms_q12 = round(math.sqrt(pw) * TXS / 16384 * 4096)

# receive bank: RXP fractional delays, RXN taps, complex band-pass
RXP, RXN = 48, 33
D = (RXN - 1) / 2
rxg = []
norm = sum(rrc((i - D) / SPS) * win((i - D) / (D + 1)) for i in range(RXN))
for p in range(RXP):
    row = []
    for i in range(RXN):
        t = i - D - p / RXP
        g = rrc(t / SPS) * win(t / (D + 1)) / norm * 2     # x2: analytic signal of a real input
        row.append((g * math.cos(WC * t), g * math.sin(WC * t)))
    rxg.append(row)
RXS = 16384

o = []
o.append("/* GENERATED by tools/gen_tabs.py - do not edit.\n"
         " * Signal space tables: Figures 2-1 .. 2-5/V.32 bis, in the scale of Figure 2-1\n"
         " * (A = (-6,-2)); index = the figure's binary label (Y0 first). */")
o.append('#include "v32_priv.h"\n')
def ctab(name, tab):
    s = "const int8_t %s[%d][2] = {\n" % (name, len(tab))
    for i in range(0, len(tab), 8):
        s += "    " + " ".join("{%2d,%2d}," % t for t in tab[i:i + 8]) + "\n"
    return s + "};"
for name, tab in (("v32_c4800", c48), ("v32_c7200", c72), ("v32_c9600", c96), ("v32_c12000", c120), ("v32_c14400", c144)):
    o.append(ctab(name, tab))
o.append("\n/* transmit pulse: root raised cosine, %g %% roll-off, %d symbols, Q14, [phase][age][re,im] */" % (ALPHA * 100, TXL))
s = "const int16_t v32_txh[10][V32_TXL][2] = {\n"
for row in txh:
    s += "  {" + " ".join("{%d,%d}," % (round(c[0] * TXS), round(c[1] * TXS)) for c in row) + "},\n"
o.append(s + "};")
s = "/* echo canceller reference pulse, same scale */\nconst int16_t v32_ech[10][V32_TXL][2] = {\n"
for row in ech:
    s += "  {" + " ".join("{%d,%d}," % (round(c[0] * TXS * ECS), round(c[1] * TXS * ECS)) for c in row) + "},\n"
o.append(s + "};")
o.append("const int32_t v32_tx_rms_q12 = %d;   /* line RMS per unit symbol gain, Q12 */" % tx_rms_q12)
o.append("\n/* receive band-pass matched filter bank, Q14, [fractional delay][tap][re,im] */")
s = "const int16_t v32_rxg[V32_RXP][V32_RXN][2] = {\n"
for row in rxg:
    s += "  {" + " ".join("{%d,%d}," % (round(c[0] * RXS), round(c[1] * RXS)) for c in row) + "},\n"
o.append(s + "};")
open("v32_tabs.c", "w").write("\n".join(o) + "\n")
for n, t in (("4800", c48), ("7200", c72), ("9600", c96), ("12000", c120), ("14400", c144)):
    print(n, "mean power", sum(x * x + y * y for x, y in t) / len(t))
print("tx_rms_q12", tx_rms_q12)
