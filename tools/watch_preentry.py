#!/usr/bin/env python3
"""Log EVERY pre-entry main-RAM write (Phase-4B): while the ARM9 has not yet
executed its first in-window instruction, whatever lands in main RAM comes
from the ARM9 BIOS or the ARM7's own boot — side effects the native runtime
(skipping both) must reproduce as measured initial state.

Idempotent.
"""
import sys

path = '/home/hermes/tools/melonDS/src/NDS.cpp'
s = open(path).read()

ANCHOR = '''    if (off >= 0x0006F400 && off <= 0x0006F600) return true; /* Phase-4B: struct @0x6F480 (+0xBE field) */
'''
ADD = '''    if (off >= 0x0006F400 && off <= 0x0006F600) return true; /* Phase-4B: struct @0x6F480 (+0xBE field) */
    /* Phase-4B: every main-RAM write BEFORE the ARM9 entry point (a9idx==0):
     * BIOS/ARM7 pre-entry initialization the native run must model. */
    if ((addr & 0xFF000000u) == 0x02000000u && ARM9TracePCCount() == 0) return true;
'''
if ADD in s:
    print('already present')
    sys.exit(0)
assert s.count(ANCHOR) == 1, 'anchor %d' % s.count(ANCHOR)
s = s.replace(ANCHOR, ADD, 1)
open(path, 'w').write(s)
print('pre-entry main-RAM watch added')
