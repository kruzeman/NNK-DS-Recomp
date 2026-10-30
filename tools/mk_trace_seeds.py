#!/usr/bin/env python3
"""Phase-3A3 trace-gap seeds: close the recovery holes the boot actually hits.

The Ghidra-seeded CFG walk leaves holes inside otherwise-recovered bodies
(~34% of bodies are only partially walked).  When the melonDS ground-truth
PC trace steps an address the dispatch image has no `case` for, the native
run halts there ("dispatch to untranslated pc").  Every such address is
trace-proven executable code, so seeding it directly is well-founded.

  python3 tools/mk_trace_seeds.py <melonds_trace> <generated.c> <base.tsv> <out.tsv> [mode.tsv]

* <generated.c>  — the CURRENT nnktool recompile-all output (its
  `case 0x02XXXXXXu:` labels are the translated-PC set),
* <base.tsv>     — the seed table that produced it (rows kept verbatim),
* <out.tsv>      — base rows + one row per untranslated trace PC inside the
  image range.  Without [mode.tsv] ARM is inferred from alignment
  (4-aligned -> insn_len 4, else 2), which mis-decodes 4-aligned PCs inside
  Thumb bodies; with [mode.tsv] (MELONDS_ARM9MODE_PATH: "0xADDR thumb") the
  mode is the one melonDS actually executed.


Re-run after each recompile: the case set only grows, so the gap set
converges to zero (verify with the tool's own summary before compiling).
Out-of-image trace PCs (BIOS 0xFFFFxxxx, ITCM 0x01FF8xxx, ...) cannot be
seeded into the flat-window image and are counted, not listed.
"""
import os
import re
import sys

if len(sys.argv) not in (5, 6):
    sys.exit(__doc__)

trace_path, c_path, base_tsv, out_tsv = sys.argv[1:5]
mode_path = sys.argv[5] if len(sys.argv) == 6 else None

modes = {}
if mode_path:
    with open(mode_path, errors='replace') as f:
        for line in f:
            p = line.split()
            if len(p) == 2 and p[0].startswith('0x'):
                modes[int(p[0], 16)] = int(p[1])

CASE_RE = re.compile(r'case (0x02[0-9A-Fa-f]{6})u:')

cases = set()
with open(c_path, errors='replace') as f:
    for line in f:
        m = CASE_RE.search(line)
        if m:
            cases.add(int(m.group(1), 16))
if not cases:
    raise SystemExit('no dispatch cases found in %s' % c_path)
img_lo, img_hi = min(cases), max(cases) + 0x1000

rows = []
have = set()
with open(base_tsv) as f:
    hdr = f.readline()
    for line in f:
        if not line.strip():
            continue
        rows.append(line.rstrip('\n'))
        have.add(int(line.split('\t')[0], 16))

gaps = []
out_of_range = 0
with open(trace_path) as f:
    for line in f:
        s = line.strip()
        if len(s) != 10 or not s.startswith('0x'):
            continue
        v = int(s, 16)
        if not (img_lo <= v < img_hi):
            out_of_range += 1
            continue
        if v in cases or v in have:
            continue
        have.add(v)
        gaps.append(v)

gaps.sort()
for v in gaps:
    mode = 2 if modes.get(v, 0 if (v & 3) == 0 else 1) else 4
    rows.append('%08X\tTRACE_%08X\t0\tfalse\tunknown\t%d' % (v, v, mode))

os.makedirs(os.path.dirname(os.path.abspath(out_tsv)), exist_ok=True)
with open(out_tsv, 'w') as f:
    f.write(hdr if hdr.startswith('entry\t') else 'entry\tname\tbody_bytes\tis_thunk\tcc\tinsn_len\n')
    f.write('\n'.join(rows) + '\n')

print('cases in image     : %d (range 0x%08X..0x%08X)' % (len(cases), img_lo, img_hi))
print('trace PCs out of image range (not seedable): %d' % out_of_range)
print('untranslated trace PCs added as seeds      : %d' % len(gaps))
if gaps:
    print('  first: %s' % ' '.join('0x%08X' % v for v in gaps[:8]))
print('wrote %s: %d rows total' % (out_tsv, len(rows)))
