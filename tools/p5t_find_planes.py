#!/usr/bin/env python3
"""Phase-5T: locate a decoded frame's Y/U/V planes inside a melonDS main-RAM dump.

usage: p5t_find_planes.py <mel_ram.bin> <movie.yuv> <frame> [ram_base]
Prints every RAM address whose contents equal the Y, U and V plane of that
frame (exact byte match on a 256-byte probe, then full-plane verification).
"""
import sys

W, H = 256, 192
FRAME = 73728


def main():
    ramp, yuvp, fr = sys.argv[1], sys.argv[2], int(sys.argv[3])
    base = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x02000000
    ram = open(ramp, 'rb').read()
    d = open(yuvp, 'rb').read()
    buf = d[fr * FRAME:(fr + 1) * FRAME]
    planes = {
        'Y': (buf[:W * H], W * H),
        'U': (buf[W * H:W * H + 12288], 12288),
        'V': (buf[W * H + 12288:], 12288),
    }
    for name, (pl, size) in planes.items():
        probe = pl[:64]
        hits = []
        pos = ram.find(probe)
        while pos >= 0 and len(hits) < 8:
            if ram[pos:pos + size] == pl:
                hits.append(pos)
            pos = ram.find(probe, pos + 1)
        print('%s plane (%d bytes): %d hit(s)' % (name, size, len(hits)))
        for h in hits:
            print('   @0x%08X' % (base + h))
        if not hits:
            # report the best near-miss by comparing the 64-byte probe
            pass


main()
