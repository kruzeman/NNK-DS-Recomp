#!/usr/bin/env python3
"""Phase-5T: find the chroma planes of a decoded frame near its Y plane.

usage: p5t_find_chroma.py <mel_ram.bin> <movie.yuv> <frame> <y_addr> [ram_base]
Scans offsets 0x8000..0x20000 (0x10 granularity) from the Y plane address and
reports the best correlation with the frame's U and V planes.
"""
import sys

W, H = 256, 192
FRAME = 73728
UW = W // 2
UH = H // 2
USZ = UW * UH


def corr(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    num = n * sab - sa * sb
    den = ((n * saa - sa * sa) * (n * sbb - sb * sb)) ** 0.5
    return num / den if den else 0.0


def main():
    ramp, yuvp, fr, yaddr = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4], 0)
    base = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x02000000
    ram = open(ramp, 'rb').read()
    d = open(yuvp, 'rb').read()
    buf = d[fr * FRAME:(fr + 1) * FRAME]
    U = buf[W * H:W * H + USZ]
    V = buf[W * H + USZ:]
    yoff = yaddr - base
    print('Y @0x%08X  (dump offset 0x%X)' % (yaddr, yoff))
    for name, pl in (('U', U), ('V', V)):
        probe = pl[:256]
        best = []
        for off in range(0x8000, 0x20000, 0x10):
            cand = ram[yoff + off:yoff + off + USZ]
            if len(cand) < USZ:
                break
            c = corr(cand[:1024], probe[:1024])
            if c > 0.5:
                best.append((c, off))
        best.sort(reverse=True)
        print('%s plane: %d candidates corr>0.5' % (name, len(best)))
        for c, off in best[:5]:
            full = ram[yoff + off:yoff + off + USZ]
            print('   +0x%05X -> 0x%08X  corr(1k) %.4f  MAE %.1f  exact=%s' % (
                off, yaddr + off, c,
                sum(abs(a - b) for a, b in zip(full, pl)) / USZ,
                full == pl))


main()
