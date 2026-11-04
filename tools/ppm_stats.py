#!/usr/bin/env python3
"""ppm_stats.py <a.ppm> [b.ppm] — mean luma / lit fraction of 256x192 P6 PPMs,
and a byte comparison when two are given."""
import sys

def load(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    assert parts[0] == b'P6', parts[0]
    w, h = [int(v) for v in parts[1].split()]
    return w, h, parts[3]

w, h, a = load(sys.argv[1])
n = w * h
lum = sum(0.299 * a[i * 3] + 0.587 * a[i * 3 + 1] + 0.114 * a[i * 3 + 2]
          for i in range(n)) / n
lit = sum(1 for i in range(n) if a[i * 3] or a[i * 3 + 1] or a[i * 3 + 2])
print('%s: %dx%d mean_luma=%.1f lit=%d/%d (%.1f%%)' % (sys.argv[1], w, h, lum, lit, n, 100.0 * lit / n))
if len(sys.argv) > 2:
    w2, h2, b = load(sys.argv[2])
    same = (w, h, a) == (w2, h2, b)
    print('%s: %s' % (sys.argv[2], 'IDENTICAL to %s' % sys.argv[1] if same else 'DIFFERS'))
    if not same and len(a) == len(b):
        diff = sum(1 for i in range(n) if a[i * 3:i * 3 + 3] != b[i * 3:i * 3 + 3])
        print('  differing pixels: %d (%.1f%%)' % (diff, 100.0 * diff / n))
