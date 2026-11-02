#!/usr/bin/env python3
"""Classify PPM shots: print frames whose pixels are not a single value."""
import glob
import sys

for f in sorted(glob.glob(sys.argv[1])):
    b = open(f, 'rb').read()
    px = b.split(b'\n255\n', 1)[1]
    s = set(px)
    if len(s) > 1:
        print(f, len(s), 'nonblank')
