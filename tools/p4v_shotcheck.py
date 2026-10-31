#!/usr/bin/env python3
"""Phase-4V: summarize the headless PPM shots — distinct byte values per frame
(the picture test: non-uniform = more than one distinct value)."""
import glob
import os
import sys

S = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/hermes/.hermes/profiles/coder/cache/scratch"
files = sorted(glob.glob(os.path.join(S, "p4v_sh.*.ppm")))
print("shots:", len(files))
nonuniform = 0
for f in files:
    d = open(f, "rb").read()
    parts = d.split(b"\n", 3)
    hdr = b" ".join(parts[:3]).split()
    w, h = int(hdr[1]), int(hdr[2])
    pix = parts[3]
    vals = sorted(set(pix))
    name = os.path.basename(f)
    if len(vals) > 1:
        nonuniform += 1
        print(f"{name}: {w}x{h} distinct={len(vals)} sample={vals[:10]}")
print("non-uniform frames:", nonuniform)
