#!/usr/bin/env python3
"""mel_2dregs.py <state.mln> — print the engine 2D registers of a melonDS
savestate (GPU2D::Unit::DoSavestate layout, melonDS 1.1)."""
import struct
import sys


def find_section(d, magic, start=0x10):
    off = start
    while off + 16 <= len(d):
        m = d[off:off + 4]
        seclen = struct.unpack_from('<I', d, off + 4)[0]
        if m == magic:
            return off
        if seclen < 16:
            break
        off += seclen
    return None


def main(path):
    d = open(path, 'rb').read()
    for magic in (b'GP2A', b'GP2B'):
        off = find_section(d, magic)
        p = off + 16
        dispcnt = struct.unpack_from('<I', d, p)[0]
        bgcnt = list(struct.unpack_from('<4H', d, p + 4))
        # positions/refs/rot: 4*2+4*2+2*4+2*4+2*4+2*4 + 2*2*4 = 8+8+8+8+8+8+16
        q = p + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 16
        win0 = d[q:q + 4]; win1 = d[q + 4:q + 8]; wincnt = d[q + 8:q + 12]
        q2 = q + 12
        bgmo = d[q2:q2 + 2]; objmo = d[q2 + 2:q2 + 4]
        q3 = q2 + 8          # +4 mosaic Y state bytes
        blendcnt, blendalpha = struct.unpack_from('<HH', d, q3)
        eva, evb, evy = d[q3 + 4], d[q3 + 5], d[q3 + 6]
        mb = struct.unpack_from('<H', d, q3 + 7)[0]
        print('%s DISPCNT=0x%08x BGCNT=%s win0=%s win1=%s wincnt=%s '
              'blend=0x%04x alpha=0x%04x eva=%d evb=%d evy=%d mbright=0x%04x' % (
                  magic.decode(), dispcnt,
                  ' '.join('0x%04x' % v for v in bgcnt),
                  list(win0), list(win1), list(wincnt), blendcnt, blendalpha,
                  eva, evb, evy, mb))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
