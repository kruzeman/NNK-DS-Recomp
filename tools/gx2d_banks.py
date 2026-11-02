#!/usr/bin/env python3
"""bankdump.py <state.bin> [frame] -> census of the nine RAW VRAM banks (the
0x120000 tail a NNK_GX_STATE_DUMP writes after the 0x195400 state image) and a
comparison against the derived flat views (ABG = banks A,B; BBG = C,D;
AOBJ = E,F; BOBJ = G,H; extended = I).

Why: a VRAMCNT remap must PRESERVE bank content (melonDS: banks are physical
and only the mapping changes).  If a bank holds data but the derived view for
the newly mapped window is empty (or vice versa), the remap path is the bug.
"""
import sys

STATE = 0x195400
BANK = 0x20000
VIEWS = {'ABG': (0x1400, 0x80000, (0, 1)), 'BBG': (0x81400, 0x20000, (2, 3)),
         'AOBJ': (0xA1400, 0x40000, (4, 5)), 'BOBJ': (0xE1400, 0x20000, (6, 7)),
         'ABGEXT': (0x101400, 0x8000, (8,)), 'BBGEXT': (0x109400, 0x8000, ()),
         'AOBJEXT': (0x111400, 0x2000, ()), 'BOBJEXT': (0x113400, 0x2000, ())}


def main(path):
    d = open(path, 'rb').read()
    print('state image %d bytes (expected %d)' % (len(d), STATE + 9 * BANK))
    banks = []
    for b in range(9):
        off = STATE + b * BANK
        buf = d[off:off + BANK]
        nz = sum(1 for x in buf if x)
        blocks = [sum(1 for x in buf[i * 0x2000:(i + 1) * 0x2000] if x) for i in range(16)]
        banks.append(buf)
        print('bank %s nonzero=%-7d blocks(8K): %s' % ('ABCDEFGHI'[b], nz,
              ' '.join(str(v) for v in blocks)))
    print()
    for name, (off, sz, src) in VIEWS.items():
        view = d[off:off + sz]
        nz = sum(1 for x in view if x)
        concat = b''.join(banks[i] for i in src) if src else b''
        same = concat[:sz] == view if src else None
        print('%-8s view_nonzero=%-7d  from banks %s  view==banks: %s' %
              (name, nz, ''.join('ABCDEFGHI'[i] for i in src),
               'n/a' if same is None else ('YES' if same else 'no')))
        if same is False:
            # where does it first differ?
            for i in range(0, sz):
                if concat[i] != view[i]:
                    print('    first diff at +0x%05x' % i)
                    break
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
