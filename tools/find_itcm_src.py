#!/usr/bin/env python3
"""Locate the ITCM routine's source bytes in the ARM9 images (copy base)."""
import struct
import sys

sigs = {
    # ITCM 0x01FF845C: push {r3-r9,lr}; ldr r7,[sp,#32]; mov r4,r0; mov r9,r1
    'fn_845c': bytes.fromhex('f8432de520709de50040a0e1'),
    # ITCM 0x01FF8000 vector: push {lr}; mov ip,#0x04000000
    'vec_8000': bytes.fromhex('0040bde901c0a0e3'),
}
for img in (sys.argv[1:] or ['/home/hermes/NiNoKuni/work/ghidra_src/arm9.bin',
            '/home/hermes/NiNoKuni/work/arm9.bin',
            '/home/hermes/NiNoKuni/work/phase3a/arm9_stored_padded.bin']):
    d = open(img, 'rb').read()
    for name, sig in sigs.items():
        hits = []
        start = 0
        while True:
            i = d.find(sig, start)
            if i < 0:
                break
            hits.append(i)
            start = i + 1
        print(f"{img.split('/')[-1]} {name}: " +
              (', '.join(f"off=0x{i:X} addr=0x{0x02000000+i:08X}" for i in hits) or 'NOT FOUND'))
