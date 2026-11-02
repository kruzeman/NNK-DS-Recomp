#!/usr/bin/env python3
"""mel_diff.py <gx-state.bin> <melodir> — byte-diff the GPU state of OUR build
(NNK_GX_STATE_DUMP image) against melonDS's own savestate of the same moment.

The GX2D image layout (gxrender/src/gx_2d.h):
  0x00400 2048 palette, 0x00C00 2048 OAM,
  0x1400 ABG(512K) 0x81400 BBG(128K) 0xA1400 AOBJ(256K) 0xE1400 BOBJ(128K)
  0x101400 ABGEXT 0x109400 BBGEXT 0x111400 AOBJEXT 0x113400 BOBJEXT
  0x195400 raw banks A..I (0x20000 each, written by NNK_GX_STATE_DUMP)
melonDS decodes the same banks into its own flat views via GPU::MapVRAM_*.
"""
import os
import sys

S = 0x195400


def first_diffs(a, b, limit=6):
    out = []
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            out.append(i)
            if len(out) >= limit:
                break
    return out


def cmp_bytes(name, a, b):
    nz_a = sum(1 for x in a if x)
    nz_b = sum(1 for x in b if x)
    same = a == b
    nd = sum(1 for x, y in zip(a, b) if x != y) if not same else 0
    print('%-10s ours(nz=%7d) melon(nz=%7d) %s  differing=%d  first=%s' % (
        name, nz_a, nz_b, 'EQUAL' if same else 'DIFF', nd,
        ''.join('+0x%X ' % o for o in first_diffs(a, b)) if not same else ''))
    return same


def main(state, meldir):
    d = open(state, 'rb').read()
    ok = 0
    for nm, off, size in (('palette', 0x400, 0x800), ('oam', 0xC00, 0x800)):
        mel = open(os.path.join(meldir, nm + '.bin'), 'rb').read()
        ours = d[off:off + size]
        ok += cmp_bytes(nm, ours, mel)
    sizes = [0x20000] * 4 + [0x10000, 0x4000, 0x4000, 0x8000, 0x4000]
    for i, nm in enumerate('ABCDEFGHI'):
        mel = open(os.path.join(meldir, 'vram%s.bin' % nm), 'rb').read()
        ours = d[S + i * 0x20000:S + i * 0x20000 + sizes[i]]
        ok += cmp_bytes('vram' + nm, ours, mel)
    print('%d/%d regions identical' % (ok, 11))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
