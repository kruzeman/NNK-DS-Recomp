#!/usr/bin/env python3
"""Phase-5T: match a melonDS composed top-screen capture against the
FFmpeg-decoded movie frames, trying both YCoCg (MODS) and YCbCr colour maths.

usage: p5t_match_screen.py <movie.yuv> <screen.png> [max_frame] [--555]
Prints the best matches (frame, corr, MAE) for each colour matrix.
"""
import sys, struct, zlib

W, H = 256, 192
FRAME = 73728


def png_rgb(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat = 8, b''
    w = h = bd = ct = 0
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
    stride = w * nch
    prev = bytearray(stride)
    out = []
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
        row = []
        for x in range(w):
            if nch == 1:
                v = line[x]; row.append((v, v, v))
            else:
                row.append((line[x * nch], line[x * nch + 1], line[x * nch + 2]))
        out.append(row)
        prev = line
    return w, h, out


def downscale2(w, h, rows):
    return bytes(v for y in range(0, h, 2) for x in range(0, w, 2)
                 for v in rows[y][x])


def frame_bytes_to_planes(buf):
    y = buf[:W * H]
    u = buf[W * H:W * H + (W // 2) * (H // 2)]
    v = buf[W * H + (W // 2) * (H // 2):]
    return y, u, v


def clamp8(x):
    return 0 if x < 0 else (255 if x > 255 else x)


def conv(y, u, v, mode):
    """Return a list of (r,g,b) 0..255 for every pixel, 4:2:0 upsample nearest."""
    out = [None] * (W * H)
    cw = W // 2
    for j in range(H):
        row = j * W
        cj = (j >> 1) * cw
        for i in range(W):
            Y = y[row + i]
            ci = cj + (i >> 1)
            c1 = u[ci]
            c2 = v[ci]
            if mode == 'ycocg':
                co = c1 - 128
                cg = c2 - 128
                r = Y + co - cg
                g = Y + cg
                b = Y - co - cg
            elif mode == 'ycocg_alt':
                co = c1 - 128
                cg = c2 - 128
                r = Y + co - cg
                g = Y + cg
                b = Y - co - cg
                r, b = b, r
            else:  # bt601
                d = c1 - 128; e = c2 - 128
                r = Y + 1.402 * e
                g = Y - 0.344136 * d - 0.714136 * e
                b = Y + 1.772 * d
            out[row + i] = (clamp8(int(r)), clamp8(int(g)), clamp8(int(b)))
    return out


def corr(a, b):
    n = len(a)
    sa = sum(a); sb = sum(b)
    saa = sum(x * x for x in a); sbb = sum(x * x for x in b)
    sab = sum(x * y for x, y in zip(a, b))
    num = n * sab - sa * sb
    den = ((n * saa - sa * sa) * (n * sbb - sb * sb)) ** 0.5
    return num / den if den else 0.0


def main():
    yuvp, screen, *rest = sys.argv[1:]
    maxf = int(rest[0]) if rest else 1317
    stride = int(rest[1]) if len(rest) > 1 else 1
    w, h, rows = png_rgb(screen)
    print('screen %dx%d' % (w, h))
    scr = downscale2(w, h, rows) if (w, h) == (W * 2, H * 2) else bytes(
        v for y in range(min(h, H)) for x in range(min(w, W)) for v in rows[y][x])
    if (w, h) == (W * 2, H * 2):
        print('  downsampled 2x')
    data = open(yuvp, 'rb').read()
    n = len(data) // FRAME
    print('movie frames: %d' % n)
    for mode in ('ycocg', 'bt601'):
        res = []
        for f in range(0, min(maxf, n), stride):
            buf = data[f * FRAME:(f + 1) * FRAME]
            y, u, v = frame_bytes_to_planes(buf)
            px = conv(y, u, v, mode)
            g = bytes(v_ for p in px for v_ in p)
            c = corr(g, scr)
            mae = sum(abs(a - b) for a, b in zip(g, scr)) / len(g)
            res.append((c, -mae, f, mae))
        res.sort(reverse=True)
        print('%s best:' % mode)
        for c, _, f, mae in res[:5]:
            print('  f%04d corr %.4f MAE %.1f' % (f, c, mae))


main()
