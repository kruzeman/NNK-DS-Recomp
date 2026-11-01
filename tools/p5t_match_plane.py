#!/usr/bin/env python3
"""Phase-5T: identify the decoded-plane layout by matching the melonDS 8-bit
plane dump (Phase-5P) against the FFmpeg-decoded op_d.mods Y planes.

usage: p5t_match_plane.py <op_d.yuv> <melonDS plane png> [max_frame]
Prints the best-matching frame indices by Pearson correlation and MAE.
"""
import sys, struct, zlib

W, H = 256, 192
FRAME = 73728


def png_gray(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8
    w = h = bd = ct = None
    idat = b''
    while pos < len(d):
        ln = struct.unpack_from('>I', d, pos)[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack_from('>IIBB', body, 0)
        elif typ == b'IDAT':
            idat += body
        pos += 12 + ln
    raw = zlib.decompress(idat)
    nch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    assert bd == 8, bd
    stride = w * nch
    out = bytearray(w * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        if nch == 1:
            out[y * w:(y + 1) * w] = line
        else:
            for x in range(w):
                r, g, b = line[x * nch], line[x * nch + 1], line[x * nch + 2]
                out[y * w + x] = (r * 77 + g * 150 + b * 29) >> 8
        prev = line
    return w, h, bytes(out)


def corr(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    num = n * sab - sa * sb
    den = ((n * saa - sa * sa) * (n * sbb - sb * sb)) ** 0.5
    return num / den if den else 0.0


def main():
    yuv, png = sys.argv[1], sys.argv[2]
    maxf = int(sys.argv[3]) if len(sys.argv) > 3 else 400
    w, h, mel = png_gray(png)
    print('melonDS plane png %dx%d' % (w, h))
    if (w, h) == (W * 2, H * 2):   # 2x nearest-neighbour review render
        mel = bytes(mel[(y * 2) * w + (x * 2)] for y in range(H) for x in range(W))
        w, h = W, H
        print('  -> downsampled 2x to %dx%d' % (w, h))
    if (w, h) != (W, H):
        print('dimension mismatch - resampling not supported')
        return
    data = open(yuv, 'rb').read()
    nframes = len(data) // FRAME
    mmean = sum(mel) / len(mel)
    mstd = (sum((x - mmean) ** 2 for x in mel) / len(mel)) ** 0.5
    print('melonDS plane: mean %.1f std %.1f' % (mmean, mstd))
    res = []
    for f in range(min(maxf, nframes)):
        y = data[f * FRAME:f * FRAME + W * H]
        c = corr(y, mel)
        mae = sum(abs(p - q) for p, q in zip(y, mel)) / len(y)
        res.append((c, -mae, f, mae))
    res.sort(reverse=True)
    print('best Y-plane matches (frame, corr, MAE):')
    for c, _, f, mae in res[:8]:
        print('  f%04d  corr %.4f  MAE %.1f' % (f, c, mae))
    # control: correlation of the mel plane with itself shifted
    print('worst:', ' f%04d corr %.4f MAE %.1f' % (res[-1][2], res[-1][0], res[-1][3]))


main()
