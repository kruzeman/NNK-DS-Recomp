#!/usr/bin/env python3
"""Histogram the tail of a native PC trace and locate the steady-state loop."""
import sys, collections

path = sys.argv[1]
start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
pcs = []
with open(path, errors='replace') as f:
    idx = 0
    for l in f:
        if l.startswith('0x'):
            if idx >= start:
                pcs.append(int(l.strip(), 16))
            idx += 1
print('lines', len(pcs), 'from', start)
c = collections.Counter(pcs)
print('distinct', len(c))
for pc, n in c.most_common(20):
    print('  0x%08X  %d' % (pc, n))
last = pcs[-2000:]
c2 = collections.Counter(last)
print('last 2000 distinct', len(c2))
for pc, n in c2.most_common(20):
    print('  0x%08X  %d' % (pc, n))
# find last index where pc is outside the steady loop
loop = set(pc for pc, n in c2.most_common() if n > 20)
print('loop set', ['0x%08X' % p for p in sorted(loop)])
for i in range(len(pcs) - 1, -1, -1):
    if pcs[i] not in loop:
        print('last pc outside loop at trace index', start + i, 'pc=0x%08X' % pcs[i])
        for j in range(max(0, i - 40), min(len(pcs), i + 20)):
            print('   %d 0x%08X' % (start + j, pcs[j]))
        break
