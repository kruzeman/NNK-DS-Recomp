#!/usr/bin/env python3
"""Compose a horizontal 2-up PPM (native | reference) for visual diffs.

Usage: side_by_side.py <left.ppm> <right.ppm> <out.ppm> [--gap N]

Both inputs must be P6 binary PPM.  stdlib only (the Phase-3 canon: no
ffmpeg/PIL anywhere in the capture pipeline).
"""
import sys


def read_ppm(path):
    d = open(path, 'rb').read()
    if not d.startswith(b'P6'):
        raise SystemExit('not P6: ' + path)
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, d[i:i + w * h * 3]


def main():
    a, b, out = sys.argv[1], sys.argv[2], sys.argv[3]
    gap = 4
    if '--gap' in sys.argv:
        gap = int(sys.argv[sys.argv.index('--gap') + 1])
    wa, ha, pa = read_ppm(a)
    wb, hb, pb = read_ppm(b)
    h = max(ha, hb)
    w = wa + gap + wb
    buf = bytearray()
    for y in range(h):
        for x in range(wa):
            off = (y * wa + x) * 3
            buf += pa[off:off + 3] if y < ha else b'\xff\x00\xff'
        for _ in range(gap):
            buf += b'\xff\x00\xff'
        for x in range(wb):
            off = (y * wb + x) * 3
            buf += pb[off:off + 3] if y < hb else b'\xff\x00\xff'
    with open(out, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (w, h))
        f.write(buf)
    print('%s | %s -> %s (%dx%d)' % (a, b, out, w, h))


main()
