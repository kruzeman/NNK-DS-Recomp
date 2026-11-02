#!/usr/bin/env python3
"""gx2d_bitmap.py — census + visual dump of the BG bitmap regions of a GX2D
state image (`NNK_GX_STATE_DUMP=<path>`).

WHY (Phase-5AV, second run): the in-game prompt's background is not a tile
layer but a 256x192 *direct-colour bitmap* BG, so "the background is garbled"
has to be judged on the bitmap bytes themselves, not on a screen dump.  This
tool answers three questions offline (milliseconds per state image, no guest
rebuild):

  * which 8 KiB blocks of each flat view hold data (is a bank empty?);
  * does a given ABG offset decode as a coherent picture;
  * what does its luma look like (ASCII map) — a real scene shows a horizon /
    flat sky, corrupted data shows noise even in luma.

usage:
  gx2d_bitmap.py census <state>
  gx2d_bitmap.py luma   <state> <abg_off_hex>       ASCII luma map
  gx2d_bitmap.py png    <state> <abg_off_hex> <out> 256x192 BGR555 -> PNG (x3)
"""
import sys

ABG_OFF, ABG_SZ = 0x1400, 0x80000
VIEWS = (('ABG', 0x1400, 0x80000), ('BBG', 0x81400, 0x20000),
         ('AOBJ', 0xA1400, 0x40000), ('BOBJ', 0xE1400, 0x20000),
         ('ABGEXT', 0x101400, 0x8000), ('BBGEXT', 0x109400, 0x8000),
         ('AOBJEXT', 0x111400, 0x2000), ('BOBJEXT', 0x113400, 0x2000))
BLK = 0x2000


def census(d):
    for name, off, sz in VIEWS:
        buf = d[off:off + sz]
        nz = sum(1 for b in buf if b)
        blocks = [sum(1 for x in buf[b * BLK:(b + 1) * BLK] if x)
                  for b in range(sz // BLK)]
        print('%-8s nonzero=%-7d blocks(8K): %s' % (name, nz,
              ' '.join(str(v) for v in blocks)))


def pixel(abg, off, x, y, w=256):
    a = off + (y * w + x) * 2
    v = abg[a] | (abg[a + 1] << 8)
    return v & 0x1F, (v >> 5) & 0x1F, (v >> 10) & 0x1F


def luma(d, off):
    abg = d[ABG_OFF:ABG_OFF + ABG_SZ]
    chars = ' .:-=+*#%@'
    for by in range(0, 192, 8):
        row = ''
        for bx in range(0, 256, 4):
            s = n = 0
            for y in range(by, by + 8):
                for x in range(bx, bx + 4):
                    r, g, b = pixel(abg, off, x, y)
                    s += (r * 3 + g * 6 + b) // 10
                    n += 1
            row += chars[min(9, (s // n) * 10 // 32)]
        print(row)


def png(d, off, out):
    from PIL import Image
    abg = d[ABG_OFF:ABG_OFF + ABG_SZ]
    img = Image.new('RGB', (256, 192))
    px = img.load()
    for y in range(192):
        for x in range(256):
            r, g, b = pixel(abg, off, x, y)
            px[x, y] = (r << 3, g << 3, b << 3)
    img.resize((256 * 3, 192 * 3), Image.NEAREST).save(out)
    print('wrote', out)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    d = open(argv[2], 'rb').read()
    if argv[1] == 'census':
        census(d)
    elif argv[1] == 'luma':
        luma(d, int(argv[3], 0))
    elif argv[1] == 'png':
        png(d, int(argv[3], 0), argv[4])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
