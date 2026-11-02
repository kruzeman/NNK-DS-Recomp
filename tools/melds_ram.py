"""Extract ARM9 main RAM from a melonDS v13 savestate (NDSG payload: 4-byte prefix,
then 4 MB of main RAM at 0x02000000) and answer a few questions about it.

usage:
  melds_ram.py <state.mln> ram <out.bin>
  melds_ram.py <state.mln> find <hex-u32>[,...]
  melds_ram.py <state.mln> dump <addr>:<len>
  melds_ram.py <state.mln> structs
"""
import struct
import sys

BASE = 0x02000000
SIZE = 0x400000


def main_ram(path):
    d = open(path, 'rb').read()
    off = 0x10
    while off + 16 <= len(d):
        magic = d[off:off + 4]
        ln = struct.unpack_from('<I', d, off + 4)[0]
        if magic == b'NDSG':
            pay = d[off + 16:off + ln]
            # payload[0..4) is a u32 field, main RAM follows
            return pay[4:4 + SIZE]
        if ln < 16:
            break
        off += ln
    raise SystemExit('NDSG not found')


def main():
    path = sys.argv[1]
    cmd = sys.argv[2]
    ram = main_ram(path)
    if cmd == 'ram':
        open(sys.argv[3], 'wb').write(ram)
        print('wrote %s (%d bytes)' % (sys.argv[3], len(ram)))
        return
    if cmd == 'find':
        for h in sys.argv[3].split(','):
            pat = struct.pack('<I', int(h, 16))
            hits = []
            i = ram.find(pat)
            while i >= 0 and len(hits) < 40:
                hits.append(BASE + (i & ~3))
                i = ram.find(pat, i + 1)
            print('0x%08X: %d hit(s) %s' % (int(h, 16), len(hits),
                                            [hex(x) for x in hits]))
        return
    if cmd == 'dump':
        a, ln = sys.argv[3].split(':')
        a = int(a, 0)
        ln = int(ln, 0)
        b = ram[a - BASE:a - BASE + ln]
        for i in range(0, len(b), 16):
            w = struct.unpack_from('<4I', b, i) if i + 16 <= len(b) else ()
            print('%08x  %-47s  %-23s  %s' % (
                a + i, b[i:i + 16].hex(' '),
                ' '.join('%08x' % x for x in w),
                ''.join(chr(c) if 32 <= c < 127 else '.' for c in b[i:i + 16])))
        return
    raise SystemExit('unknown command %r' % cmd)


main()
