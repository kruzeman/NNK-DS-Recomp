#!/usr/bin/env python3
"""Summarise melonDS card romdata reads: who consumes FIFO words (DMA vs CPU)."""
import re, sys, collections

path = sys.argv[1]
by_ts = collections.Counter()
halt = collections.Counter()
pc = collections.Counter()
first = None
rows = []
for l in open(path, errors='replace'):
    if not l.startswith('romdata'):
        continue
    d = dict(re.findall(r'(\w+)=(\S+)', l))
    ts = int(d['ts'])
    h = d.get('halted', '?')
    halt[h] += 1
    pc[(h, d.get('pc'))] += 1
    rows.append((ts, h))
print('romdata lines', len(rows))
print('halted counts', halt)
print('pc by halted (top 12):')
for k, v in pc.most_common(12):
    print('  ', k, v)
# window around the first mode-5 DMA arm (ts 56,526,612)
win = [r for r in rows if 56_000_000 <= r[0] <= 58_000_000]
print('reads in ts 56.0M..58.0M:', len(win), 'first', win[:3], 'last', win[-3:])
print('halted in window', collections.Counter(h for _, h in win))
# after the loop: ts > 58M, how many halted=1 and what are their timestamps
after = [r for r in rows if r[0] > 58_000_000]
print('reads after 58M:', len(after), 'halted', collections.Counter(h for _, h in after))
h1 = [ts for ts, h in after if h == '1']
print('halted=1 ts sample', h1[:5], '...', h1[-3:] if h1 else None)
