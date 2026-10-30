#!/usr/bin/env python3
"""Add a main-RAM write-watch window to melonDS's MemWatch (atomic, idempotent).

    python3 tools/add_memwatch_window.py <lo> <hi> [tag]

Adds `if (off >= lo && off <= hi) return true;` to MemWatch::Hit in
NDS.cpp.  `off` is the 4 MiB-masked main-RAM offset (NDS.cpp MemWatch::Hit),
so 0x02FFFE80 is off 0x003FFE80.  Writes from BOTH CPUs are logged with the
ARM9 PC-trace index:

  W<bits> cpu=<7|9> 0x<addr> 0x<val> pc7=0x<..> pc9=0x<..> a9idx=<n>

Env: MELONDS_MEMWATCH_PATH=<file> MELONDS_MEMWATCH_N=<count>.
"""
import sys

if len(sys.argv) < 3:
    sys.exit(__doc__)
lo = int(sys.argv[1], 0)
hi = int(sys.argv[2], 0)
tag = sys.argv[3] if len(sys.argv) > 3 else 'Phase-4C window'

PATH = '/home/hermes/tools/melonDS/src/NDS.cpp'
line = '    if (off >= 0x%08X && off <= 0x%08X) return true; /* %s */\n' % (lo, hi, tag)

s = open(PATH).read()
if line in s:
    print('already present')
    sys.exit(0)
anchor = '    u32 off = addr & 0x003FFFFF;\n'
assert s.count(anchor) == 1, 'Hit() anchor count %d' % s.count(anchor)
s = s.replace(anchor, anchor + line, 1)
open(PATH, 'w').write(s)
print('added window 0x%08X..0x%08X' % (lo, hi))
