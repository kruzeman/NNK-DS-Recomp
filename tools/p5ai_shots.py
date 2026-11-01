#!/usr/bin/env python3
"""Report per-frame byte statistics for headless PPM shots (Phase-5AI).

usage: p5ai_shots.py <dir-glob>
Prints name, distinct byte value count, min/max/mean, and whether the frame is
single-valued (=== a flat screen).
"""
import glob
import sys

pat = sys.argv[1] if len(sys.argv) > 1 else "*.ppm"
for f in sorted(glob.glob(pat)):
    d = open(f, 'rb').read()
    parts = d.split(b"\n", 3)
    hdr = b" ".join(parts[:3]).split()
    pix = parts[3]
    vals = sorted(set(pix))
    mean = sum(pix) / float(len(pix))
    print("%-22s distinct=%4d min=%3d max=%3d mean=%6.1f %s" %
          (f.rsplit('/', 1)[-1], len(vals),
           vals[0], vals[-1], mean,
           "FLAT" if len(vals) == 1 else ""))
