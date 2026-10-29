#!/usr/bin/env python3
"""Raw image-wide SWI (svc) scan for the ARM9 image.

Every word whose bits 27..24 are 0b1111 is a candidate SWI (any condition).
Prints the distribution of both SWI-number conventions:
  * high-byte convention  imm24 = N << 16   (what FreeBIOS/GBATEK read at [lr-2])
  * low-byte  convention  imm24 = N
plus a raw listing of all candidates (address, word, imm24), so the caller can
tell code from data by looking at the surroundings.
"""
import struct
import sys

img_path = sys.argv[1] if len(sys.argv) > 1 else "work/ghidra_src/arm9.bin"
base = 0x02000000
img = open(img_path, "rb").read()

hi = []   # imm24 == N << 16
lo = []   # imm24 == N (0 < N <= 0xFF)
other = []
for off in range(0, len(img) - 3, 4):
    w = struct.unpack("<I", img[off:off + 4])[0]
    if (w & 0x0F000000) != 0x0F000000:
        continue
    imm = w & 0x00FFFFFF
    a = base + off
    if imm & 0xFFFF == 0 and (imm >> 16) <= 0xFF:
        hi.append((a, w, imm >> 16))
    elif imm <= 0xFF:
        lo.append((a, w, imm))
    else:
        other.append((a, w, imm))

def show(name, lst, cap=40):
    print("%s: %d" % (name, len(lst)))
    for a, w, n in lst[:cap]:
        print("  0x%08X  %08X  swi-number=0x%02X" % (a, w, n))
    if len(lst) > cap:
        print("  ... %d more" % (len(lst) - cap))

show("high-byte convention (imm24 = N<<16, FreeBIOS reads [lr-2])", hi)
show("low-byte convention (imm24 = N)", lo)
show("other imm24", other, 20)
print()
print("distinct high-byte SWI numbers: %s" %
      ",".join("0x%02X" % n for _, _, n in sorted(set((a, w, n) for a, w, n in hi), key=lambda t: t[2])))
print("distinct low-byte SWI numbers : %s" %
      ",".join("0x%02X" % n for _, _, n in sorted(set((a, w, n) for a, w, n in lo), key=lambda t: t[2])))

# ---- Thumb: svc #imm8 is 0xDFxx ------------------------------------------
th = []
for off in range(0, len(img) - 1, 2):
    h = struct.unpack("<H", img[off:off + 2])[0]
    if (h & 0xFF00) == 0xDF00:
        th.append((base + off, h & 0xFF))
print()
print("Thumb svc candidates (any halfword): %d" % len(th))
for a, n in th[:40]:
    print("  0x%08X  svc #%d (0x%02X)" % (a, n, n))
if len(th) > 40:
    print("  ... %d more" % (len(th) - 40))
print("distinct Thumb svc immediates: %s" %
      ",".join("0x%02X" % n for n in sorted(set(n for _, n in th))))
print("word-aligned Thumb svc: %d of %d" % (sum(1 for a, _ in th if (a - base) % 4 == 0), len(th)))
