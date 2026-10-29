#!/usr/bin/env python3
"""Scan Ghidra function bodies for ARM SWI (svc) and Thumb SWI (svc #imm8).

  python3 tools/scan_swis.py [image] [ghidra_tsv] [--limit N]

Prints one line per function that contains at least one SWI, with the entry,
name, and the sorted set of SWI immediates found (aligned words / halfwords).
"""
import struct
import sys

pos = []
skip = False
for a in sys.argv[1:]:
    if skip:
        skip = False
        continue
    if a == "--limit":
        skip = True
        continue
    if a.startswith("--"):
        continue
    pos.append(a)
img_path = pos[0] if len(pos) > 0 else "work/ghidra_src/arm9.bin"
tsv_path = pos[1] if len(pos) > 1 else "work/ghidra_arm9_functions.tsv"
limit = 60
if "--limit" in sys.argv:
    limit = int(sys.argv[sys.argv.index("--limit") + 1])

img = open(img_path, "rb").read()
base = 0x02000000

fns = []
with open(tsv_path) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        fns.append((int(p[0], 16), int(p[2]), int(p[5]), p[1]))
fns.sort()

hits = []
for a, body, insn_len, name in fns:
    off = a - base
    if off < 0 or off + 4 > len(img):
        continue
    end = min(off + max(body, 4), len(img) - 3)
    imms = []
    if insn_len == 2:  # Thumb: svc #imm8 is 0xDFxx
        o = off
        while o + 2 <= end:
            h = struct.unpack("<H", img[o:o + 2])[0]
            if (h & 0xFF00) == 0xDF00:
                imms.append(h & 0xFF)
            o += 2
    else:
        for o in range(off, end - 3, 4):
            w = struct.unpack("<I", img[o:o + 4])[0]
            if (w & 0x0F000000) == 0x0F000000:
                imms.append(w & 0xFFFFFF)
    if imms:
        hits.append((a, len(imms), sorted(set(imms)), name, insn_len))

print("Ghidra functions containing a SWI: %d" % len(hits))
for a, n, im, name, il in hits[:limit]:
    print("  0x%08X  n=%-3d imm=%s  %s" % (a, n, ",".join("0x%X" % x for x in im[:8]), name))
if len(hits) > limit:
    print("  ... %d more" % (len(hits) - limit))
allimm = sorted(set(x for _, _, im, _, _ in hits for x in im))
print("distinct SWI immediates across all Ghidra functions: %s" % ",".join("0x%X" % x for x in allimm))
