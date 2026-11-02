#!/usr/bin/env python3
"""mel_walk.py <state.mln> — list the top-level sections of a melonDS
savestate (container documented in src/Savestate.cpp: header 16 bytes, then
sections of <magic:4><length:u32 incl. 16-byte header><8 reserved>)."""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
print('file %d bytes  magic=%r' % (len(d), d[:4]))
major, minor = struct.unpack_from('<HH', d, 4)
length = struct.unpack_from('<I', d, 8)[0]
print('version %d.%d  header length=%d' % (major, minor, length))
off = 0x10
while off + 16 <= len(d):
    magic = d[off:off + 4]
    seclen = struct.unpack_from('<I', d, off + 4)[0]
    print('  @0x%08x  %-6r len=0x%x (%d)' % (off, magic, seclen, seclen))
    if seclen < 16:
        print('   ... bad length, stop')
        break
    off += seclen
print('end at 0x%x (file 0x%x)' % (off, len(d)))
