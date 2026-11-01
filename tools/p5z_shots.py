#!/usr/bin/env python3
"""Phase-5Z: PPM shots -> per-screen PNGs (stdlib only; zlib for the PNG idat).

Groups the NNk_GX_SHOT_EVERY dumps by content hash, keeps the FIRST frame of
each group (= a screen transition) and writes it as a PNG into the shots dir.

usage: shots.py <shot-dir> <out-dir> [prefix]
"""
import hashlib
import os
import re
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("not a binary PPM: %s" % path)
    # P6 <ws> width <ws> height <ws> maxval <one whitespace> pixels
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    w, h, _maxv = fields
    i += 1  # single whitespace after maxval
    pix = data[i:i + w * h * 3]
    if len(pix) != w * h * 3:
        raise SystemExit("short PPM payload: %s" % path)
    return w, h, pix


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)                       # filter type 0
        raw += rgb[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)
    return len(png)


def main():
    sd, od = sys.argv[1], sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else "p5z"
    os.makedirs(od, exist_ok=True)
    shot_re = re.compile(r"^(.*)\.(\d+)\.(top|bot)\.ppm$")
    files = {}
    for name in sorted(os.listdir(sd)):
        m = shot_re.match(name)
        if m:
            files.setdefault(int(m.group(2)), {})[m.group(3)] = os.path.join(sd, name)
    if not files:
        raise SystemExit("no shots in %s" % sd)
    frames = sorted(files)
    # content hash per (frame, screen) -> group consecutive identical frames
    prev = {}
    transitions = []
    for fr in frames:
        cur = {}
        for scr in ("top", "bot"):
            p = files[fr].get(scr)
            cur[scr] = hashlib.md5(open(p, "rb").read()).hexdigest() if p else None
        changed = [s for s in ("top", "bot") if cur[s] != prev.get(s)]
        if changed:
            transitions.append((fr, changed))
            prev = cur
    print("shots: %d frames sampled (%d..%d), %d transitions"
          % (len(frames), frames[0], frames[-1], len(transitions)))
    written = 0
    for fr, changed in transitions:
        for scr in changed:
            p = files[fr].get(scr)
            if not p:
                continue
            w, h, pix = read_ppm(p)
            out = os.path.join(od, "%s_f%05d_%s.png" % (prefix, fr, scr))
            n = write_png(out, w, h, pix)
            written += 1
            print("  f%05d %-3s %dx%d -> %s (%d bytes)" % (fr, scr, w, h, out, n))
    print("wrote %d PNG(s) to %s" % (written, od))


if __name__ == "__main__":
    main()
