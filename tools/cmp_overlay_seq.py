#!/usr/bin/env python3
"""Compare the OVERLAY-region PC sequence of the native run with melonDS.

    tools/cmp_overlay_seq.py <melonds_trace> <native_trace> <lo> <hi>

Both traces are filtered to PCs in [lo, hi) (a runtime-loaded overlay slot).
melonDS's sequence is searched for the BEST alignment of the native sequence:
we find the native subsequence's first element in melonDS, then count
positional matches with resynchronisation (skip until the next native PC is
found), and report the first divergence in overlay-relative terms -- i.e.
whether the translated overlay code executes the same instruction sequence
as the emulator, independent of the global timing offset.
"""
import sys

if len(sys.argv) != 5:
    sys.exit(__doc__)

m_path, n_path, lo, hi = sys.argv[1], sys.argv[2], int(sys.argv[3], 0), int(sys.argv[4], 0)


def read(path, drop_thumb2=False):
    out = []
    with open(path, errors='replace') as f:
        for line in f:
            s = line.strip()
            if len(s) == 10 and s.startswith('0x'):
                v = int(s, 16)
                if lo <= v < hi:
                    out.append(v)
    if drop_thumb2:
        # melonDS's hook is called for BOTH halfwords of a 32-bit Thumb
        # instruction (0x...E0 then 0x...E2), the recompiled image traces the
        # instruction once.  Drop the second halfword so the sequences are
        # comparable at instruction granularity.
        out = [v for i, v in enumerate(out)
               if not (i > 0 and (v & 2) and out[i - 1] == v - 2)]
    return out


m = read(m_path)
n = read(n_path)
print('melonDS overlay PCs: %d   native overlay PCs: %d' % (len(m), len(n)))
if not n or not m:
    sys.exit('no overlay PCs in one of the traces')

# align: find the native prefix inside melonDS (first element), then walk
start = None
for i, v in enumerate(m):
    if v == n[0]:
        start = i
        break
if start is None:
    sys.exit('native first overlay PC 0x%08X never executed by melonDS' % n[0])
print('alignment: native[0]=0x%08X at melonDS overlay index %d' % (n[0], start))

j = start
matched = 0
first_bad = None
resyncs = 0
for k, v in enumerate(n):
    if j < len(m) and m[j] == v:
        matched += 1
        j += 1
    else:
        if first_bad is None:
            first_bad = (k, v, m[j] if j < len(m) else None)
        # resync: skip melonDS until the native PC shows up again (bounded)
        nxt = None
        for d in range(1, 4000):
            if j + d < len(m) and m[j + d] == v:
                nxt = j + d
                break
        if nxt is None:
            print('cannot resync at native overlay index %d (0x%08X)' % (k, v))
            break
        resyncs += 1
        j = nxt
        matched += 1
        j += 1
print('matched in order (with resync): %d / %d native overlay PCs (%d resync events)'
      % (matched, len(n), resyncs))
if first_bad:
    k, v, exp = first_bad
    print('first divergence at native overlay index %d: native 0x%08X vs melonDS %s'
          % (k, v, ('0x%08X' % exp) if exp else '(end)'))
    print('native    around: %s' % ' '.join('0x%08X' % x for x in n[max(0, k - 6):k + 6]))
    print('melonDS   around: %s' % ' '.join('0x%08X' % x for x in m[max(0, j - 12):j + 6]))
else:
    print('native overlay sequence is a prefix of the melonDS overlay sequence')
