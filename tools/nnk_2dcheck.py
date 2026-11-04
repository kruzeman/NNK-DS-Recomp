#!/usr/bin/env python3
"""nnk_2dcheck.py — diff OUR 2D state dump against the melonDS reference.

usage: nnk_2dcheck.py <nnk_state_dump.bin> <mel_ref_dir> [outdir]

<nnk_state_dump.bin> is what NNK_GX_STATE_DUMP writes (gx_display.c state_dump):
  +0x000000 0x195400  the GX2D state image (regs, global, palette, OAM, flats)
  +0x195400 9 x 0x20000  the raw VRAM banks A..I (zero padded)

It prints, per engine, the 2D registers; byte-diffs palette / ABG / BBG /
engine-B-OBJ / ext-palette flats and the raw banks against the melonDS state;
and renders our per-BG layers to <outdir> for eyeballing against the
melds_bg.py reference layers.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import melds_bg  # noqa: E402  (render_engine / regs reader reused)

IMG_PAL, IMG_OAM = 0x400, 0xC00
IMG_ABG, IMG_BBG, IMG_AOBJ, IMG_BOBJ = 0x1400, 0x81400, 0xA1400, 0xE1400
IMG_ABGEXT, IMG_BBGEXT = 0x101400, 0x109400
IMG_RAW = 0x115400
BANK = 0x20000


def regs(img, n):
    b = n * 0x100
    dispcnt = struct.unpack_from('<I', img, b)[0]
    bgcnt = list(struct.unpack_from('<4H', img, b + 4))
    xpos = list(struct.unpack_from('<4H', img, b + 0x0C))
    ypos = list(struct.unpack_from('<4H', img, b + 0x14))
    win = img[b + 0x4C:b + 0x58]
    return dispcnt, bgcnt, xpos, ypos, win


def diff(tag, a, b):
    n = min(len(a), len(b))
    bad = sum(1 for i in range(n) if a[i] != b[i])
    first = next((i for i in range(n) if a[i] != b[i]), -1)
    na = sum(1 for i in range(n) if a[i])
    nb = sum(1 for i in range(n) if b[i])
    print('  %-10s %s (%d/%d bytes differ%s) nonzero ours=%d ref=%d'
          % (tag, 'EQUAL' if not bad else 'DIFF',
             bad, n, '' if first < 0 else ', first @0x%x' % first, na, nb))


def main(argv):
    path, ref = argv[1], argv[2]
    outdir = argv[3] if len(argv) > 3 else '/tmp/ours5bo'
    os.makedirs(outdir, exist_ok=True)
    d = open(path, 'rb').read()
    img = d[:IMG_RAW]
    banks = d[IMG_RAW:]
    print('dump %d bytes; img %d banks %d' % (len(d), len(img), len(banks)))

    ref_eng = melds_bg.read_engines(os.path.join(ref, '..', 'melonds_oliver.mln')) \
        if os.path.exists(os.path.join(ref, '..', 'melonds_oliver.mln')) else None
    cnts, blocks = melds_bg.read_maps(ref)
    ref_abg, ref_bbg = melds_bg.flat_window(ref, cnts, blocks)
    ref_pal = open(os.path.join(ref, 'palette.bin'), 'rb').read()

    print('== engine registers (ours vs melonDS)')
    for n in range(2):
        dispcnt, bgcnt, xpos, ypos, win = regs(img, n)
        print('  engine %d DISPCNT=0x%08x BGCNT=%s xpos=%s ypos=%s win=%s'
              % (n, dispcnt, ' '.join('0x%04x' % v for v in bgcnt),
                 xpos, ypos, list(win)))
        if ref_eng:
            rdisp, rbg = ref_eng[n]
            ok = 'MATCH' if (rdisp == dispcnt and rbg == bgcnt) else 'DIFF'
            print('    reference    DISPCNT=0x%08x BGCNT=%s -> %s'
                  % (rdisp, ' '.join('0x%04x' % v for v in rbg), ok))

    print('== memory (ours vs melonDS)')
    diff('palette', img[IMG_PAL:IMG_PAL + 2048], ref_pal)
    diff('ABG', img[IMG_ABG:IMG_ABG + 0x80000], ref_abg)
    diff('BBG', img[IMG_BBG:IMG_BBG + 0x20000], ref_bbg)
    for i, name in enumerate('ABCDEFGHI'):
        r = open(os.path.join(ref, 'vram%s.bin' % name), 'rb').read()
        off = i * BANK
        diff('bank %s' % name, banks[off:off + len(r)], r)

    print('== ext palettes')
    ref_bbgext = melds_bg.build_flat(blocks, 'BBGEXT', 0x2000, 0x8000,
                                     {n: open(os.path.join(ref, 'vram%s.bin' % n), 'rb').read()
                                      for n in 'ABCDEFGHI'})
    diff('BBGEXT', img[IMG_BBGEXT:IMG_BBGEXT + 0x8000], ref_bbgext)
    ref_aobj = melds_bg.build_flat(blocks, 'AOBJ', 0x4000, 0x40000,
                                   {n: open(os.path.join(ref, 'vram%s.bin' % n), 'rb').read()
                                    for n in 'ABCDEFGHI'})
    diff('AOBJ', img[IMG_AOBJ:IMG_AOBJ + 0x40000], ref_aobj)
    diff('OAM', img[IMG_OAM:IMG_OAM + 2048],
         open(os.path.join(ref, 'oam.bin'), 'rb').read())

    print('== our per-BG layers -> %s' % outdir)
    pal = img[IMG_PAL:IMG_PAL + 2048]
    for n in range(2):
        dispcnt, bgcnt, _, _, _ = regs(img, n)
        lines = melds_bg.render_engine(n, dispcnt, bgcnt, None if n else img[IMG_ABG:IMG_ABG + 0x80000],
                                       img[IMG_BBG:IMG_BBG + 0x20000],
                                       pal, outdir)
        for l in lines:
            print('  ' + l)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
