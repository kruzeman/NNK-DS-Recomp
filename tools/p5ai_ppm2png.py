#!/usr/bin/env python3
"""Minimal PPM (P6) -> PNG converter, stdlib only (Phase-5AI evidence shots).

usage: p5ai_ppm2png.py <in.ppm> <out.png>
"""
import struct
import sys
import zlib


def read_ppm(path):
    d = open(path, 'rb').read()
    assert d.startswith(b'P6'), path
    i, fields = 2, []
    while len(fields) < 3:
        while d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b'#':
            while d[i:i + 1] != b'\n':
                i += 1
            continue
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        fields.append(int(d[i:j]))
        i = j
    i += 1
    w, h, _mx = fields
    return w, h, d[i:i + w * h * 3]


def png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 9))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)


w, h, rgb = read_ppm(sys.argv[1])
png(sys.argv[2], w, h, rgb)
print("%s -> %s (%dx%d)" % (sys.argv[1], sys.argv[2], w, h))
