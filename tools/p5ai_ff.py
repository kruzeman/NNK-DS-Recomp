#!/usr/bin/env python3
"""Phase-5AI probe helper: report the 0x80-filled regions of a guest RAM dump.

The abort object 0x022CC8CC sits in a stretch of main RAM whose unwritten
bytes are 0x80 — this prints where such runs are, so the fill can be
attributed (heap block from the guest's own allocator vs. an emulation write).
usage: p5ai_ff.py <dump.bin> [base=0x02000000] [lo] [hi]
"""
import sys

path = sys.argv[1]
base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x02000000
lo = (int(sys.argv[3], 0) - base) if len(sys.argv) > 3 else 0
hi = (int(sys.argv[4], 0) - base) if len(sys.argv) > 4 else None

data = open(path, 'rb').read()
if hi is None:
    hi = len(data)
data = data[lo:hi]

runs = []
i = 0
n = len(data)
while i < n:
    if data[i] == 0x80:
        j = i
        while j < n and data[j] == 0x80:
            j += 1
        if j - i >= 16:
            runs.append((base + lo + i, j - i))
        i = j
    else:
        i += 1

print("%d runs of >=16 x 0x80 bytes in 0x%08X..0x%08X" %
      (len(runs), base + lo, base + hi))
for a, l in runs[:80]:
    print("  0x%08X len 0x%X (%d)" % (a, l, l))
