#!/usr/bin/env python3
"""Phase-4B: ITCM seeds from the extended-window melonDS ARM9 PC trace.

    python3 tools/mk_itcm_seeds.py <melonds_trace> <blob_len> <out.tsv>

Every distinct trace PC in the ITCM window 0x01FF8000..0x02000000 is
trace-proven ARM code inside the loaded block, so it becomes an ARM recovery
seed (insn_len 4).  The blob file handed to nnktool covers exactly the
auto-load block (0x540 bytes), so seeds past it are rejected.  Prints the
seed count and any observed-but-out-of-block PCs (honest coverage report).
"""
import sys

if len(sys.argv) != 4:
    sys.exit(__doc__)

trace_path, blob_len_s, out_tsv = sys.argv[1], sys.argv[2], sys.argv[3]
ITCM_LO = 0x01FF8000
ITCM_HI = 0x02000000
blob_len = int(blob_len_s, 0)

seeds = []
seen = set()
out_of_block = 0
with open(trace_path) as f:
    for line in f:
        s = line.strip()
        if len(s) != 10 or not s.startswith('0x'):
            continue
        v = int(s, 16)
        if not (ITCM_LO <= v < ITCM_HI):
            continue
        if v - ITCM_LO >= blob_len:
            out_of_block += 1
            continue
        if v in seen:
            continue
        seen.add(v)
        seeds.append(v)
seeds.sort()

with open(out_tsv, 'w') as f:
    f.write('entry\tname\tbody_bytes\tis_thunk\tcc\tinsn_len\n')
    for v in seeds:
        f.write('%08X\tITCM_%08X\t0\tfalse\tunknown\t4\n' % (v, v))

print('ITCM seeds: %d distinct PCs -> %s' % (len(seeds), out_tsv))
print('observed ITCM PCs past blob end (0x%X): %d' % (blob_len, out_of_block))
if seeds:
    print('range: 0x%08X..0x%08X' % (seeds[0], seeds[-1]))
