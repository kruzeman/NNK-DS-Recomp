#!/usr/bin/env python3
"""Phase-4E: seeds for a runtime-loaded region (overlay), from the trace.

The ARM9 static image ends at 0x0206CB78; the boot then loads NITRO overlay
modules into main RAM at fixed slots (0x020Cxxxx, 0x0210xxxx, ...) and jumps
into them.  The recompiler only knows the static image, so those regions are
"untranslated pc" barriers even though melonDS executes them.

This turns the melonDS PC trace into (a) the seed table for
`nnktool recompile-all --base <lo> --seeds ...` over a memory dump of that
region, and (b) prints the per-64 KiB band occupancy of the region, which is
the "translation sweep" for the coverage class (one pass, not one crash per
opcode).

    tools/mk_region_seeds.py <trace> <lo> <hi> <out.tsv> [modefile]

Seeds are one row per distinct trace PC in [lo, hi).  With [modefile] (the
MELONDS_ARM9MODE_PATH capture: "0xADDR thumb" per executed PC) the ARM/Thumb
decision is the mode melonDS actually executed at that address -- this is the
only reliable source for 4-aligned PCs inside Thumb code.  Without it the
alignment rule is used (4-aligned -> ARM, else Thumb), which MIS-decodes the
4-aligned half of a Thumb region.
"""
import sys

if len(sys.argv) not in (5, 6):
    sys.exit(__doc__)

trace, lo, hi, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
modefile = sys.argv[5] if len(sys.argv) == 6 else None

modes = {}
mixed = []
if modefile:
    with open(modefile, errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) != 2 or not p[0].startswith('0x'):
                continue
            v = int(p[0], 16)
            if not (lo <= v < hi):
                continue
            m = int(p[1])
            if v in modes and modes[v] != m:
                mixed.append(v)
            modes[v] = m

pcs = set()
with open(trace, errors='replace') as f:
    for line in f:
        s = line.strip()
        if len(s) != 10 or not s.startswith('0x'):
            continue
        v = int(s, 16)
        if lo <= v < hi:
            pcs.add(v)

pcs = sorted(pcs)
bands = {}
for v in pcs:
    bands.setdefault(v >> 16, []).append(v)

rows = []
for v in pcs:
    if modefile:
        if v not in modes:
            sys.exit('pc 0x%08X in trace but missing from the mode file' % v)
        thumb = modes[v]
    else:
        thumb = 1 if (v & 3) else 0
    rows.append('%08X\tOVL_%08X\t0\tfalse\tunknown\t%d' % (v, v, 2 if thumb else 4))
with open(out, 'w') as f:
    f.write('entry\tname\tbody_bytes\tis_thunk\tcc\tinsn_len\n')
    f.write('\n'.join(rows) + ('\n' if rows else ''))

print('distinct trace PCs in [0x%08X,0x%08X): %d' % (lo, hi, len(pcs)))
print('thumb PCs: %d   arm PCs: %d' % (sum(1 for v in pcs if (modes.get(v) if modefile else (v & 3))),
                                       sum(1 for v in pcs if not (modes.get(v) if modefile else (v & 3)))))
if mixed:
    print('WARNING: %d PCs executed in BOTH modes (first 0x%08X)' % (len(mixed), mixed[0]))
print('occupied 64 KiB bands: %d' % len(bands))
for b in sorted(bands):
    v = bands[b]
    print('  band 0x%08X: %5d pcs  0x%08X..0x%08X' % (b << 16, len(v), v[0], v[-1]))
print('wrote %s (%d seed rows)' % (out, len(rows)))
