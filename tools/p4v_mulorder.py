#!/usr/bin/env python3
"""Phase-4V: which long-multiply (UMULL/SMULL/UMLAL/SMLAL) case PCs appear in
the melonDS 30M-instruction PC trace, and in what order — to say honestly
whether the first measured divergence (pc 0x0201E814) is also the first
long-multiply execution."""
import re
import sys

image = sys.argv[1]
trace = sys.argv[2]
mnem = re.compile(r"/\* 0x([0-9A-Fa-f]{8}): (umull|smull|umlal|smlal)s? ")
pcs = {}
with open(image, encoding="utf-8", errors="replace") as f:
    for line in f:
        m = mnem.search(line)
        if m:
            pcs["0x" + m.group(1).lower()] = m.group(2)
print(f"long-multiply case pcs in image: {len(pcs)}")

first = {}
with open(trace, encoding="utf-8", errors="replace") as f:
    for i, line in enumerate(f, 1):
        pc = line.strip().lower()
        if pc in pcs and pc not in first:
            first[pc] = (i, pcs[pc])
        if len(first) == len(pcs):
            break
for pc, (idx, kind) in sorted(first.items(), key=lambda kv: kv[1][0]):
    print(f"first at trace line {idx:>9}  {pc}  {kind}")
print("pcs never executed in the trace window:",
      len(pcs) - len(first))
