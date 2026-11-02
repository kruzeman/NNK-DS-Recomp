#!/usr/bin/env python3
"""mel_parse.py — extract GPU state (palette, OAM, VRAM banks A..I, VRAMCNT,
bank maps) from a melonDS savestate, using the section layout of
GPU::DoSavestate in melonDS 1.1 (src/GPU.cpp).

usage: mel_parse.py <state.mln> <outdir>
writes outdir/{palette.bin,oam.bin,vramA..vramI.bin,vramcnt.txt,maps.txt}
"""
import os
import struct
import sys

U32 = 4


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


def main(path, outdir):
    d = open(path, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    gsec = find_section(d, b'GPUG')
    if gsec is None:
        print('GPUG not found'); return 2
    seclen = struct.unpack_from('<I', d, gsec + 4)[0]
    p = gsec + 16
    end = gsec + seclen
    print('GPUG section @0x%x len=0x%x payload=0x%x' % (gsec, seclen, seclen - 16))

    def take(name, n):
        nonlocal p
        blob = d[p:p + n]
        assert len(blob) == n, (name, n, len(blob))
        if name:
            open(os.path.join(outdir, name), 'wb').write(blob)
        p += n
        return blob

    vcount, nextvc, total, dispstat0, dispstat1, vmatch0, vmatch1 = \
        struct.unpack_from('<HIHHHHH', d, p)
    print('VCount=%d NextVCount=%d Scanlines=%d DispStatA=0x%04x B=0x%04x '
          'VMatch %d/%d' % (vcount, nextvc, total, dispstat0, dispstat1,
                            vmatch0, vmatch1))
    p += 16
    take('palette.bin', 2048)
    take('oam.bin', 2048)
    sizes = [('A', 0x20000), ('B', 0x20000), ('C', 0x20000), ('D', 0x20000),
             ('E', 0x10000), ('F', 0x4000), ('G', 0x4000), ('H', 0x8000),
             ('I', 0x4000)]
    for name, sz in sizes:
        take('vram%s.bin' % name, sz)
    vramcnt = take(None, 9)
    vramstat = take(None, 1)[0]
    print('VRAMCNT: ' + ' '.join('%s=%02x' % ('ABCDEFGHI'[i], vramcnt[i])
                                 for i in range(9)) + '  VRAMSTAT=%02x' % vramstat)
    lcdc = struct.unpack_from('<I', d, p)[0]
    p += 4
    maps = {}
    for name, n in (('ABG', 32), ('AOBJ', 16), ('BBG', 8), ('BOBJ', 8),
                    ('ABGEXT', 4), ('AOBJEXT', 1), ('BBGEXT', 4),
                    ('BOBJEXT', 1), ('TEX', 4), ('TEXPAL', 8), ('ARM7', 2)):
        vals = list(struct.unpack_from('<%dI' % n, d, p))
        maps[name] = vals
        p += 4 * n
    print('VRAMMap_LCDC=0x%08x' % lcdc)
    with open(os.path.join(outdir, 'maps.txt'), 'w') as f:
        f.write('VRAMCNT ' + ' '.join('%02x' % v for v in vramcnt) + '\n')
        f.write('LCDC 0x%08x\n' % lcdc)
        for k, v in maps.items():
            f.write('%-8s %s\n' % (k, ' '.join('0x%02x' % x for x in v)))
    print('remaining in section: %d bytes' % (end - p))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
