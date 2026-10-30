#!/usr/bin/env python3
"""Widen the melonDS MemWatch main-RAM window for Phase-4B (atomic).

Phase-4B root-cause probe: the native run diverges at 0x0201C1B4 because
ldrh at 0x02FFFC30 (SDK shared halfword, expected 0xFFFF) reads 0 in the
runtime while melonDS already sees 0xFFFF.  The existing watch only covers
0x3FFF80..0x3FFFB0, so this adds the 0x3FFC00..0x3FFD00 band around it —
enough to capture WHO writes the word (pc7/pc9/a9idx are logged).
"""
import sys

path = '/home/hermes/tools/melonDS/src/NDS.cpp'
s = open(path).read()

OLD = '''    if (off >= 0x003FFF80 && off <= 0x003FFFB0) return true; /* flag words */
    if (off >= 0x003FFC00 && off <= 0x003FFD00) return true; /* Phase-4B: SDK halfword @0x3FFC30 */
'''
NEW = '''    if (off >= 0x003FFF80 && off <= 0x003FFFB0) return true; /* flag words */
    if (off >= 0x003FFC00 && off <= 0x003FFD00) return true; /* Phase-4B: SDK halfword @0x3FFC30 */
    if (off >= 0x0006F400 && off <= 0x0006F600) return true; /* Phase-4B: struct @0x6F480 (+0xBE field) */
'''
if NEW in s:
    print('already widened')
    sys.exit(0)
assert s.count(OLD) == 1, 'anchor count %d' % s.count(OLD)
s = s.replace(OLD, NEW, 1)
open(path, 'w').write(s)
print('MemWatch window widened with 0x3FFC00..0x3FFD00 band')
