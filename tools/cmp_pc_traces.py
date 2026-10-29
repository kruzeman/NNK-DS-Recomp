#!/usr/bin/env python3
"""Compare the melonDS ARM9 boot PC trace against the recompiled native run.

  python3 tools/cmp_pc_traces.py <melonds_trace> <native_trace>
                                 [--lo 0x02000000] [--hi 0x02400000]

Both files: one "0x%08x" line per executed instruction (melonDS also carries
"# ..." comment lines; the native run prints "#"-prefixed headers too).
Reports, in this order:

  1. strict common prefix  — identical PC sequences from index 0 until the
     first difference (this is the honest headline number),
  2. window-filtered prefix — both traces restricted to the melonDS logging
     window first (the native run may dispatch to PCs melonDS never logs),
  3. in-order matched count — how many of melonDS's PCs the native trace
     still contains in the same order after the divergence (greedy, so a
     resynced trace scores high), with the first divergence shown.
"""
import sys

args = sys.argv[1:]
lo = 0x02000000
hi = 0x02400000
files = []
i = 0
while i < len(args):
    if args[i] == "--lo":
        lo = int(args[i + 1], 0); i += 2
    elif args[i] == "--hi":
        hi = int(args[i + 1], 0); i += 2
    else:
        files.append(args[i]); i += 1

if len(files) != 2:
    sys.exit(__doc__)


def load(path, lo=0, hi=0x100000000):
    out = []
    for line in open(path):
        s = line.strip()
        if len(s) != 10 or not s.startswith("0x"):
            continue
        try:
            v = int(s, 16)
        except ValueError:
            continue
        if not (lo <= v < hi):
            continue
        out.append(v)
    return out


ref = load(files[0])
nat = load(files[1])
ref_w = load(files[0], lo, hi)
nat_w = load(files[1], lo, hi)

print("melonDS trace : %d PCs" % len(ref))
print("native  trace : %d PCs" % len(nat))


def prefix(a, b):
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


p = prefix(ref, nat)
print()
print("1) strict common prefix        : %d / %d melonDS PCs (%.1f%%)" %
      (p, len(ref), 100.0 * p / max(1, len(ref))))
if p < min(len(ref), len(nat)):
    print("   first divergence at index %d: melonDS 0x%08X vs native 0x%08X" %
          (p, ref[p], nat[p]))
elif p >= len(ref):
    print("   melonDS trace fully reproduced")

pw = prefix(ref_w, nat_w)
print("2) window-filtered prefix      : %d / %d (window 0x%08X..0x%08X)" %
      (pw, len(ref_w), lo, hi))
if pw < min(len(ref_w), len(nat_w)):
    print("   first divergence at index %d: melonDS 0x%08X vs native 0x%08X" %
          (pw, ref_w[pw], nat_w[pw]))

# greedy in-order match of melonDS PCs inside the native trace
j = 0
matched = 0
first_miss = None
for v in ref_w:
    while j < len(nat_w) and nat_w[j] != v:
        j += 1
    if j == len(nat_w):
        first_miss = first_miss if first_miss is not None else v
        break
    matched += 1
    j += 1
print("3) in-order matched after resync: %d / %d melonDS PCs (%.1f%%)" %
      (matched, len(ref_w), 100.0 * matched / max(1, len(ref_w))))
if first_miss is not None:
    print("   first melonDS PC absent from the native window trace: 0x%08X" % first_miss)
