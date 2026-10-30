#!/usr/bin/env python3
"""Phase-4F: combine two overlay-slot snapshots.

The dispatch image gets ONE case per PC, but an overlay slot holds different
modules at different times.  Snapshot A (an early dump) has real bytes for the
modules loaded early and zeros where a slot was still empty; snapshot B is a
later dump of the same window.  Merge: keep A's bytes wherever A is non-zero
(the early module the already-verified path executes), fill only A's zero
holes from B (a module that is loaded later).  Colliding modules that reuse
the same addresses at different times stay a documented limitation.
"""
import sys

a_path, b_path, out_path = sys.argv[1:4]
a = open(a_path, "rb").read()
b = open(b_path, "rb").read()
assert len(a) == len(b), (len(a), len(b))
out = bytearray(a)
filled = 0
for i, av in enumerate(a):
    if av == 0 and b[i] != 0:
        out[i] = b[i]
        filled += 1
open(out_path, "wb").write(out)
print(f"{out_path}: {filled} bytes filled from {b_path} (of {len(a)})")
