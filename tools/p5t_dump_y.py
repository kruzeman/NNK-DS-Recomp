#!/usr/bin/env python3
"""Dump one frame's Y plane (or the full YUV as RGB) from a raw yuv420p file."""
import sys, zlib, struct

W, H = 256, 192
FRAME = 73728


def chunk(typ, data):
    return (struct.pack('>I', len(data)) + typ + data +
            struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF))


def png(path, w, h, raw, gray):
    hdr = struct.pack('>IIBBBBB', w, h, 8, 0 if gray else 2, 0, 0, 0)
    body = b''.join(b'\x00' + raw[y * w * (1 if gray else 3):(y + 1) * w * (1 if gray else 3)]
                    for y in range(h))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr) +
                           chunk(b'IDAT', zlib.compress(body, 6)) + chunk(b'IEND', b''))


def main():
    yuv, fr, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    d = open(yuv, 'rb').read()
    buf = d[fr * FRAME:(fr + 1) * FRAME]
    y = buf[:W * H]
    # 2x nearest upscale like the review renders
    up = bytes(y[((i // (2 * W)) // 2) * W + (i % (2 * W)) // 2]
               for i in range(2 * W * 2 * H))
    png(out, 2 * W, 2 * H, up, True)
    print('%s frame %d -> %s' % (yuv, fr, out))


main()
