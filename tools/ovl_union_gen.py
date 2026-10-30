#!/usr/bin/env python3
"""Phase-4I: build the UNION (generation-aware) overlay-slot image.

The guest's overlay arena 0x02159FE0 holds several *generations* of the same
addresses: the ROM image the loader copies in, and the successive in-place
decryptions its own rolling-key routine (0x0215B468) performs over the resident
module -- melonDS calls it 10 times during the boot, each pass rewriting another
range (`docs/PHASE4I_UNION_DISPATCH.md`).  A dispatch image with one case per PC
can only serve one of them, which is why native never reached the decryptor
(Phase-4H) and why a wholesale capture of the arena broke the loader path
(Phase-4G).

This tool keys addresses by generation, using two measured inputs:

  * the melonDS overlay PC trace -- which arena words the guest EXECUTES;
  * a native RAM dump taken after the guest's own decryptor ran -- the
    post-decrypt bytes (byte-verified against melonDS at 0x0205D448).

Rule: an arena word the guest executes takes its post-decrypt bytes from RAM,
EXCEPT inside the loader/decrypt machinery windows (0x0215B400..0x0215B520 and
0x0215C100..0x0215C140): those addresses are executed *before* the guest's
decrypt rewrites anything, and overwriting them with the later data generation
is exactly what stalled the run at 0x0215B408 (measured: ROM `bl 0x0215B750`
-> RAM `pop {r4,pc}`, pc=2).

    python3 tools/ovl_union_gen.py <base.bin> <ram_dump.bin> <mel_ovl_pcs.txt> <out.bin>

<base.bin> is the previous union slot image (start from the ROM generation,
work/phase3a/ovl_p4f4.bin); re-running with a later RAM dump folds one more
decrypt generation in (the "loop method": each native halt gives the next
dump).
"""
import sys

BASE = 0x020C0000            # dispatch-image window base
ARENA_LO, ARENA_HI = 0x02159FE0, 0x0215C400
GBASE = 0x02000000           # native guest window base (nnk_rt.h GBASE)
LOADER_WINDOWS = [(0x0215B400, 0x0215B520), (0x0215C100, 0x0215C140)]


def main(argv):
    if len(argv) != 5:
        sys.exit(__doc__)
    base_path, ram_path, trace_path, out_path = argv[1:5]
    img = bytearray(open(base_path, 'rb').read())
    ram = open(ram_path, 'rb').read()

    pcs = []
    for line in open(trace_path):
        s = line.strip()
        if s.startswith('0x'):
            pcs.append(int(s, 16))
    executed = {pc & ~3 for pc in pcs if ARENA_LO <= pc < ARENA_HI}

    def in_loader(a):
        return any(lo <= a < hi for lo, hi in LOADER_WINDOWS)

    patched = same = kept = zero_data = 0
    for a in sorted(executed):
        if in_loader(a):
            kept += 1
            continue
        o = (a - GBASE) & 0x3FFFFF          # main-RAM mirror, as the guest sees it
        w = ram[o:o + 4]
        off = a - BASE
        cur = bytes(img[off:off + 4])
        if cur == w:
            same += 1
            continue
        if w == b'\0\0\0\0':
            zero_data += 1                  # runtime zero-fill (data), not code
            kept += 1
            continue
        img[off:off + 4] = w
        patched += 1

    open(out_path, 'wb').write(bytes(img))
    print('executed arena words : %d' % len(executed))
    print('patched from RAM     : %d' % patched)
    print('already equal        : %d' % same)
    print('kept base generation : %d (loader windows %d + zero-fill %d)'
          % (kept, kept - zero_data, zero_data))
    print('wrote %s' % out_path)


if __name__ == '__main__':
    main(sys.argv)
