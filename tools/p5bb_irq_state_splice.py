#!/usr/bin/env python3
"""Phase-5BB splice: an exception return must carry the interrupted T bit.

    python3 tools/p5bb_irq_state_splice.py <in.c> <out.c>

WHY
---
Two places in the frozen prelude lose the interrupted instruction state, and
the dispatch has no other way to learn it:

1. `irq_enter` synthesises SPSR_irq from the NZCV flags plus `cpsr_ctl`:

       spsr_bank[MODE_IRQ] = (n<<31)|(z<<30)|(c<<29)|(v<<28)|0x10u|cpsr_ctl;

   `cpsr_ctl` is a control byte WITHOUT the T bit: the runtime resolves the
   instruction state per dispatch (compiled case form / carrier / pc-form map)
   and never stores it there, because a `mrs rd, CPSR` inside ARM code must
   read T=0.  So every IRQ taken over Thumb code told the guest its interrupted
   thread was in ARM state.

2. `bios_irq_return` resumes the interrupted pc without telling the dispatcher
   what state the SPSR named, so the resumed pc falls through 5AZ's
   "predecessor state" rule (the ARM BIOS IRQ stub) and is decoded ARM.

Measured (Phase-5BB, savestate at frame 4201 + the Phase-5BA drive script):
  * step 3,064,069,170 `0x0200729C subs pc, lr, #4` restored SPSR 0x4000001F
    (T=0) with lr = 0x02137A96 -> target lr-4 = 0x02137A92.  That address is
    Thumb code (ov17 `adds r0, #16`; the image carries no compiled case
    anywhere in 0x02137Axx), so the run executed it as ARM: 0x02137A92/96/9A/
    9E/A2, then the ARM reading of `pop {r4,r5,r6,pc}` popped 0x02267068
    (heap), executed the heap as ARM and halted on SWI #0x65 (0xEF656C00) at
    0x02270CDC.
  * with only (1) fixed, step 3,124,271,861 resumed the Thumb `blx
    0x02043BD0` at 0x020D827A out of the BIOS IRQ stub as ARM and jumped to
    0x08BD8000 (`untranslated pc`).

WHAT
----
  1. forward-declare the runtime's state witness (`nnk_state_of_next`, in
     tools/nnk_smc.inc, included later in the same TU) and the resume carrier
     (`nnk_smc_note_resume`);
  2. write SPSR_irq's T bit from that witness (stream stride, else the
     compiled pc-form map) instead of losing it;
  3. keep `cpsr_ctl` free of the bit on the IRQ return (`& 0xDFu`), so the fix
     cannot leak a stale T into later `mrs rd, CPSR` reads (today the SPSR
     never carried the bit, so this masks nothing that was ever set);
  4. stamp the resume carrier from the SPSR so the dispatch resumes the
     interrupted pc in the state the guest was told about.

The canonical text lives in nnktool/src/recompile.rs (irq_enter /
bios_irq_return) and tools/nnk_smc.inc; this transform applies the identical
effect to a frozen image that predates it, like the Phase-5AW/5AF/5AT splices.

Idempotent: every item is applied only if it is still missing.
"""
import sys

MARKER = "/* Phase-5BB IRQ-state splice applied */\n"
ENTER = "static void irq_enter(uint32_t *pc) {\n"
FORWARD = """/* Phase-5BB IRQ-state splice applied */
/* The interrupted CPSR's T bit: the state witness and the resume carrier
 * (both defined in tools/nnk_smc.inc, included later in this TU). */
static int nnk_state_of_next(uint32_t next, uint32_t prev);
void nnk_smc_note_resume(uint32_t target, uint32_t spsr);
"""
OLD_SPSR = ("    spsr_bank[MODE_IRQ] = (n << 31) | (z << 30) | (c << 29) | "
            "(v << 28) | 0x10u | cpsr_ctl;\n")
NEW_SPSR = """    /* Phase-5BB: the interrupted T bit is part of the CPSR and only exists
     * here (cpsr_ctl is deliberately state-free).  Witness: stream stride,
     * else the compiled pc-form map. */
    spsr_bank[MODE_IRQ] = (n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x10u |
                          (cpsr_ctl & ~0x20u) |
                          (nnk_state_of_next(resume, last_pc) ? 0x20u : 0u);
"""
OLD_RET = "        cpsr_set_ctl(sr & 0xFFu);       /* back to the interrupted mode+bank */\n"
NEW_RET = """        /* Phase-5BB: mode+bank only -- cpsr_ctl stays the state-free control
         * byte; the T bit lives in the guest-visible SPSR. */
        cpsr_set_ctl(sr & 0xDFu);
"""
OLD_RESUME = "    *pc = lr - 4u;\n"
NEW_RESUME = """    *pc = lr - 4u;
    /* Phase-5BB: resume the interrupted instruction in the state the SPSR
     * names (the BIOS return lands in the same loop iteration as the
     * dispatch, so the carrier is stamped for the current step). */
    nnk_smc_note_resume(*pc, spsr_bank[MODE_IRQ]);
"""


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()
    todo = []
    need_fwd = MARKER.strip() not in text.split("\n", 3)[0]
    need_resume = OLD_RESUME in text and "nnk_smc_note_resume(*pc" not in text
    if need_fwd:
        todo.append("forward-declaration")
    if OLD_SPSR in text:
        todo.append("SPSR_irq T bit")
    if OLD_RET in text:
        todo.append("IRQ-return mask")
    if need_resume:
        todo.append("resume carrier")
    if not todo:
        with open(dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(text)
        print("p5bb: %s already spliced" % src)
        return
    if text.count(ENTER) != 1:
        sys.exit("p5bb: irq_enter anchor matched %d times in %s"
                 % (text.count(ENTER), src))
    if OLD_SPSR in text and text.count(OLD_SPSR) != 1:
        sys.exit("p5bb: SPSR line matched %d times in %s"
                 % (text.count(OLD_SPSR), src))
    if OLD_RET in text and text.count(OLD_RET) != 1:
        sys.exit("p5bb: IRQ-return line matched %d times in %s"
                 % (text.count(OLD_RET), src))
    if OLD_RESUME in text and text.count(OLD_RESUME) != 1:
        sys.exit("p5bb: resume line matched %d times in %s"
                 % (text.count(OLD_RESUME), src))
    if MARKER.strip() not in text.split("\n", 3)[0]:
        text = text.replace(ENTER, FORWARD + ENTER)
    if OLD_SPSR in text:
        text = text.replace(OLD_SPSR, NEW_SPSR)
    if OLD_RET in text:
        text = text.replace(OLD_RET, NEW_RET)
    if OLD_RESUME in text and "nnk_smc_note_resume(*pc" not in text:
        text = text.replace(OLD_RESUME, NEW_RESUME)
    with open(dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)
    print("p5bb: spliced %s into %s -> %s" % (", ".join(todo), src, dst))


if __name__ == "__main__":
    main()
