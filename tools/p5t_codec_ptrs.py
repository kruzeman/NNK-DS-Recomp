#!/usr/bin/env python3
"""Phase-5T: dump the movie codec struct and test every pointer field against
the frame's U/V planes (any of a few layouts).

usage: p5t_codec_ptrs.py <mel_ram.bin> <movie.yuv> <frame> [codec_addr]
"""
import sys

W, H = 256, 192
FRAME = 73728
UVSZ = 12288
BASE = 0x02000000


def corr(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    den = ((n * saa - sa * sa) * (n * sbb - sb * sb)) ** 0.5
    return (n * sab - sa * sb) / den if den else 0.0


def main():
    ramp, yuvp, fr = sys.argv[1], sys.argv[2], int(sys.argv[3])
    codec = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x0227834C
    ram = open(ramp, 'rb').read()
    d = open(yuvp, 'rb').read()
    buf = d[fr * FRAME:(fr + 1) * FRAME]
    U = buf[W * H:W * H + UVSZ]
    V = buf[W * H + UVSZ:]
    co = codec - BASE
    print('codec struct @0x%08X:' % codec)
    for i in range(0, 0x200, 4):
        w = int.from_bytes(ram[co + i:co + i + 4], 'little')
        tag = ''
        if 0x02000000 <= w < 0x02400000:
            off = w - BASE
            y = ram[off:off + UVSZ]
            cu = corr(y[:1024], U[:1024])
            cv = corr(y[:1024], V[:1024])
            tag = '  RAM ptr  corrU %.3f corrV %.3f' % (cu, cv)
        print('  +0x%03X = 0x%08X%s' % (i, w, tag))


main()
