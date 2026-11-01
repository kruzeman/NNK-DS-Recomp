#!/usr/bin/env python3
"""Phase-5AE: splice the per-step runtime speed-ups into a frozen dispatch image.

The canonical change lives in `nnktool/src/recompile.rs` (the prelude template),
which is what a regenerated image picks up.  This script applies the IDENTICAL
text transforms to a frozen image (`work/phase3a/arm9_thumb_ovl_itcm_p5u.c`) so
the optimisations can be measured on the exact guest parts the previous phases
used, without a full recompile-all round trip.  Same precedent as
tools/p5x_runtime_splice.py.

Every transform is semantics-preserving (no guest-visible change):

  1. vcount()            per-call 64-bit division (cycles / 2130) % 263 becomes
                         a cached scanline window; `cycles` is monotonic.  Up to
                         four calls per dispatched guest instruction.
  2. nnk_display_tick()  the vc-unchanged case returns before the tail call into
                         libgxrender (gxd_should_quit); frame presentation is on
                         the same vc edge it already tested.
  3. trace_pc()          one gate for the per-step debug/probe chain: the four
                         env-gated dumps are skipped when none of their parsed
                         addresses is configured, and p5u_hook() is skipped when
                         NNK_P5U is off (it self-initialises on the first call).

usage: p5ae_runtime_splice.py <image.c> <out.c>
"""
import sys

T1_OLD = ("static uint32_t vcount(void) { return (uint32_t)((cycles / CYCLES_PER_SCANLINE) "
          "% SCANLINES); }")

T1_NEW = """/* Phase-5AE: vcount() is called up to four times per dispatched guest
 * instruction (the VBlank-IF producer, the DMA tick, the gx display tick and
 * trace_pc), and each call was a 64-bit division by 2130 plus a modulo - ~40
 * cycles a time on the measured host.  `cycles` only ever moves forward, so
 * cache the scanline for the 2130-cycle window that contains it and answer the
 * common case with two compares.  The returned value is bit-identical to
 * `(cycles / CYCLES_PER_SCANLINE) % SCANLINES` for every input; a jump outside
 * the cached window (a card stall advancing `cycles`, or a fresh start)
 * recomputes it. */
static unsigned long long vc_win_lo, vc_win_hi;
static uint32_t vc_win_val;
static uint32_t vcount(void) {
    if (cycles >= vc_win_lo && cycles < vc_win_hi) return vc_win_val;
    { unsigned long long q = cycles / CYCLES_PER_SCANLINE;
      vc_win_lo = q * CYCLES_PER_SCANLINE;
      vc_win_hi = vc_win_lo + CYCLES_PER_SCANLINE;
      vc_win_val = (uint32_t)(q % SCANLINES); }
    return vc_win_val;
}"""

T2_OLD = """static int nnk_display_tick(void) {
    uint32_t vc;
    if (!gxd) return 0;
    vc = vcount();
    if (gxd_last_vc < 192u && vc >= 192u) {"""

T2_NEW = """static int nnk_display_tick(void) {
    uint32_t vc;
    if (!gxd) return 0;
    vc = vcount();
    /* Phase-5AE: inside one scanline there is nothing to do, and the tail call
     * into libgxrender (gxd_should_quit) was made on EVERY dispatched guest
     * instruction.  Poll once per scanline instead: the frame edge tested below
     * is the only state this function has, so presentation is unchanged and
     * the window-close latency becomes <2130 instructions. */
    if (vc == (uint32_t)gxd_last_vc) return 0;
    if (gxd_last_vc < 192u && vc >= 192u) {"""

T3A_OLD = """static void p5u_hook(uint32_t pc); /* p5u probe */
static void trace_pc(uint32_t pc) {
    p5u_hook(pc);"""

T3A_NEW = """static void p5u_hook(uint32_t pc); /* p5u probe */
/* Phase-5AE: one gate for the per-step debug/probe chain.  Every probe below is
 * inert unless its env var is set, but each still cost a call, a static load and
 * a test on EVERY dispatched guest instruction (TSC profiler: 26 cyc/step for
 * p5u_hook plus 32 cyc/step for the four dump probes).
 *   nnk_probe_mask  computed once in main() from the parsed probe addresses;
 *                   when it is 0 each of those probes would return at its first
 *                   line, so the chain can be skipped wholesale.
 *   nnk_p5u_on      -1 until p5u_init() has parsed NNK_P5U (the first call runs
 *                   it), 0 = off, 1 = on.  nnk_smc.inc sets it when it sees
 *                   NNK_P5U_GATE below. */
#define NNK_P5U_GATE 1
static int nnk_probe_mask;
static int nnk_p5u_on = -1;
static void trace_pc(uint32_t pc) {
    if (nnk_p5u_on) p5u_hook(pc);"""

T3B_OLD = """    checkpoint_dump(pc);
    debug_dump(pc);
    itcm_dump_once(pc);
    dc_probe(pc);
    pred_probe(pc);"""

T3B_NEW = """    /* Phase-5AE: itcm_dump_once() is unconditional on the FIRST ITCM dispatch
     * (its line is part of every captured run), so it keeps an arms-length
     * test; the four env-gated probes run only when at least one of their
     * addresses is configured. */
    if (!itcm_dumped && pc >= ITCM_LO && pc < ITCM_HI) itcm_dump_once(pc);
    if (nnk_probe_mask) {
        checkpoint_dump(pc);
        debug_dump(pc);
        dc_probe(pc);
        pred_probe(pc);
    }"""

T3C_OLD = """    printf("nnk recompile-all ARM9 entry 0x%08X\\n", ENTRY_CONST);"""

T3C_NEW = """    /* Phase-5AE: the per-step probe chain is gated on this (see trace_pc). */
    nnk_probe_mask = (ckpt_pc || dbg_pc || dc_npc || pred_npc) ? 1 : 0;
    printf("nnk recompile-all ARM9 entry 0x%08X\\n", ENTRY_CONST);"""

TRANSFORMS = [
    ("vcount() cache", T1_OLD, T1_NEW),
    ("nnk_display_tick() scanline gate", T2_OLD, T2_NEW),
    ("trace_pc() probe gate", T3A_OLD, T3A_NEW),
    ("trace_pc() dump calls", T3B_OLD, T3B_NEW),
    ("main() probe mask", T3C_OLD, T3C_NEW),
]


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    txt = open(src).read()
    for name, old, new in TRANSFORMS:
        n = txt.count(old)
        if n != 1:
            sys.stderr.write("p5ae splice: %s: expected 1 match, found %d\n" % (name, n))
            return 1
        txt = txt.replace(old, new)
    open(dst, "w").write(txt)
    sys.stderr.write("Phase-5AE: %d prelude transforms spliced into %s -> %s\n"
                     % (len(TRANSFORMS), src, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
