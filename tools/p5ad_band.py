#!/usr/bin/env python3
"""Phase-5AD: band audit for the movie window (stdlib only).

For every *.{top,bot}.ppm capture in a shot dir, find the reference YUV frame of
a movie stream with the best luma correlation (a stride-8 coarse pass over the
whole stream, then a stride-2 refine of the best 5), report corr/MAE, and emit a
side-by-side PNG (native RGB | reference luma) for the TOP screen of each
capture.

usage: p5ad_band.py <shot-dir> <movie.yuv> <png-dir> <f0> <f1> [prefix] [nref] [screen]

`screen` selects which capture the side-by-side PNGs are made from (`top`
default, or `bot`); both screens are always correlated and reported.  Audit the
bottom screen against NNK_MOVIE_YUV2's stream (op_d), not op_u.
"""
import os
import re
import struct
import sys
import zlib

W, H = 256, 192
FRAME = 73728
YSZ = W * H
COARSE = [(y * W + x) for y in range(0, H, 8) for x in range(0, W, 8)]
FINE = [(y * W + x) for y in range(0, H, 2) for x in range(0, W, 2)]


def read_ppm(path):
    with open(path, 'rb') as f:
        d = f.read()
    assert d.startswith(b'P6'), path
    i, fields = 2, []
    while len(fields) < 3:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, d[i:i + w * h * 3]


def gray(w, h, px):
    """BT.601 luma (byte 1 of each RGB triple) -> list of W*H ints."""
    return [px[(y * w + x) * 3 + 1] for y in range(h) for x in range(w)]


def corr(a, b, idx):
    n = len(idx)
    sa = sum(a[i] for i in idx)
    sb = sum(b[i] for i in idx)
    ma = sa / n
    mb = sb / n
    num = da = db = 0.0
    for i in idx:
        x = a[i] - ma
        y = b[i] - mb
        num += x * y
        da += x * x
        db += y * y
    if da <= 0 or db <= 0:
        return 0.0
    return num / (da ** 0.5 * db ** 0.5)


def mae(a, b, idx):
    return sum(abs(a[i] - b[i]) for i in idx) / len(idx)


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data +
                struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) +
                chunk(b'IEND', b''))


def read_frame(fp, i):
    fp.seek(i * FRAME)
    d = fp.read(YSZ)
    return list(d) if len(d) == YSZ else None


def main():
    sd, yuv, od = sys.argv[1], sys.argv[2], sys.argv[3]
    f0, f1 = int(sys.argv[4]), int(sys.argv[5])
    prefix = sys.argv[6] if len(sys.argv) > 6 else 'p5ad'
    maxref = int(sys.argv[7]) if len(sys.argv) > 7 else 600
    want = sys.argv[8] if len(sys.argv) > 8 else 'top'
    os.makedirs(od, exist_ok=True)
    shot_re = re.compile(r'^.*?\.(\d+)\.(top|bot)\.ppm$')
    files = {}
    for name in sorted(os.listdir(sd)):
        m = shot_re.match(name)
        if m:
            files.setdefault(int(m.group(1)), {})[m.group(2)] = os.path.join(sd, name)
    band = [f for f in sorted(files) if f0 <= f <= f1]
    if not band:
        raise SystemExit('no shots in [%d,%d] (sampled: %s)' % (f0, f1, sorted(files)[:6]))
    with open(yuv, 'rb') as fp:
        fp.seek(0, 2)
        n = fp.tell() // FRAME
        nr = min(n, maxref)
        print('ref: %d frames (of %d) from %s' % (nr, n, yuv))
        for fr in band:
            for scr in ('top', 'bot'):
                p = files[fr].get(scr)
                if not p:
                    continue
                w, h, px = read_ppm(p)
                nat = gray(w, h, px)
                scores = []
                for i in range(nr):
                    y = read_frame(fp, i)
                    if y is not None:
                        scores.append((corr(nat, y, COARSE), i))
                scores.sort(reverse=True)
                best = (-2.0, -1, 0.0, None)
                for _, i in scores[:5]:
                    y = read_frame(fp, i)
                    c = corr(nat, y, FINE)
                    if c > best[0]:
                        best = (c, i, mae(nat, y, FINE), y)
                mean = sum(nat) / len(nat)
                sdv = (sum((v - mean) ** 2 for v in nat) / len(nat)) ** 0.5
                print('f%05d.%s  mean %5.1f sd %5.1f  best ref f%04d corr %.4f MAE %5.2f'
                      % (fr, scr, mean, sdv, best[1], best[0], best[2]))
                sys.stdout.flush()
                if scr == want and w == W and h == H and best[3] is not None:
                    ref = best[3]
                    lw = W * 2 + 4
                    out = bytearray(lw * H * 3)
                    for y in range(H):
                        row = y * lw * 3
                        out[row:row + W * 3] = px[y * W * 3:(y + 1) * W * 3]
                        for x in range(W):
                            v = ref[y * W + x]
                            o = row + (W + 4 + x) * 3
                            out[o] = out[o + 1] = out[o + 2] = v
                    write_png(os.path.join(od, '%s_top_f%04d_vs_ref%04d_c%04d.png'
                                           % (prefix, fr, best[1], int(best[0] * 10000))),
                              lw, H, out)


if __name__ == '__main__':
    main()
