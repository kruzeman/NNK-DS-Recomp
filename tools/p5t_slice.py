#!/usr/bin/env python3
"""Slice a guest-memory image into a scratch file for objdump.

usage: p5t_slice.py <image> <image_vma_base> <addr> <len> <out>
  image_vma_base = guest address the first byte of <image> maps to
                   (e.g. 0x02000000 for arm9.bin, 0x01FF8000 for an ITCM blob)
"""
import sys

def main():
    img, base, addr, ln, out = sys.argv[1:6]
    base = int(base, 0); addr = int(addr, 0); ln = int(ln, 0)
    d = open(img, 'rb').read()
    off = addr - base
    assert 0 <= off <= len(d), (hex(off), len(d))
    chunk = d[off:off + ln]
    open(out, 'wb').write(chunk)
    print('%s[0x%X..0x%X] (%d bytes) -> %s' % (img, addr, addr + len(chunk), len(chunk), out))

main()
