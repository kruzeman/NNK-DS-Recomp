#!/usr/bin/env python3
"""Annotate a pc trace: stride (2 = Thumb-ish, 4 = ARM-ish) and non-sequential jumps."""
import sys

path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 200
start_pc = int(sys.argv[3], 16) if len(sys.argv) > 3 else None

pcs = [int(l.strip(), 16) for l in open(path) if l.startswith("0x")]
if start_pc is not None:
    try:
        pcs = pcs[pcs.index(start_pc):]
    except ValueError:
        print("start pc 0x%08X not in trace" % start_pc)
        sys.exit(1)
print("total pcs=%d first=0x%08X last=0x%08X" % (len(pcs), pcs[0], pcs[-1]))
win = pcs[-limit:]
base = len(pcs) - len(win)
prev = None
for i, pc in enumerate(win):
    tag = ""
    if prev is not None:
        d = pc - prev
        if d == 2:
            tag = "(+2)"
        elif d == 4:
            tag = "(+4)"
        else:
            tag = "JUMP %+d" % d
    print("%4d  0x%08X  %s" % (base + i, pc, tag))
    prev = pc
