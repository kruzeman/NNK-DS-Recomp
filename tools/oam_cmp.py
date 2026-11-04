#!/usr/bin/env python3
"""oam_cmp.py <nnk_state_dump> <mel_oam.bin> — compare the two 1K OAM blocks
(engine A @0x000, engine B @0x400) and list the entries that differ, with a
DS attr0/1/2 decode, so a sprite-layer defect can be told from a state skew."""
import struct
import sys

dump = open(sys.argv[1], 'rb').read()
IMG_OAM = 0xC00
ours = dump[IMG_OAM:IMG_OAM + 2048]
ref = open(sys.argv[2], 'rb').read()

SIZES = {0: ('8x8', '16x16', '32x32', '64x64'),
         1: ('16x8', '32x8', '32x16', '64x32'),
         2: ('8x16', '8x32', '16x32', '32x64')}


def decode(blk, base, i):
    a0, a1, a2 = struct.unpack_from('<HHH', blk, base + i * 8)
    shape, size = (a0 >> 14) & 3, (a1 >> 14) & 3
    wh = SIZES[shape][size] if shape != 3 else '---'
    return ('obj%02d a0=%04x a1=%04x a2=%04x y=%d x=%d %s tile=%d pal=%d '
            '256=%d prio=%d dis=%d rot=%d'
            % (i, a0, a1, a2, a0 & 0xFF, a1 & 0x1FF, wh, a1 & 0x3FF,
               (a2 >> 12) & 0xF, (a2 >> 13) & 1, (a2 >> 10) & 3,
               (a0 >> 9) & 1, (a0 >> 8) & 1))


for eng in (0, 1):
    base = eng * 0x400
    n_en_ours = sum(1 for i in range(128)
                    if not (struct.unpack_from('<H', ours, base + i * 8)[0] & 0x200))
    n_en_ref = sum(1 for i in range(128)
                   if not (struct.unpack_from('<H', ref, base + i * 8)[0] & 0x200))
    print('== engine %d: enabled sprites ours=%d ref=%d, bytes differ=%d/1024'
          % (eng, n_en_ours, n_en_ref,
             sum(1 for k in range(1024) if ours[base + k] != ref[base + k])))
    for i in range(128):
        o = ours[base + i * 8:base + i * 8 + 8]
        r = ref[base + i * 8:base + i * 8 + 8]
        if o == r:
            continue
        print('  ours ' + decode(ours, base, i))
        print('  ref  ' + decode(ref, base, i))
print('engine B block equal: %s' % (ours[0x400:0x800] == ref[0x400:0x800]))
