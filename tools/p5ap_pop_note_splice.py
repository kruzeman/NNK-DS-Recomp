#!/usr/bin/env python3
"""Phase-5AP splice: the ARM `pop {pc}` interworking note must keep bit0.

    python3 tools/p5ap_pop_note_splice.py <in.c> <out.c>

WHY
---
`nnktool recompile-all` masks the loaded word before handing it to the runtime
state tracker when the LDM list has a SINGLE register:

    uint32_t _t = ld32(_a + 0u);
    pc = _t & ~1u;
    nnk_smc_note_branch(_t & ~1u);      <- the state is LOST here

For a non-SP base (`ldmia r0,{pc}`) and for the Thumb POP forms that is the
measured Phase-5J behaviour, but `ldmfd sp!,{pc}` is A_POP: the loaded word IS
the hardware's interworking target, bit0 included, and a Thumb `blx` leaves
LR = (return | 1).  Measured (Phase-5AP): ov17's Thumb caller 0x020E3390
`blx 0x02043BD0` returned to 0x020E3394; the masking note forced ARM, the ARM
reading of the Thumb bytes (`0x1C221C21`) executed as `stmiapl sl!,{...}`,
wrecked sp (0x027E39D0 -> 0xFFFFFFF0) and the stub's `ldmfd sp!,{pc}` popped 0
(halt step 560,192,510, "dispatch to untranslated pc 0x00000000").

WHAT
----
Rewrite `nnk_smc_note_branch(_t & ~1u);` to `nnk_smc_note_branch(_t);` ONLY in
case bodies whose LDM base is `r[13]` (SP).  The canonical fix lives in
nnktool/src/recompile.rs; this transform applies it to a frozen image that
predates it, exactly like the Phase-5X/5AE/5AF/5AJ prelude splices.
"""
import os
import re
import sys

BASE = re.compile(r"\{ uint32_t _base = r\[(\d+)\];")
NOTE = "nnk_smc_note_branch(_t & ~1u);"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.readlines()
    n_fix = 0
    n_keep = 0
    base = None
    for i, line in enumerate(lines):
        m = BASE.search(line)
        if m:
            base = int(m.group(1))
        s = line.strip()
        if s == NOTE:
            if base == 13:
                indent = line[: len(line) - len(line.lstrip())]
                lines[i] = indent + "nnk_smc_note_branch(_t);\n"
                n_fix += 1
            else:
                n_keep += 1
            base = None
        elif s.startswith("case 0x"):
            base = None
    with open(dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write("/* Phase-5AP pop-note splice applied: ARM `ldmfd sp!,{pc}` keeps "
                 "bit0 in nnk_smc_note_branch */\n")
        fh.writelines(lines)
    print("p5ap pop-note splice: %d SP-base notes now keep bit0, %d kept masked -> %s"
          % (n_fix, n_keep, dst))


if __name__ == "__main__":
    main()
