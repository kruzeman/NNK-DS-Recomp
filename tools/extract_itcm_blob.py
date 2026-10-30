#!/usr/bin/env python3
"""Extract the ITCM code block (auto-load entry: dest=0x01FF8000 len=0x540)
from the decompressed ARM9 image to a raw blob for nnktool translation.

Source: work/ghidra_src/arm9.bin at 0x6C5C0 == guest address 0x0206C5C0 —
verified byte-identical to the melonDS ITCM dump (MELONDS_ITCM_DUMP) for the
full 0x540 bytes, i.e. exactly what the guest loader copies into ITCM.
"""
import sys

img_path = sys.argv[1] if len(sys.argv) > 1 else '/home/hermes/NiNoKuni/work/ghidra_src/arm9.bin'
out_path = sys.argv[2] if len(sys.argv) > 2 else '/home/hermes/.hermes/profiles/coder/cache/scratch/itcm_blob.bin'
SRC_OFF = 0x6C5C0
LEN = 0x540

d = open(img_path, 'rb').read()
assert len(d) >= SRC_OFF + LEN, 'image too short'
blob = d[SRC_OFF:SRC_OFF + LEN]
open(out_path, 'wb').write(blob)
print('wrote %s: %d bytes from %s+0x%X (guest 0x%08X)' % (
    out_path, LEN, img_path, SRC_OFF, 0x02000000 + SRC_OFF))
