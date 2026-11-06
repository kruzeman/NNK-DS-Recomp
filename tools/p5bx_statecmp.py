#!/usr/bin/env python3
"""Phase-5BX helper: region-by-region comparison of two NNKS savestates.

Reads the NNKS v2 container (72-byte header + region records {id,version,offset,
length,crc,nfields}) and compares the payload of every region that exists in
both files.  For a differing region it prints the first few differing offsets
(relative to the region) and the 16-byte window around each, so a divergence can
be attributed to a guest structure instead of "the hash differs".

usage: p5bx_statecmp.py <a.nnks> <b.nnks> [first_diff_count]
"""
import struct
import sys

HDR = struct.Struct('<4sIIIIIQIIQQQII')    # magic ver hsize nreg build flags rom rcrc acrc step frame psize pcrc hcrc
REG2 = struct.Struct('<IIQQII16s')         # v2: id ver off len crc nfields name


def read(path):
    b = open(path, 'rb').read()
    (magic, ver, hsize, nreg, build, flags, rom, rcrc, acrc, step, frame,
     psize, pcrc, hcrc) = HDR.unpack_from(b, 0)
    assert magic == b'NNKS', path
    regs = {}
    for i in range(nreg):
        rid, rver, roff, rlen, rcrc_, nf, name = REG2.unpack_from(b, HDR.size + i * REG2.size)
        regs[rid] = (rver, hsize + roff, rlen, rcrc_, nf,
                     name.decode('ascii', 'replace').rstrip('\0'))
    return b, hsize, regs, step


def main():
    a, ahdr, aregs, astep = read(sys.argv[1])
    b, bhdr, bregs, bstep = read(sys.argv[2])
    print('step %d vs %d' % (astep, bstep))
    k = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    names = {1: 'io', 2: 'smc', 3: 'card', 4: 'input', 5: 'movie',
             6: 'moviestreams', 7: 'display', 8: 'gxblob', 9: 'ram',
             10: 'itcm', 11: 'dtcm'}
    for rid in sorted(set(aregs) & set(bregs)):
        av, ao, al, ac, anf, nm = aregs[rid]
        bv, bo, bl, bc, bnf, _ = bregs[rid]
        nm = names.get(rid, nm)
        if al != bl:
            print('%-12s LENGTH %d vs %d (layout changed)' % (nm, al, bl))
            continue
        pa = a[ao:ao + al]
        pb = b[bo:bo + bl]
        if pa == pb:
            print('%-12s IDENTICAL (%d B, crc 0x%08X)' % (nm, al, ac))
            continue
        diffs = [i for i in range(al) if pa[i] != pb[i]]
        print('%-12s DIFFERS: %d of %d bytes (crc 0x%08X vs 0x%08X)'
              % (nm, len(diffs), al, ac, bc))
        for i in diffs[:k]:
            s = max(0, i - 8)
            print('   off 0x%06X  A=%s  B=%s' % (i, pa[s:i + 8].hex(), pb[s:i + 8].hex()))
    return 0


if __name__ == '__main__':
    sys.exit(main())
