#!/usr/bin/env python3
"""Phase-5J: splice the interworking-state note into an EXISTING dispatch image.

WHY A TEXT PATCH AND NOT A REGENERATION
---------------------------------------
The fix for the 0x020F5DF4 null return lives in the *emitter*
(nnktool/src/recompile.rs, `LdmStm` + pc: `nnk_smc_note_branch(_t);`), so any
future `nnktool recompile-all` run emits it.  The image this branch runs
(`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`) is a checked-out build artifact from
Phase-4F, and the inputs needed to regenerate it byte-for-byte are gone from the
scratch cache (the ITCM sub-image and its melonDS seed trace); a fresh
regeneration would also fold in every runtime change made since Phase-4F (the
emitter's RUNTIME strings are part of the image prelude).  Regenerating would
therefore change the measured baseline instead of one decode class.

So: patch the artifact with EXACTLY the text the new emitter emits, and keep the
emitter as the source of truth for the next full regeneration.

THE PATTERN
-----------
The emitter prints, for every LDM/`pop` with pc in the register list:

    uint32_t _t = ld32(_a + 20u);      <- index is the transfer slot
    pc = _t & ~1u;

`bit0` of that loaded word is the target's instruction state (melonDS A_POP
forces it for Thumb `pop {pc}`, A_LDM takes it from the word).  The patch
appends

    nnk_smc_note_branch(_t);

after the `pc = _t & ~1u;` line — and only where the preceding line is that
`_a + N` form, so the LDR-into-pc sites (which melonDS does NOT interwork on)
and the bx/blx sites are untouched.  A block whose register list is pc ALONE
takes the single-register form (`note(_t | 1u)`): melonDS T_POP does
`if (Num==1) pc |= 0x1` there, while a multi-register pop takes the word's bit0.
Idempotent: a file that already carries the note is reported and left alone.

usage: tools/p5j_note_branch.py <image.c> [--check]
"""
import re
import sys

LOAD = re.compile(r'^(\s*)uint32_t _t = ld32\(_a \+ \d+u\);\s*$')
ASSIGN = re.compile(r'^(\s*)pc = _t & ~1u;\s*$')
REGROW = re.compile(r'^\s*r\[\d+\] = ld32\(_a \+ \d+u\);\s*$')
BLOCK = re.compile(r'^(\s*)\{ uint32_t _base = ')
NOTE = 'nnk_smc_note_branch(_t);'
NOTE_SINGLE = 'nnk_smc_note_branch(_t | 1u);'
# Phase-5K: the single-register ARM form takes `& ~1u` (melonDS A_LDM), and
# tools/p5k_note_branch.py may already have written it.  Without this the
# patcher would insert a SECOND, contradicting note (and re-introduce the
# Thumb tag that made sp=0 at 0x0207EA20).
NOTE_ARM_SINGLE = 'nnk_smc_note_branch(_t & ~1u);'


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    path = argv[1]
    check = '--check' in argv[2:]
    lines = open(path).read().split('\n')
    out, added, already, single = [], 0, 0, 0
    prev = None
    regs_in_block = 0
    for i, ln in enumerate(lines):
        out.append(ln)
        if BLOCK.match(ln):
            regs_in_block = 0
        elif REGROW.match(ln):
            regs_in_block += 1
        m = ASSIGN.match(ln)
        if m and prev is not None and LOAD.match(prev):
            # a LDM/`pop` with pc: one register left in the list == pc alone
            note = NOTE_SINGLE if regs_in_block == 0 else NOTE
            nxt = lines[i + 1] if i + 1 < len(lines) else ''
            if nxt.strip() in (NOTE, NOTE_SINGLE, NOTE_ARM_SINGLE):
                already += 1
            else:
                out.append(m.group(1) + note)
                added += 1
                single += 1 if note is NOTE_SINGLE else 0
        prev = ln
    if check:
        print('%s: %d LDM-pc site(s) would gain the note (%d already carry it)'
              % (path, added, already))
        return 0 if added == 0 else 1
    if added:
        open(path, 'w').write('\n'.join(out))
    print('%s: %d LDM-pc site(s) patched (%d single-register), %d already had the note'
          % (path, added, single, already))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
