#!/usr/bin/env python3
"""Locate the Ni no Kuni Mobiclip movie player in a main-RAM dump and render
its decoded frame planes (Phase-5P).

The movie player is an SDK object found in BOTH engines at the same address:

    0x02278268  "MODS" container header  (u32 LE: w, h, frames at +0x0C/+0x10/+0x08)
    0x02278264  player object, fields at 0x0227829C:
        +0x00 decode callback (ITCM, 0x01FF8540)
        +0x04 current 8-bit 256x192 frame buffer
        +0x08 next frame buffer        (current+0xC010)
        +0x0C VRAM destination, 16bpp
        +0x10 stride in bytes (0x200)
        +0x14 width (0x100), +0x18 height (0xC0)

    python3 p5p_movie_frames.py <dump.bin> <base> <outdir>

`<dump.bin>` is a raw main-RAM dump whose guest base is <base> (native gmem is
0x02000000-based; melonDS RAMDump output is the same layout, and, with the
Phase-5P change, carries the 32 KiB ITCM appended).  For every frame buffer
found in the 6-slot ring (0x12010 apart) the tool writes <outdir>/plane_*.png
(the raw 8-bit plane) and reports mean/variance/quadrant statistics, which is
how the native decode (plateau/noise) is distinguished from melonDS's (image).
"""
import os
import struct
import sys

W, H = 256, 192
PLANE = W * H          # 0xC000, one 8-bit frame
STEP = 0xC010          # player's current->next stride
RING_STEP = 0x12010    # ring slots


def u32(d, off):
    return struct.unpack_from("<I", d, off)[0]


def quad_stats(plane):
    out = []
    for (y0, y1, x0, x1) in ((0, H // 2, 0, W // 2), (0, H // 2, W // 2, W),
                             (H // 2, H, 0, W // 2), (H // 2, H, W // 2, W)):
        vals = [plane[j * W + i] for j in range(y0, y1) for i in range(x0, x1)]
        m = sum(vals) / float(len(vals))
        var = sum((v - m) ** 2 for v in vals) / float(len(vals))
        out.append((m, var))
    flat = sum(1 for j in range(0, H, 8) for i in range(0, W, 8)
               if len(set(plane[(j + b) * W + i + a] for b in range(8) for a in range(8))) == 1)
    return out, flat


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    path, base, outdir = sys.argv[1], int(sys.argv[2], 0), sys.argv[3]
    d = open(path, "rb").read()
    os.makedirs(outdir, exist_ok=True)

    i = d.find(b"MODS")
    while i >= 0:
        w, h, nf = u32(d, i + 0x0C), u32(d, i + 0x10), u32(d, i + 0x08)
        print("MODS header @0x%08X w=%d h=%d frames=%d" % (base + i, w, h, nf))
        i = d.find(b"MODS", i + 1)

    # player object: the callback word is field +0x00 of the object whose
    # +0x04/+0x08/+0x0C are the frame source pointers and the VRAM destination.
    players = []
    j = d.find(struct.pack("<I", 0x01FF8540))
    while j >= 0:
        players.append(j)
        j = d.find(struct.pack("<I", 0x01FF8540), j + 1)

    for obj in players:
        if obj + 0x1C > len(d):
            continue
        src, nxt, dst = u32(d, obj + 4), u32(d, obj + 8), u32(d, obj + 0x0C)
        stride, w, h = u32(d, obj + 0x10), u32(d, obj + 0x14), u32(d, obj + 0x18)
        if not (0x02000000 <= src < base + len(d)):
            continue
        print("player @0x%08X cb=0x%08X src=0x%08X next=0x%08X dst=0x%08X "
              "stride=0x%X %dx%d (next-src=0x%X)" %
              (base + obj, u32(d, obj), src, nxt, dst, stride, w, h, nxt - src))
        for k in range(6):
            a = src - k * RING_STEP
            off = a - base
            if off < 0 or off + PLANE > len(d):
                continue
            plane = d[off:off + PLANE]
            quads, flat = quad_stats(plane)
            print("   ring[%d] 0x%08X mean=%.0f quads=%s const8x8=%d" %
                  (k, a, sum(plane) / float(PLANE),
                   " ".join("[%.0f/%.0f]" % q for q in quads), flat))
            try:
                from PIL import Image
            except ImportError:
                continue
            im = Image.new("L", (W, H))
            im.putdata(list(plane))
            im.resize((W * 2, H * 2), Image.NEAREST).save(
                os.path.join(outdir, "plane_%08X.png" % a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
