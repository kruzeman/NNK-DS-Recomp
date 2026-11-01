#!/usr/bin/env python3
"""Phase-5T: extract a file from the Ni no Kuni NDS ROM using the file-id map
already present in work/fnt_full.txt (lines are "<id>\t<path>").

usage: p5t_extract.py <rom> <fnt_full.txt> <path> <out>
FAT: header 0x48 = FAT offset, 0x4C = FAT size (8 bytes per file: start, end).
"""
import sys, struct

def main():
    rom_path, map_path, wanted, out = sys.argv[1:5]
    wanted = wanted.lstrip('/')
    ids = {}
    for line in open(map_path, errors='replace'):
        line = line.rstrip('\n')
        if '\t' not in line:
            continue
        a, b = line.split('\t', 1)
        try:
            ids[b] = int(a)
        except ValueError:
            pass
    if wanted not in ids:
        cands = sorted(k for k in ids if k.endswith(wanted))
        print('no exact match; candidates:', cands[:10])
        sys.exit(2)
    fid = ids[wanted]
    rom = open(rom_path, 'rb').read()
    fat_off, fat_size = struct.unpack_from('<II', rom, 0x48)
    assert fat_off + (fid + 1) * 8 <= fat_off + fat_size
    start, end = struct.unpack_from('<II', rom, fat_off + fid * 8)
    data = rom[start:end]
    open(out, 'wb').write(data)
    print('%s id=%d off=0x%X len=0x%X (%d bytes) -> %s' %
          (wanted, fid, start, len(data), len(data), out))
    print('head:', data[:64].hex(' '))

main()
