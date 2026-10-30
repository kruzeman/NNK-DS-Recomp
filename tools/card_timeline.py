#!/usr/bin/env python3
"""Cycle-level card-transfer timing from the timestamped melonDS IO trace."""
import sys

path = sys.argv[1]
start_ln = int(sys.argv[2])
nlines = int(sys.argv[3]) if len(sys.argv) > 3 else 40

ln = 0
shown = 0
prev_ts = None
for line in open(path):
    ln += 1
    if ln < start_ln:
        continue
    p = line.split()
    rw, addr, val = p[0], p[1], p[2]
    ts = int(p[4].split('=')[1])
    d = (ts - prev_ts) if prev_ts is not None else 0
    prev_ts = ts
    print("%8d %-4s %s %s %s %-12s dts=%-8d" % (ln, rw, addr, val, p[3], p[3], d))
    shown += 1
    if shown >= nlines:
        break
