#!/usr/bin/env python3
"""Inspect an NNKS savestate: header identity + per-region table and a coarse
content summary (nonzero byte count, first 32 bytes as hex).  Diagnostic tool
for SAVESTATES.md; never written by the runtime."""
import struct
import sys
import zlib

HDR = struct.Struct('<4sIIII4xQIIQQQII')   # magic,ver,hsize,nreg,build,rom,rcrc,acrc,step,frame,psize,pcrc,hcrc
REG = struct.Struct('<16sQQII')

path = sys.argv[1]
b = open(path, 'rb').read()
(magic, ver, hsize, nreg, build, rom, rcrc, acrc, step, frame, psize, pcrc,
 hcrc) = HDR.unpack_from(b, 0)
print('file %s (%d B)' % (path, len(b)))
print('magic=%r ver=%d hsize=%d regions=%d build=0x%08X' % (magic, ver, hsize, nreg, build))
print('rom=%d hdr_crc=0x%08X arm9_crc=0x%08X step=%d frame=%d' % (rom, rcrc, acrc, step, frame))
print('payload=%d B crc=0x%08X (recomputed 0x%08X)' %
      (psize, pcrc, zlib.crc32(b[hsize:hsize + psize]) & 0xFFFFFFFF))
for i in range(nreg):
    name, off, ln, crc, rsv = REG.unpack_from(b, HDR.size + i * REG.size)
    data = b[hsize + off:hsize + off + ln]
    nz = sum(1 for x in data if x)
    print('  [%d] %-14s off=%-9d len=%-9d crc=0x%08X nz=%-9d %s' %
          (i, name.decode().rstrip('\0'), off, ln, crc, nz, data[:16].hex()))
