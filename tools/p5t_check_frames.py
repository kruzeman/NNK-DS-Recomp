#!/usr/bin/env python3
"""Phase-5T: identify the frame each captured screen shows, and export PNGs.

usage: p5t_check_frames.py <movie.yuv> <ppm-dir-or-glob-prefix> <png-out-dir> [tag]
For every *.top.ppm capture: nearest reference frame by luma correlation,
MAE of luma vs the reference Y plane, mean RGB of the capture, and a PNG copy.
"""
import sys, os, glob, zlib, struct

W, H = 256, 192
FRAME = 73728


def read_ppm(path):
    d = open(path, 'rb').read()
    assert d[:2] == b'P6', d[:2]
    # P6\n<w> <h>\n<max>\n
    pos = 2
    vals = []
    while len(vals) < 3:
        while pos < len(d) and d[pos:pos + 1].isspace():
            pos += 1
        if d[pos:pos + 1] == b'#':
            while d[pos:pos + 1] not in (b'\n', b''):
                pos += 1
            continue
        s = pos
        while pos < len(d) and not d[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(d[s:pos]))
    pos += 1
    w, h, mx = vals
    return w, h, d[pos:pos + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(typ, data):
        c = struct.pack('>I', len(data)) + typ + data
        return c + struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF)
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6))
    png += chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def corr(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    den = ((n * saa - sa * sa) * (n * sbb - sb * sb)) ** 0.5
    return (n * sab - sa * sb) / den if den else 0.0


STEP = 1


def main():
    yuvp, pat, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    tag = sys.argv[4] if len(sys.argv) > 4 else "p5t"
    global STEP
    if len(sys.argv) > 5: STEP = int(sys.argv[5])
    os.makedirs(outdir, exist_ok=True)
    d = open(yuvp, 'rb').read()
    nf = len(d) // FRAME
    files = sorted(glob.glob(pat))
    if not files:
        print('no captures match', pat)
        return
    for f in files:
        w, h, rgb = read_ppm(f)
        if (w, h) != (W, H):
            sd = 2 if (w, h) == (W * 2, H * 2) else 1
            rgb2 = bytes(rgb[(y * sd) * w * 3 + (x * sd) * 3 + c]
                         for y in range(H) for x in range(W) for c in range(3))
            rgb, w, h = rgb2, W, H
        gray = bytes((rgb[i * 3] * 77 + rgb[i * 3 + 1] * 150 + rgb[i * 3 + 2] * 29) >> 8
                     for i in range(W * H))
        best = (-2.0, -1, 0.0)
        for fr in range(0, nf, STEP):
            y = d[fr * FRAME:fr * FRAME + W * H]
            c = corr(y, gray)
            if c > best[0]:
                mae = sum(abs(a - b) for a, b in zip(y, gray)) / (W * H)
                best = (c, fr, mae)
        m = [sum(rgb[i * 3 + c] for i in range(W * H)) / (W * H) for c in range(3)]
        name = os.path.basename(f).split('.')[1]
        out = os.path.join(outdir, '%s.%s_top.png' % (tag, name))
        write_png(out, W, H, rgb)
        print('%s  best ref f%04d  corr %.4f  lumaMAE %.1f  meanRGB %3.0f/%3.0f/%3.0f -> %s'
              % (os.path.basename(f), best[1], best[0], best[2], m[0], m[1], m[2], out))


main()
