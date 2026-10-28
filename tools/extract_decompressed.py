#!/usr/bin/env python3
"""
tools/extract_decompressed.py — write the RAM images of B2KJ into work/.

The ARM9 binary in the ROM is stored with Nintendo "code compression"
(reversed LZ77 + footer; first 0x4000 bytes are a literal prefix), so the
image that actually runs in RAM at 0x02000000 is bigger than the ROM slice
named in the header.  ARM7 is stored raw.  The 21 ARM9 overlays are stored
code-compressed too (all 21 decompress to exactly their OVT ramSize).

Outputs:
  work/arm9_dec.bin     decompressed ARM9 RAM image
  work/arm7_dec.bin     ARM7 RAM image (identical to the stored slice)
  work/ovt9_dec/ov9_*.bin   decompressed overlay RAM images
  work/ovt9_dec/ovt9_dec.tsv (id, file_id, ram, stored, decompressed, ramSize)

Usage: /usr/bin/python3 tools/extract_decompressed.py [rom]
Requires: /usr/bin/python3 with ndspy (pip --user).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.expanduser("~/.local/lib/python3.12/site-packages"))
from ndspy import codeCompression  # noqa: E402

ROM = sys.argv[1] if len(sys.argv) > 1 else "Ni No Kuni.nds"
OUT = "work"


def main():
    rom = open(ROM, "rb").read()
    a9_off, a9_entry, a9_ram, a9_size = struct.unpack_from("<IIII", rom, 0x20)
    a7_off, a7_entry, a7_ram, a7_size = struct.unpack_from("<IIII", rom, 0x30)
    ovt_off, ovt_size = struct.unpack_from("<II", rom, 0x50)
    fat_off, fat_size = struct.unpack_from("<II", rom, 0x48)

    a9 = rom[a9_off:a9_off + a9_size]
    a9d = codeCompression.decompress(a9)
    with open(os.path.join(OUT, "arm9_dec.bin"), "wb") as f:
        f.write(a9d)
    print("work/arm9_dec.bin  stored 0x%X -> 0x%X  entry 0x%08X (low16 window identical: %s)" % (
        len(a9), len(a9d), a9_entry, a9[0x800:0x900] == a9d[0x800:0x900]))

    a7 = rom[a7_off:a7_off + a7_size]
    a7d = codeCompression.decompress(a7)
    with open(os.path.join(OUT, "arm7_dec.bin"), "wb") as f:
        f.write(a7d)
    print("work/arm7_dec.bin  stored 0x%X -> 0x%X  (raw: %s)  entry 0x%08X" % (
        len(a7), len(a7d), len(a7) == len(a7d), a7_entry))

    ovt_dir = os.path.join(OUT, "ovt9_dec")
    os.makedirs(ovt_dir, exist_ok=True)
    lines = ["# id\tfile_id\tram\tram_size\tstored\tdecompressed\tmatch"]
    for i in range(ovt_size // 32):
        b = ovt_off + i * 32
        oid, ram, ram_size, bss, is_, ie, fid = struct.unpack_from("<7I", rom, b)
        s, e = struct.unpack_from("<II", rom, fat_off + fid * 8)
        data = rom[s:e]
        dec = codeCompression.decompress(data)
        ok = len(dec) == ram_size
        name = "ov9_%03d_f%05d.bin" % (oid, fid)
        with open(os.path.join(ovt_dir, name), "wb") as f:
            f.write(dec)
        lines.append("%d\t%d\t0x%08X\t0x%X\t0x%X\t0x%X\t%s" % (
            oid, fid, ram, ram_size, len(data), len(dec), "OK" if ok else "MISMATCH"))
        print("work/ovt9_dec/%s 0x%X -> 0x%X (ramSize 0x%X) %s" % (
            name, len(data), len(dec), ram_size, "OK" if ok else "MISMATCH"))
    with open(os.path.join(ovt_dir, "ovt9_dec.tsv"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote work/ovt9_dec/ovt9_dec.tsv (%d overlays)" % (ovt_size // 32))


if __name__ == "__main__":
    main()
