#!/usr/bin/env python3
"""Search raw ROM bytes for byte patterns (default: the ARM words at the halt)."""
import sys

PATS = {
    "str lr,[sp,#-4]! (E52DE004)": bytes([0x04, 0xE0, 0x2D, 0xE5]),
    "ldr r2,[sp,#0x18] (E59D2018)": bytes([0x18, 0x20, 0x9D, 0xE5]),
    "mov r2,#1 (E3A02001)": bytes([0x01, 0x20, 0xA0, 0xE3]),
    "mov r2,#0 (E3A02000)": bytes([0x00, 0x20, 0xA0, 0xE3]),
    "blx r3 (E12FFF33)": bytes([0x33, 0xFF, 0x2F, 0xE1]),
}
paths = sys.argv[1:]
for p in paths:
    d = open(p, "rb").read()
    print("== %s (%d bytes)" % (p, len(d)))
    for label, pat in PATS.items():
        hits = []
        i = 0
        while True:
            i = d.find(pat, i)
            if i < 0:
                break
            hits.append(i)
            i += 1
        print("  %-32s %4d  %s" % (label, len(hits),
                                   " ".join("0x%06X" % h for h in hits[:12])))
