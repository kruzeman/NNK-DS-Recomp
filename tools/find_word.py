#!/usr/bin/env python3
"""Find 32-bit word occurrences in the ARM9 image and overlay 17."""
import struct
import sys

TARGETS = [
    (0xE52DE004, "str lr,[sp,#-4]!"),
    (0xE59D2018, "ldr r2,[sp,#0x18]"),
    (0xE3A02001, "mov r2,#1"),
    (0xE3A02000, "mov r2,#0"),
    (0xE92D4038, "push {r3,r4,r5,lr}"),
    (0xE92D4008, "push {r3,lr}"),
]

IMAGES = [
    ("arm9", "/home/hermes/NiNoKuni/work/arm9_dec.bin", 0x02000000),
    ("ov17", "/home/hermes/NiNoKuni/work/ovl_targets/ovl_17_020D0A00.bin", 0x020D0A00),
]

for name, path, base in IMAGES:
    d = open(path, "rb").read()
    for word, label in TARGETS:
        hits = []
        i = 0
        pat = struct.pack("<I", word)
        while True:
            i = d.find(pat, i)
            if i < 0:
                break
            hits.append(base + i)
            i += 1
        print("%-5s %-22s %3d %s" % (name, label, len(hits),
                                     " ".join("0x%08X" % h for h in hits[:16])))
