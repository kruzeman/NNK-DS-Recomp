#!/usr/bin/env python3
"""synth_state.py — build a GX2D state image that carries melonDS's OWN GPU
data (palette, OAM, VRAM banks, bank maps, 2D registers) inside the container
our offline probe (tools/gx2d_probe.c) reads.

WHY: the operator's melonDS savestate is the ground truth of the "Skip the
cutscene?" frame.  Feeding its GPU bytes through our renderer separates "our
renderer misinterprets the hardware state" from "our guest/upload path put
different bytes in VRAM": if the render is right, the renderer is exonerated.

usage: synth_state.py <our-state.bin> <melodir> <mel2dregs-output> <out.bin>
"""
import os
import struct
import sys

STATE = 0x195400
BANK = 0x20000
BANKSIZE = [0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000,
            0x8000, 0x4000]
VIEWS = [('ABG', 0x1400, 0x80000, 0x4000, 32),
         ('BBG', 0x81400, 0x20000, 0x4000, 8),
         ('AOBJ', 0xA1400, 0x40000, 0x4000, 16),
         ('BOBJ', 0xE1400, 0x20000, 0x4000, 8),
         ('ABGEXT', 0x101400, 0x8000, 0x2000, 4),
         ('BBGEXT', 0x109400, 0x8000, 0x2000, 4)]


def parse_maps(path):
    maps = {}
    for line in open(path):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == 'VRAMCNT' or parts[0] == 'LCDC':
            continue
        maps[parts[0]] = [int(x, 16) for x in parts[1:]]
    return maps


def main(our, meldir, regs, out):
    d = bytearray(open(our, 'rb').read())
    banks = []
    for i, nm in enumerate('ABCDEFGHI'):
        b = open(os.path.join(meldir, 'vram%s.bin' % nm), 'rb').read()
        assert len(b) == BANKSIZE[i], (nm, len(b))
        banks.append(b.ljust(BANK, b'\0'))

    d[0x400:0xC00] = open(os.path.join(meldir, 'palette.bin'), 'rb').read()
    d[0xC00:0x1400] = open(os.path.join(meldir, 'oam.bin'), 'rb').read()
    for i in range(9):
        d[STATE + i * BANK:STATE + (i + 1) * BANK] = banks[i]

    maps = parse_maps(os.path.join(meldir, 'maps.txt'))
    for name, off, size, blk, nblk in VIEWS:
        view = bytearray(size)
        mask_list = maps[name]
        for b in range(nblk):
            mask = mask_list[b]
            for bank in range(9):
                if not (mask & (1 << bank)):
                    continue
                src = banks[bank]
                for i in range(blk):
                    a = b * blk + i
                    view[a] |= src[a & (BANKSIZE[bank] - 1)]
        d[off:off + size] = view
    # AOBJEXT / BOBJEXT (single 8K views, mask in one word)
    for name, off in (('AOBJEXT', 0x111400), ('BOBJEXT', 0x113400)):
        view = bytearray(0x2000)
        mask = maps[name][0]
        for bank in range(9):
            if mask & (1 << bank):
                src = banks[bank]
                for i in range(0x2000):
                    view[i] |= src[i & (BANKSIZE[bank] - 1)]
        d[off:off + 0x2000] = view

    # 2D registers from melonDS: engine A dispcnt, blend, master brightness
    for line in open(regs):
        f = line.split()
        if not f or f[0] not in ('GP2A', 'GP2B'):
            continue
        eng = 0 if f[0] == 'GP2A' else 1
        base = eng * 0x100
        kv = {}
        for tok in f[1:]:
            if '=' in tok:
                k, v = tok.split('=', 1)
                kv[k] = v
        struct.pack_into('<I', d, base, int(kv['DISPCNT'], 16))
        for i, v in enumerate(kv['BGCNT'].split()):
            pass
        struct.pack_into('<H', d, base + 0x5C, int(kv['blend'], 16))
        struct.pack_into('<H', d, base + 0x5E, int(kv['alpha'], 16))
        d[base + 0x60] = int(kv['eva'])
        d[base + 0x61] = int(kv['evb'])
        d[base + 0x62] = int(kv['evy'])
        struct.pack_into('<H', d, 0x200 + eng * 2, int(kv['mbright'], 16))
    open(out, 'wb').write(d)
    print('wrote', out)


if __name__ == '__main__':
    main(*sys.argv[1:5])
