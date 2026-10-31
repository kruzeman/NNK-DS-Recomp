#!/usr/bin/env python3
"""Diff the card transfer-start sequence: native (p4t: start) vs melonDS (start ... cmd=).

Phase-5H.  Both engines log one line per WriteROMCnt 0->1 start:

  native   p4t: start op=0xB7 addr=0x1D126600 size=512 cyc=... steps=...   (tools/split_dispatch.py)
  melonDS  start ts=7962150 pc=0x02016930 size=0x00000200 ... cmd=B71D126600000000  (tools/melonds-cardlog.patch)

The (op, addr, size) tuple is the only thing that names WHO chooses a command:
it is built by the guest (the CARD_ReadRom / dummy-read helpers at 0x0201690C
and 0x02016940) and merely shipped by the emulator, so a tuple mismatch means
the guest diverged and a *missing* tuple at the end means the request producer
stopped.

usage: tools/card_cmdseq.py <native.err> <melonDS.cardlog>

Prints counts, the first differing index with context, and the 0xB7-only
address stream comparison (the ROM data reads).
"""
import re
import sys

nat_path, mel_path = sys.argv[1], sys.argv[2]

nat = []   # (op, addr, size, steps, cyc)
for line in open(nat_path, errors='replace'):
    m = re.match(r'p4t: start op=0x([0-9A-Fa-f]{2}) addr=0x([0-9A-Fa-f]{8}) '
                 r'size=(\d+) cyc=(\d+) steps=(\d+)', line)
    if m:
        nat.append((int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)),
                    int(m.group(5)), int(m.group(4))))

mel = []   # (op, addr, size, ts)
for line in open(mel_path, errors='replace'):
    if not line.startswith('start ts='):
        continue
    parts = dict(p.split('=', 1) for p in line.split() if '=' in p)
    cmd = parts['cmd']
    mel.append((int(cmd[0:2], 16), int(cmd[2:10], 16), int(parts['size'], 16),
                int(parts['ts'])))

print("native   starts=%d  B7=%d  B8=%d"
      % (len(nat), sum(1 for t in nat if t[0] == 0xB7),
         sum(1 for t in nat if t[0] == 0xB8)))
print("melonDS  starts=%d  B7=%d  B8=%d"
      % (len(mel), sum(1 for t in mel if t[0] == 0xB7),
         sum(1 for t in mel if t[0] == 0xB8)))

n = min(len(nat), len(mel))
diff = None
for i in range(n):
    if nat[i][:3] != mel[i][:3]:
        diff = i
        break
print("\nfirst (op,addr,size) divergence at index %s of %d compared"
      % (diff if diff is not None else 'none', n))

lo = max(0, (diff if diff is not None else n) - 4)
hi = min(max(len(nat), len(mel)), lo + 16)
print("\n  idx | native (op addr size step cyc)          | melonDS (op addr size ts)")
for i in range(lo, hi):
    a = nat[i] if i < len(nat) else None
    b = mel[i] if i < len(mel) else None
    mark = '  <<< DIFF' if diff is not None and i == diff else ''
    sa = ("%02X %08X %5d %10d %10d" % a) if a else "-"
    sb = ("%02X %08X %5d %12d" % b) if b else "-"
    print("%5d | %-41s | %s%s" % (i, sa, sb, mark))

nb7 = [t for t in nat if t[0] == 0xB7]
mb7 = [t for t in mel if t[0] == 0xB7]
k = min(len(nb7), len(mb7))
d7 = None
for i in range(k):
    if nb7[i][1] != mb7[i][1] or nb7[i][2] != mb7[i][2]:
        d7 = i
        break
print("\nB7-only: native=%d melonDS=%d  first addr/size divergence at %s"
      % (len(nb7), len(mb7), d7 if d7 is not None else 'none'))
if d7 is not None:
    for i in range(max(0, d7 - 3), min(k, d7 + 6)):
        print("%5d | nat %08X %5d | mel %08X %5d"
              % (i, nb7[i][1], nb7[i][2], mb7[i][1], mb7[i][2]))
