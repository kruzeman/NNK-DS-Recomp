#!/usr/bin/env python3
"""oam_dump.py <oam.bin> [x y w h] — list the enabled OBJ entries of an OAM
dump (512 bytes of attribute pairs x 128 sprites, DS layout) and, when a
rectangle is given, how many cover it. Used to tell "the picture is a sprite
layer we fail to draw" from "that region has no sprites at all"."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
rect = [int(v) for v in sys.argv[2:6]] if len(sys.argv) > 5 else None
SIZES = {0: ('8x8', '16x16', '32x32', '64x64'),
         1: ('16x8', '32x8', '32x16', '64x32'),
         2: ('8x16', '8x32', '16x32', '32x64')}
rows = []
cover = 0
for i in range(128):
    a0, a1, a2 = struct.unpack_from('<HHH', d, i * 8)
    if a0 & 0x0200:              # bit9 = disable; bit8 = rotate/scale
        continue                 # (parked sprites set y=192 instead)
    y = a0 & 0xFF
    x = a1 & 0x1FF
    shape = (a0 >> 14) & 3
    size = (a1 >> 14) & 3
    if shape == 3:
        continue
    w, h = [int(v) for v in SIZES[shape][size].split('x')]
    if y > 192:
        y -= 256
    if x > 256:
        x -= 512
    visible = (y < 192 and y + h > 0 and x < 256 and x + w > 0)
    rows.append((i, x, y, w, h, a1 & 0x0FFF, (a2 >> 12) & 0xF, a0 & 0x3000,
                 bool(a2 & 0x2000), a2 & 0xFF, (a2 >> 9) & 0x1F))
    if rect:
        rx, ry, rw, rh = rect
        if not (x + w <= rx or rx + rw <= x or y + h <= ry or ry + rh <= y):
            cover += 1
print('%d enabled sprites, %d visible (on-screen)'
      % (len(rows), sum(1 for r in rows if r[2] < 192 and r[2] + r[4] > 0
                        and r[1] < 256 and r[1] + r[3] > 0)))
for r in rows[:60]:
    print('  obj%3d x=%4d y=%4d %dx%d tile=%3d pal=%d prio=%d 256col=%s rot=%s'
          % (r[0], r[1], r[2], r[3], r[4], (r[5] >> 9) & 0x1FF, r[6], r[7] >> 12,
             r[8], r[9]))
if rect:
    print('sprites overlapping rect %s: %d' % (rect, cover))
