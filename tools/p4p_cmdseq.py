#!/usr/bin/env python3
"""Line up melonDS vs native card command streams by transfer index."""
import re, sys


def starts(path, key):
    out = []
    for l in open(path, errors='replace'):
        if not l.startswith('start'):
            continue
        d = dict(re.findall(r'(\w+)=(\S+)', l))
        cmd = d.get('cmd', '')
        op = cmd[:2]
        addr = int(cmd[2:10], 16) if len(cmd) >= 10 else -1
        out.append({'t': int(d[key]), 'op': op, 'addr': addr,
                    'size': int(d['size'], 16), 'pc': d.get('pc')})
    return out


mel = starts('/home/hermes/.hermes/profiles/coder/cache/scratch/p4o_card.log', 'ts')
nat = starts(sys.argv[1], 'step')
print('mel', len(mel), 'nat', len(nat))
n = min(len(mel), len(nat))
bad = 0
for i in range(n):
    if (mel[i]['op'], mel[i]['addr'], mel[i]['size']) != (nat[i]['op'], nat[i]['addr'], nat[i]['size']):
        print('FIRST COMMAND DIVERGENCE at index %d' % i)
        for j in range(max(0, i - 3), min(n, i + 3)):
            print('  mel[%d] %s' % (j, mel[j]))
            print('  nat[%d] %s' % (j, nat[j]))
        bad = 1
        break
if not bad:
    print('command stream identical for the first %d transfers' % n)
print('last nat transfers:')
for x in nat[-4:]:
    print('  ', x)
print('mel at the same index:')
for x in mel[n - 4:n + 2]:
    print('  ', x)
