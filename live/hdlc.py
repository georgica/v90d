#!/usr/bin/env python3
"""hdlc.py BITFILE [MAX]: the HDLC frames in a bit dump (octets LSB first, FCS-16 checked)."""
import sys
b = open(sys.argv[1]).read().strip(); mx = int(sys.argv[2]) if len(sys.argv) > 2 else 12
def fcs(data):
    c = 0xFFFF
    for o in data:
        c ^= o
        for _ in range(8): c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c
i = 0; n = 0; ones = 0; cur = []; inframe = False; start = 0
frames = []
while i < len(b):
    bit = b[i] == '1'; i += 1
    if bit:
        ones += 1; cur.append(1); continue
    if ones == 6:      # flag
        body = cur[:-6 - 1] if len(cur) >= 7 else []
        if inframe and len(body) >= 32:
            oct = [sum(body[k + j] << j for j in range(8)) for k in range(0, len(body) - 7, 8)]
            frames.append((start, oct, len(body) % 8, fcs(oct) == 0xF0B8))
        cur = []; inframe = True; start = i
    elif ones == 5: pass            # stuffed zero
    elif ones > 6: cur = []; inframe = False
    else: cur.append(0)
    ones = 0
print(len(frames), "frames")
for s, o, rem, ok in frames[:mx]:
    print("bit %7d %s%s  %s" % (s, "FCS ok " if ok else "FCS BAD", "" if rem == 0 else " (+%d bits)" % rem, " ".join("%02x" % x for x in o)))
