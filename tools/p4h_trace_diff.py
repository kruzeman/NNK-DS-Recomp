#!/usr/bin/env python3
"""Robust (insertion/deletion-tolerant) comparison of two PC-trace files.

The melonDS ARM9 trace logs the second halfword of a 32-bit Thumb instruction
as a ghost PC (start+2), so a strict prefix compare reports false divergences.
SequenceMatcher reports only blocks that cannot be explained by insertions.

usage: p4h_diff.py <a.txt> <b.txt> [max_context]
"""
import sys
from difflib import SequenceMatcher

def pcs(path, limit=None):
    out = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            if line.startswith("0x"):
                out.append(line.strip())
                if limit and len(out) >= limit:
                    break
    return out

a = pcs(sys.argv[1])
b = pcs(sys.argv[2])
print("A=%d PCs  B=%d PCs" % (len(a), len(b)))
sm = SequenceMatcher(None, a, b, autojunk=False)
reported = 0
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag == "equal":
        continue
    # Ghost PC of a 32-bit Thumb instruction: melonDS logs start and start+2.
    if tag == "delete" and (i2 - i1) == 1 and i1 > 0 and a[i1] == "%#010x" % (int(a[i1 - 1], 16) + 2):
        continue
    print("\n%s A[%d:%d] (%d) B[%d:%d] (%d)" % (tag, i1, i2, i2 - i1, j1, j2, j2 - j1))
    print("  A ctx before:", " ".join(a[max(0, i1 - 6):i1]))
    print("  A block     :", " ".join(a[i1:i2][:12]))
    print("  B block     :", " ".join(b[j1:j2][:12]))
    print("  A ctx after :", " ".join(a[i2:i2 + 6]))
    print("  B ctx after :", " ".join(b[j2:j2 + 6]))
    reported += 1
    if reported >= 2:
        break
if not reported:
    print("no substantive difference (only Thumb ghost PCs)")
