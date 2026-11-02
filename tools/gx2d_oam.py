#!/usr/bin/env python3
"""On-screen sprites only: position, size, tile, and whether their tile data
is present in the OBJ flat view.  usage: oam2.py <state.bin> <engine>"""
import sys, struct

IMG = {0: 0xA1400, 1: 0xE1400}
path, eng = sys.argv[1], int(sys.argv[2])
d = open(path, 'rb').read()
dispcnt = struct.unpack_from('<I', d, 0x100 * eng)[0]
obj1d = (dispcnt >> 4) & 1
oam = d[0xC00 + eng * 0x400: 0xC00 + (eng + 1) * 0x400]
objv = d[IMG[eng]:IMG[eng] + (0x40000 if eng == 0 else 0x20000)]
print('engine %s dispcnt=%08x 1D=%d objvram_nonzero=%d'
      % ('AB'[eng], dispcnt, obj1d, sum(1 for b in objv if b)))
rows = []
for s in range(128):
    a0, a1, a2 = struct.unpack_from('<3H', oam, s * 8)
    y = a0 & 0xFF
    if a0 & 0x200:
        continue
    if y >= 192:
        continue
    rotscale = (a0 >> 8) & 1
    shape = (a0 >> 14) & 3
    size = (a1 >> 14) & 3
    colmode = (a0 >> 13) & 1
    tile = a2 & 0x3FF
    pal = (a2 >> 12) & 0xF
    prio = (a2 >> 10) & 3
    x = a1 & 0x1FF
    sx = x - 512 if x >= 256 else x
    tw = th = [8, 16, 32, 64][size]
    if shape == 1: tw, th = [(16, 8), (32, 8), (32, 16), (64, 32)][size]
    elif shape == 2: tw, th = [(8, 16), (8, 32), (16, 32), (32, 64)][size]
    bpp = 64 if colmode else 32
    ntl = (tw // 8) * (th // 8)
    span = objv[tile * bpp:(tile + ntl) * bpp]
    nz = sum(1 for b in span if b)
    rows.append((s, sx, y, tw, th, tile, prio, pal, colmode, rotscale, nz, len(span)))
rows.sort(key=lambda r: (r[2], r[1]))
print('%d sprites with y<192 (visible rows)' % len(rows))
for r in rows:
    print('  #%3d pos=(%3d,%3d) %2dx%-2d %s tile=%3d prio=%d pal=%d rs=%d data=%d/%d'
          % (r[0], r[1], r[2], r[3], r[4], '256c' if r[8] else '16c', r[5], r[6], r[7],
             r[9], r[10], r[11]))
