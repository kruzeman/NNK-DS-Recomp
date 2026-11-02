#!/usr/bin/env python3
"""Phase-5AW splice: an ARM exception return must carry the SPSR's T bit.

    python3 tools/p5aw_exc_return_splice.py <in.c> <out.c>

WHY
---
The ARM exception return (`subs`/`movs pc, lr`, rd=15 with S=1 in a privileged
mode) restores CPSR from the mode's SPSR.  The compiled case already restores
the mode byte and the NZCV flags, but it drops the T bit -- and the dispatcher
resolves a PC's instruction state per dispatch (carried branch state, else the
ROM / compiled map, else a register/stride/sticky guess).  The target of
`subs pc, lr, #4` is a dynamic `lr-4`, so NONE of those can classify it.

Measured (Phase-5AW): 0x0200729C `subs pc, lr, #4` resumed the ITCM font
renderer at 0x01FFC684 under SPSR 0x2000001F (T=0 -> ARM).  The previous live
decode had been a Thumb stream, so the stale sticky guess ran the ARM blit code
as Thumb:

    0x01FFC684  ARM: e089314a  add  r3, r9, sl, asr #2
                Thumb: 314a     adds r1, #0x4A        <- wrong
                        e089     b.n  0x01FFC79C
                0x01FFC79C  7087     strb r7, [r0, #2]
                0x01FFC79E  e040     b.n  0x01FFC822
                0x01FFC822  e263     ...  -> 0x01FFC4EC (pop {pc})

The wrong stream reached the ARM epilogue at 0x01FFC4EC without its
`push {lr}` prologue, so `pop {pc}` at 0x01FFC548 popped the codec/data pointer
0x022C9F38 from the stack.  The run then executed the movie codec's data table
as ARM until it halted with `dispatch to untranslated pc` at 0x022CA20C (the
data word 0xFFFFFFD3, step 1,112,592,480).

WHAT
----
Rewrite every emitted exception-return block

    if (mode_priv()) { uint32_t _spsr = spsr_bank[mode_cur]; ... v = (_spsr >> 28) & 1; }

to also call the runtime carrier

    ... v = (_spsr >> 28) & 1; nnk_smc_note_exc_return(pc, _spsr); }

`pc` is already the computed target (the emitter assigns it before this block).
The canonical change lives in nnktool/src/recompile.rs and in the live decoder
(tools/nnk_smc.inc); this transform applies the identical effect to a frozen
image that predates it, exactly like the Phase-5X/5AE/5AF/5AJ/5AP/5AT splices.

The runtime side (nnk_smc.inc) is compiled from the tree, so only the case
bodies need splicing.
"""
import sys

MARKER = "/* Phase-5AW exception-return state splice applied */\n"
ANCHOR = "if (mode_priv()) { uint32_t _spsr = spsr_bank[mode_cur];"
TAIL = "v = (_spsr >> 28) & 1; }"
NEW_TAIL = "v = (_spsr >> 28) & 1; nnk_smc_note_exc_return(pc, _spsr); }"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.readlines()
    if any(MARKER.strip() in line for line in lines[:4]):
        sys.exit("p5aw: %s already carries the marker (refuse to double-splice)" % src)
    n_fix = 0
    for i, line in enumerate(lines):
        s = line.strip()
        if not s.startswith(ANCHOR):
            continue
        if "nnk_smc_note_exc_return" in s:
            continue
        if not s.endswith(TAIL):
            sys.exit("p5aw: %s:%d: exception-return block does not end in %r\n"
                     "      line: %s" % (src, i + 1, TAIL, s))
        lines[i] = line[: len(line) - len(TAIL) - 1] + NEW_TAIL + "\n"
        n_fix += 1
    if n_fix == 0:
        sys.exit("p5aw: no exception-return block found in %s" % src)
    with open(dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(MARKER)
        fh.writelines(lines)
    sys.stderr.write("p5aw: %d exception-return block(s) now carry the SPSR T bit\n"
                     % n_fix)


if __name__ == "__main__":
    main()
