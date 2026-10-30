#!/usr/bin/env python3
"""Regenerate / verify the BIOS boot-state seed used by the ARM9 runtime.

The runtime starts at the ARM9 entry, so the state the DS BIOS wrote into main
RAM *before* entry has to be seeded (nnktool/src/recompile.rs,
`bios_boot_seed()`).  This tool derives that state from melonDS ground truth
and checks the model against the ROM, so the seed is reproducible rather than
hand-copied.

Inputs
  --memwatch  melonDS pre-entry main-RAM write log from
              tools/add_memwatch_window.py (lines:
              "W32 cpu=9 <addr> <value> pc7=.. pc9=.. a9idx=0").  Writes are
              replayed in order (last write wins, honouring W8/W16/W32 widths)
              to rebuild the byte image of the watched window.
  --rom       the .nds image (read-only; never committed).

Model checked
  0x027FFE00 is the BIOS ARM9 boot-parameter block: the first 0x200 bytes of
  the cartridge header, with header bytes 0x160..0x1BF zeroed.  The tool
  compares the replayed block with the ROM header and prints any mismatch.

Output
  * a summary line stating whether the header-copy model holds;
  * the C initialiser for the BIOS/firmware boot-param words
    (0x027FF800 / 0x027FFC00 areas) in the exact form used by
    `bios_bootparam[]` in recompile.rs.

usage:
  python3 tools/mk_bios_bootparams.py --memwatch <dump> --rom "<rom.nds>"
"""
import argparse
import sys

HDR_SRC = 0x027FFE00        # BIOS header-copy destination
HDR_LEN = 0x200
HDR_ZERO_LO, HDR_ZERO_HI = 0x160, 0x1BF   # measured-zeroed header range
PARAM_LO, PARAM_HI = 0x027FF000, HDR_SRC  # BIOS/firmware boot params


def replay(path, lo, hi):
    img = bytearray(hi - lo)
    seen = 0
    with open(path) as f:
        for ln in f:
            p = ln.split()
            if len(p) < 4:
                continue
            op, addr, val = p[0], int(p[2], 16), int(p[3], 16)
            width = 4 if op.endswith('32') else 2 if op.endswith('16') else 1
            if not (lo <= addr and addr + width <= hi):
                continue
            seen += 1
            for i in range(width):
                img[addr - lo + i] = (val >> (8 * i)) & 0xFF
    return img, seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--memwatch', required=True)
    ap.add_argument('--rom', required=True)
    a = ap.parse_args()

    lo = min(PARAM_LO, HDR_SRC)
    hi = HDR_SRC + HDR_LEN
    img, seen = replay(a.memwatch, lo, hi)
    print("replayed %d writes in 0x%08X..0x%08X" % (seen, lo, hi))

    hdr = open(a.rom, 'rb').read(HDR_LEN)
    model = bytearray(hdr)
    model[HDR_ZERO_LO:HDR_ZERO_HI + 1] = b"\x00" * (HDR_ZERO_HI + 1 - HDR_ZERO_LO)
    got = bytes(img[HDR_SRC - lo:HDR_SRC - lo + HDR_LEN])
    print("header copy: melonDS 0x%08X == ROM[0:0x%X] with 0x%X..0x%X zeroed -> %s"
          % (HDR_SRC, HDR_LEN, HDR_ZERO_LO, HDR_ZERO_HI, got == bytes(model)))
    if got != bytes(model):
        for i in range(0, HDR_LEN, 4):
            if got[i:i + 4] != model[i:i + 4]:
                print("  mismatch 0x%08X: melonDS %s model %s"
                      % (HDR_SRC + i, got[i:i + 4].hex(), bytes(model[i:i + 4]).hex()))
    print("  header word 0x80 (read by 0x02016FBC) = 0x%08X"
          % int.from_bytes(got[0x80:0x84], 'little'))

    print("\nBIOS/firmware boot params (non-zero words, C initialiser):")
    n = 0
    for addr in range(PARAM_LO, PARAM_HI, 4):
        w = int.from_bytes(img[addr - lo:addr - lo + 4], 'little')
        if w:
            print("    {0x%08Xu, 0x%08Xu}," % (addr, w))
            n += 1
    print("  /* %d words */" % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
