#!/usr/bin/env python3
"""tools/card_stream_gap.py — find the WAITS inside a card transfer stream.

Phase-5L: the guest parks after a `B8` dummy for ~4.06e9 raw ARM9 cycles and
then continues the stream.  A "stall" is invisible in a command-sequence diff
(the next command matches) but it is exactly what a step-limited run hits: the
run ends inside the wait and looks frozen.

Inputs (either form, auto-detected per line):
  * native   `p4t: start op=0xB7 addr=0x... size=512 cyc=<card cycles> steps=<n>`
  * melonDS  `start ts=<raw ARM9Timestamp> pc=0x... size=0x... cmd=B7...`

Usage:  python3 tools/card_stream_gap.py <log> [<log> ...] [--top N]
Prints, per log, the N largest gaps between consecutive transfer starts, with
the op/address on each side and the gap in the log's own time unit.
"""
import re
import sys

NAT = re.compile(r'p4t: start op=0x([0-9A-Fa-f]+) addr=0x([0-9A-Fa-f]+) size=(\d+) '
                 r'cyc=(\d+) steps=(\d+)')
MEL = re.compile(r'^start ts=(\d+) pc=0x([0-9A-Fa-f]+) size=0x([0-9A-Fa-f]+) '
                 r'.*?cmd=([0-9A-Fa-f]{16})')


def parse(path):
    """-> (unit, [(t, op, addr, size, extra)...])"""
    unit = '?'
    ev = []
    for line in open(path, 'r', errors='replace'):
        m = NAT.search(line)
        if m:
            unit = 'card cycles'
            op, addr, size, cyc, steps = (int(m.group(1), 16), int(m.group(2), 16),
                                          int(m.group(3)), int(m.group(4)),
                                          int(m.group(5)))
            ev.append((cyc, op, addr, size, 'steps=%d' % steps))
            continue
        m = MEL.match(line)
        if m:
            unit = 'raw ARM9Timestamp'
            ts, pc, size, cmd = (int(m.group(1)), int(m.group(2), 16),
                                 int(m.group(3), 16), m.group(4))
            op = int(cmd[0:2], 16)
            addr = int(cmd[2:10], 16)
            ev.append((ts, op, addr, size, 'pc=0x%08X' % pc))
    return unit, ev


def main(argv):
    top = 5
    if '--top' in argv:
        i = argv.index('--top')
        top = int(argv[i + 1])
        del argv[i:i + 2]
    for path in argv:
        unit, ev = parse(path)
        if len(ev) < 2:
            print('%s: %d starts — nothing to compare' % (path, len(ev)))
            continue
        gaps = []
        for a, b in zip(ev, ev[1:]):
            gaps.append((b[0] - a[0], a, b))
        gaps.sort(reverse=True)
        total = ev[-1][0] - ev[0][0]
        print('%s: %d starts, unit=%s, span=%d, median gap=%d'
              % (path, len(ev), unit, total,
                 sorted(g[0] for g in gaps)[len(gaps) // 2]))
        for d, a, b in gaps[:top]:
            print('  gap %-12d (%.1f%% of span)  after 0x%02X@0x%08X (%s)'
                  '  ->  0x%02X@0x%08X (%s)'
                  % (d, 100.0 * d / total if total else 0.0,
                     a[1], a[2], a[4], b[1], b[2], b[4]))


if __name__ == '__main__':
    main(sys.argv[1:])
