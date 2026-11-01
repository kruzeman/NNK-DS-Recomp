#!/usr/bin/env python3
"""Phase-5AL: PPM (P6, 8-bit) -> PNG, stdlib only.

Converts one or more PPMs to PNGs; used to turn the melonDS reference
screenshots (MELONDS_SHOT_DIR) into files a human can open.

usage: ppm2png.py <out.png> <in.ppm> [<in2.ppm> <out2.png> ...]
       ppm2png.py <file.ppm> ...        (writes <file>.png next to each)
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, "rb").read()
    if not data.startswith(b"P6"):
        raise SystemExit("not a binary PPM: %s" % path)
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b'#':
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, _maxv = fields
    i += 1
    pix = data[i:i + w * h * 3]
    if len(pix) != w * h * 3:
        raise SystemExit("short PPM payload: %s" % path)
    return w, h, pix


def write_png(path, w, h, pix):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pix[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    out = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    open(path, "wb").write(out)


def convert(src, dst):
    w, h, pix = read_ppm(src)
    write_png(dst, w, h, pix)
    print("%s (%dx%d) -> %s" % (src, w, h, dst))


args = sys.argv[1:]
if not args:
    raise SystemExit(__doc__)
if len(args) == 2 and args[0].endswith(".png"):
    convert(args[1], args[0])
else:
    for a in args:
        convert(a, a + ".png")
