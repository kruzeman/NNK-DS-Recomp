#!/usr/bin/env python3
"""melds_bg.py — decode melonDS GPU state (from tools/melds_gpu.py output dir)
into per-layer PPMs for the two 2D engines, so a reference screen can be
attributed layer by layer.

usage: melds_bg.py <mel_ref_dir> <outdir>

For each engine (0 = A/main, 1 = B/sub) and each of the 4 BGs it writes
  <outdir>/e<engine>.bg<n>.ppm      256x192, tile fetch per BGxCNT
  <outdir>/e<engine>.bg<n>.txt      coverage stats + the first tilemap entries

The BG window bytes come from the flat views the melonDS banks imply:
  engine A BG = banks mapped to ABG (VRAMCNT MST 1/2/3), 512K window
  engine B BG = banks mapped to BBG (VRAMCNT MST 4 for C/D, MST 1 for H/I)
This mirrors GPU::MapVRAM_* of melonDS 1.1; it exists to answer "which layer
draws the picture" without a melonDS build.
"""
import os
import struct
import sys

SIZES = [0x20000] * 4 + [0x10000] + [0x4000] * 3 + [0x8000] + [0x4000]

# DISPCNT / BGxCNT per engine, read by tools/melds_2dregs.py; duplicated here
# so this tool is standalone (it re-reads the GP2A/GP2B sections itself).
def find_section(d, magic, start=0x10):
    off = start
    while off + 16 <= len(d):
        if d[off:off + 4] == magic:
            return off
        seclen = struct.unpack_from('<I', d, off + 4)[0]
        if seclen < 16:
            break
        off += seclen
    return None


def read_engines(state):
    d = open(state, 'rb').read()
    out = []
    for magic in (b'GP2A', b'GP2B'):
        off = find_section(d, magic)
        p = off + 16
        dispcnt = struct.unpack_from('<I', d, p)[0]
        bgcnt = list(struct.unpack_from('<4H', d, p + 4))
        out.append((dispcnt, bgcnt))
    return out


def read_maps(ref):
    """melonDS's own VRAMMap_* block maps from maps.txt (authoritative).

    Each entry is a bank bitmask (bit i = bank 'ABCDEFGHI'[i]); block sizes are
    16K for ABG/BBG/AOBJ/BOBJ and 8K for the ext-pal maps."""
    cnts, blocks = None, {}
    for line in open(os.path.join(ref, 'maps.txt')):
        parts = line.split()
        if parts[0] == 'VRAMCNT':
            cnts = [int(x, 16) for x in parts[1:10]]
        elif len(parts) > 1 and parts[0] in (
                'ABG', 'AOBJ', 'BBG', 'BOBJ', 'ABGEXT', 'BBGEXT',
                'AOBJEXT', 'BOBJEXT', 'TEX', 'TEXPAL'):
            blocks[parts[0]] = [int(x, 16) for x in parts[1:]]
    return cnts, blocks


def build_flat(blocks, name, blk, phys, banks):
    out = bytearray(blk * len(blocks[name]))
    for b, mask in enumerate(blocks[name]):
        if not mask:
            continue
        for i in range(9):
            if not (mask & (1 << i)):
                continue
            src = banks['ABCDEFGHI'[i]]
            for k in range(blk):
                out[b * blk + k] |= src[(b * blk + k) & (len(src) - 1)]
    return bytes(out)


def flat_window(d, cnts, blocks):
    """flat engine windows for engine A BG and engine B BG, per melonDS maps"""
    banks = {}
    for name in 'ABCDEFGHI':
        banks[name] = open(os.path.join(d, 'vram%s.bin' % name), 'rb').read()
    abg = build_flat(blocks, 'ABG', 0x4000, 0x80000, banks)
    bbg = build_flat(blocks, 'BBG', 0x4000, 0x20000, banks)
    return abg, bbg


def u16(buf, off):
    return buf[off] | (buf[off + 1] << 8)


def render_engine(eng, dispcnt, bgcnt, abg, bbg, pal, outdir):
    num = eng
    win = bbg if num == 1 else abg
    wmask = len(win) - 1
    paloff = 0x400 if num == 1 else 0     # byte offset into palette.bin
    lines = []
    for bgnum in range(4):
        cnt = bgcnt[bgnum]
        if not (dispcnt & (0x100 << bgnum)):
            lines.append('BG%d cnt=0x%04x DISABLED' % (bgnum, cnt))
            continue
        if (dispcnt & 7) != 0:
            lines.append('BG%d cnt=0x%04x mode!=0 (not decoded here)' % (bgnum, cnt))
            continue
        if num == 0:
            tilebase = ((dispcnt & 0x07000000) >> 8) + ((cnt & 0x3C) << 12)
            mapbase = ((dispcnt & 0x38000000) >> 11) + ((cnt & 0x1F00) << 3)
        else:
            tilebase = (cnt & 0x3C) << 12
            mapbase = (cnt & 0x1F00) << 3
        colors = 256 if (cnt & 0x80) else 16
        prio = cnt & 3
        size = (cnt >> 14) & 3
        sb = 256 if (size & 1) else 256
        px = bytearray(256 * 192 * 3)
        lit = 0
        entries = []
        for y in range(192):
            for x in range(256):
                tx, ty = x >> 3, y >> 3
                if size & 1:
                    tx &= 0xFF
                if size & 2:
                    ty &= 0xFF
                if size & 1 and tx >= 32:
                    off = mapbase + 0x800 + ((ty & 31) << 5) + (tx & 31)
                elif size & 2 and ty >= 32:
                    off = mapbase + (((ty - 32) & 31) << 6) + (tx << 1)
                else:
                    off = mapbase + ((ty & 31) << 6) + ((tx & 31) << 1)
                e = u16(win, off & wmask)
                if y == 0 and x < 64 and x % 8 == 0:
                    entries.append('%d:%04x' % (x // 8, e))
                t = e & 0x3FF
                fy = y & 7
                if e & 0x800:
                    fy = 7 - fy
                fx = x & 7
                if e & 0x400:
                    fx = 7 - fx
                if colors == 256:
                    a = tilebase + (t << 6) + (fy << 3) + fx
                    c = win[a & wmask]
                    p = paloff + c * 2 if c else None
                else:
                    a = tilebase + (t << 5) + ((fy << 2) + (fx >> 1))
                    b = win[a & wmask]
                    c = (b >> 4) if (fx & 1) else (b & 0xF)
                    p = paloff + (e >> 12) * 32 + c * 2 if c else None
                if p is not None:
                    v = u16(pal, p)
                    r = (v & 0x1F) << 3
                    g = ((v >> 5) & 0x1F) << 3
                    bl = ((v >> 10) & 0x1F) << 3
                    px[(y * 256 + x) * 3] = r
                    px[(y * 256 + x) * 3 + 1] = g
                    px[(y * 256 + x) * 3 + 2] = bl
                    lit += 1
        with open(os.path.join(outdir, 'e%d.bg%d.ppm' % (num, bgnum)), 'wb') as f:
            f.write(b'P6\n256 192\n255\n' + bytes(px))
        lines.append('BG%d cnt=0x%04x prio=%d tilebase=0x%05x mapbase=0x%05x '
                     'colors=%d size=%d extpalbit=%d lit=%d/49152 (%.1f%%) '
                     'map[0..]: %s'
                     % (bgnum, cnt, prio, tilebase, mapbase, colors, size,
                        (cnt >> 13) & 1, lit, lit * 100.0 / (256 * 192),
                        ' '.join(entries[:8])))
    return lines


def main(argv):
    ref, outdir = argv[1], argv[2]
    state = argv[3] if len(argv) > 3 else None
    os.makedirs(outdir, exist_ok=True)
    engs = read_engines(state)
    cnts, blocks = read_maps(ref)
    abg, bbg = flat_window(ref, cnts, blocks)
    pal = open(os.path.join(ref, 'palette.bin'), 'rb').read()
    all_lines = ['VRAMCNT=' + ' '.join('%02x' % c for c in cnts)]
    for num, (dispcnt, bgcnt) in enumerate(engs):
        all_lines.append('== engine %d DISPCNT=0x%08x' % (num, dispcnt))
        all_lines += render_engine(num, dispcnt, bgcnt, abg, bbg, pal, outdir)
    txt = '\n'.join(all_lines)
    open(os.path.join(outdir, 'layers.txt'), 'w').write(txt + '\n')
    print(txt)


def vramcnt_from_state(state):
    d = open(state, 'rb').read()
    gsec = find_section(d, b'GPUG')
    p = gsec + 16
    # GPU::DoSavestate field order (melds_gpu.py): VCount u16, NextVCount u32,
    # TotalScanlines u32, DispStat A/B u16, VMatch A/B u16, then the 9 VRAMCNT
    # bytes; melds_gpu.py prints them, so trust its numbers and fall back to
    # the printed line if this offset ever drifts.
    p += 2 + 4 + 4 + 2 + 2 + 2 + 2
    return list(d[p:p + 9])


d_global = b''
if __name__ == '__main__':
    sys.exit(main(sys.argv))
