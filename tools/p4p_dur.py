#!/usr/bin/env python3
"""start->end durations and inter-start gaps for melonDS card transfers."""
import re, sys

ev = []
for l in open(sys.argv[1], errors='replace'):
    f = l.split()
    if not f or f[0] not in ('start', 'end'):
        continue
    d = dict(re.findall(r'(\w+)=(\S+)', l))
    ev.append((f[0], int(d['ts']), d.get('cmd', '')[:2], d.get('len'), d.get('reads')))

lo, hi = int(sys.argv[2]), int(sys.argv[3])
sel = [e for e in ev if lo <= e[1] <= hi]
print('events in window', len(sel))
pairs = []
i = 0
while i < len(sel) - 1:
    if sel[i][0] == 'start':
        j = i + 1
        while j < len(sel) and sel[j][0] != 'end':
            j += 1
        if j < len(sel):
            pairs.append((sel[i], sel[j]))
        i = j
    else:
        i += 1
print('pairs', len(pairs))
durs = []
for s, e in pairs[:12]:
    print('  start ts=%d %s len=%s  end ts=%d reads=%s  dur=%d' %
          (s[1], s[2], s[3], e[1], e[4], e[1] - s[1]))
    durs.append(e[1] - s[1])
if durs:
    print('dur min/max/first', min(durs), max(durs), durs[0])
gaps = [pairs[i + 1][0][1] - pairs[i][0][1] for i in range(len(pairs) - 1)]
print('inter-start gaps (first 12):', gaps[:12])
