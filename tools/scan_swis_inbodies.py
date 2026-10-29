#!/usr/bin/env python3
"""Cross-check SWI candidates against Ghidra function bodies.

For each ARM-state SWI candidate (bits 27..24 == 0xF) and each Thumb-state
candidate (0xDFxx) we report whether the address lies inside a Ghidra function
body, so code hits can be separated from data hits.
"""
import struct
import sys

img_path = sys.argv[1] if len(sys.argv) > 1 else "work/ghidra_src/arm9.bin"
tsv_path = sys.argv[2] if len(sys.argv) > 2 else "work/ghidra_arm9_functions.tsv"
base = 0x02000000
img = open(img_path, "rb").read()

spans = []
with open(tsv_path) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        a = int(p[0], 16)
        spans.append((a, a + max(int(p[2]), 4), p[1], int(p[5])))


def owner(addr):
    for s, e, name, ilen in spans:
        if s <= addr < e:
            return name, ilen, s
    return None


arm_hits = []
for off in range(0, len(img) - 3, 4):
    w = struct.unpack("<I", img[off:off + 4])[0]
    if (w & 0x0F000000) == 0x0F000000:
        imm = w & 0xFFFFFF
        if imm <= 0x1F or (imm & 0xFFFF) == 0:
            arm_hits.append((base + off, w, imm))

th_hits = []
for off in range(0, len(img) - 1, 2):
    h = struct.unpack("<H", img[off:off + 2])[0]
    if (h & 0xFF00) == 0xDF00:
        th_hits.append((base + off, h & 0xFF))

print("== ARM-state SWI candidates with a BIOS-plausible imm24 (<=0x1F or N<<16) ==")
for a, w, imm in arm_hits:
    o = owner(a)
    print("  0x%08X %08X imm=0x%06X  %s" %
          (a, w, imm, "in %s (entry-insn-len %d, body 0x%08X)" % o if o else "NOT in a Ghidra body"))
print("count: %d (inside bodies: %d)" %
      (len(arm_hits), sum(1 for a, _, _ in arm_hits if owner(a))))

print()
print("== Thumb-state svc candidates inside a Ghidra function body ==")
inside = [(a, n) for a, n in th_hits if owner(a)]
for a, n in inside:
    o = owner(a)
    if o:
        print("  0x%08X svc #%d (0x%02X)  in %s (entry 0x%08X)" % (a, n, n, o[0], o[2]))
print("count: %d inside bodies, %d total raw" % (len(inside), len(th_hits)))
