#!/usr/bin/env python3
"""Phase-5AL: translate a runtime input script (tools/*.nnk, "frame mask") into
a melonDS GXLog press schedule ("unit:keys:hexmask;...").

Checkout 5Z scripts are already melonDS key masks (active-low 0x3xx), so the
translation is 1:1 apart from the frame->unit offset (default +1: melonDS's
GXLog unit counter increments after the first completed frame).

usage: nnk_to_melonds.py <script.nnk> [offset]
"""
import sys

src = sys.argv[1]
off = int(sys.argv[2]) if len(sys.argv) > 2 else 1
out = []
for line in open(src):
    line = line.strip()
    if not line or line.startswith('#') or line == 'end':
        continue
    f, mask = line.split()
    out.append("%d:keys:%s" % (int(f) + off, mask.replace('0x', '')))
print(';'.join(out))
