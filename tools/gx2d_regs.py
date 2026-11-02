#!/usr/bin/env python3
"""gx2d_regs.py <state.bin> — decode the engine register block, BGxCNT, and
the VRAMCNT bank map from a GX2D state image (see gxrender/src/gx_2d.h)."""
import struct
import sys

MSTNAME = {0: 'ABG/LCDC?', }


def engine(d, e):
    off = e * 0x100
    dispcnt = struct.unpack_from('<I', d, off)[0]
    bgcnt = list(struct.unpack_from('<4H', d, off + 4))
    blendcnt = struct.unpack_from('<H', d, off + 0x5C)[0]
    return dispcnt, bgcnt, blendcnt


def bgcnt_dec(v):
    mode = (v >> 7) & 1          # 0 tile, 1 bitmap (extended)
    prio = v & 3
    cbase = ((v >> 2) & 0xF) << 14
    sbase = ((v >> 8) & 0x1F) << 11
    size = (v >> 14) & 3
    col256 = (v >> 7) & 1        # tile: 256 colour ; bitmap: 1 = 15-bit direct
    direct = (v >> 2) & 1        # bitmap mode only: 1 = direct colour
    areaov = (v >> 13) & 1
    return ('prio=%d %s extbase=0x%05x mapbase=0x%05x size=%d bmp_direct=%d '
            'tile256=%d areaov=%d raw=0x%04x' %
            (prio, 'BITMAP' if mode else 'tile ', cbase, sbase, size,
             direct if mode else -1, col256 if not mode else -1, areaov, v))


def vramcnt_name(b, cnt):
    if not (cnt & 0x80):
        return 'off'
    if b in (0, 1):
        ofs = (cnt >> 3) & 3
        return {0: 'LCDC', 1: 'ABG[%d]' % ofs, 2: 'AOBJ[%d]' % (ofs & 1),
                3: 'TEX[%d]' % ofs}[cnt & 3]
    if b in (2, 3):
        ofs = (cnt >> 3) & 7
        return {0: 'LCDC', 1: 'ABG[%d]' % ofs, 2: 'ARM7[%d]' % (ofs & 1),
                3: 'TEX[%d]' % ofs, 4: 'BBG' if b == 2 else 'BOBJ'}[cnt & 7]
    if b == 4:
        return {0: 'LCDC', 1: 'ABG[0]', 2: 'AOBJ[0]', 3: 'TEXPAL',
                4: 'ABGEXT'}[cnt & 7]
    if b in (5, 6):
        return {0: 'LCDC', 1: 'ABG', 2: 'AOBJ', 3: 'TEXPAL', 4: 'ABGEXT',
                5: 'AOBJEXT'}[cnt & 7]
    if b == 7:
        return {0: 'LCDC', 1: 'BBG', 2: 'BBGEXT'}[cnt & 3]
    if b == 8:
        return {0: 'LCDC', 1: 'BBG', 2: 'BOBJ', 3: 'BOBJEXT'}[cnt & 3]
    return '?'


def main(path):
    d = open(path, 'rb').read()
    for e in range(2):
        dispcnt, bgcnt, blendcnt = engine(d, e)
        bgmask = (dispcnt >> 8) & 0x1F
        print('engine %s DISPCNT=0x%08x mode=%d bgmask=0x%02x (%s) obj=%d 1D=%d '
              'bmp2D=%d forcedblank=%d mapbase=%d win0=%d win1=%d objwin=%d '
              'extpal=%d' % (
                  'AB'[e], dispcnt, dispcnt & 7, bgmask,
                  ','.join('BG%d' % i for i in range(4) if bgmask & (1 << i)) or '-',
                  (dispcnt >> 12) & 1, (dispcnt >> 4) & 1, (dispcnt >> 5) & 1,
                  (dispcnt >> 6) & 1, (dispcnt >> 7) & 1, (dispcnt >> 13) & 1,
                  (dispcnt >> 14) & 1, (dispcnt >> 15) & 1, (dispcnt >> 30) & 1))
        for i in range(4):
            print('   BG%dCNT=0x%04x %s' % (i, bgcnt[i], bgcnt_dec(bgcnt[i])))
        print('   BLENDCNT=0x%04x' % blendcnt)
    g = 0x200
    vc = list(d[g + 0x08:g + 0x11])
    print('VRAMCNT: ' + ' '.join('%s=%02x->%s' % ('ABCDEFGHI'[i], vc[i],
                                                  vramcnt_name(i, vc[i]))
                                 for i in range(9)))
    print('masterbright A=0x%04x B=0x%04x screens_enabled=%d swap=%d engA=%d engB=%d' %
          (struct.unpack_from('<H', d, g)[0], struct.unpack_from('<H', d, g + 2)[0],
           d[g + 4], d[g + 5], d[g + 6], d[g + 7]))


if __name__ == '__main__':
    main(sys.argv[1])
