#!/usr/bin/env python3
"""Phase-5BL splice: the GXSTAT (0x04000600) read path + GXFIFO IRQ line.

    python3 tools/p5bl_gxstat_splice.py <in.c> <out.c>

WHY
---
`work/phase3a/arm9_thumb_ovl_itcm_p5u.c` is a frozen `nnktool recompile-all`
artifact.  Its `io_read32` has no case for GXSTAT, so the register read back 0
and the guest's "wait until the GX command FIFO is less than half full" poll
never ended:

    0x0200985C  ldr  r0,[0x04000600]
    0x02009860  and  r0,r0,#0x07000000
    0x02009864  lsr  r0,r0,#24
    0x02009868  tst  r0,#2
    0x0200986C  beq  0x0200985C

Phase-5BK measured 99.6 % of the post-skip trace in that loop and the
instruction after it never executing; answering "Yes" at the skip prompt
therefore left both screens black.

WHAT (three text transforms, identical to the canonical prelude in
`nnktool/src/recompile.rs`; the register/IRQ model itself lives in
`gxrender/src/{gx_display.c,gx_fifo.c}` and is compiled live, not spliced):

 1. `io_read32` gains the GXSTAT window case (width/byte-offset aware, so a
    16-bit read of 0x04000602 and a byte read of 0x04000601 return what
    melonDS GPU3D::Read16/Read8 would).
 2. `nnk_gx_irq_eval()` is defined next to the write entry points and called
    after every 32/16-bit IO write: melonDS calls GPU3D::CheckFIFOIRQ from
    WriteToGXFIFO, the GXSTAT write and the IF acknowledge, and those are the
    only three places where the level-sensitive GXFIFO IRQ line (IF bit 21)
    can change.
 3. Both are wired into the frozen image's `io_write32` / `io_write16` (the
    frozen image predates the Phase-4E+ `io2d_write()` lines, so its bodies
    are anchored without them).

 3. The immediate-DMA path (`io_do_dma`) raises IRQ_DMA0+i when DMAxCNT bit 30
    is set, exactly as melonDS DMA::End does.  The SDK's async-DMA queue is
    retired by that interrupt (its completion callback is what clears the
    queue flag at 0x02074EB4).

Exit status is non-zero if an anchor is missing or matches more than once, so
a stale image cannot silently ship without the fix.
"""
import sys

MARKER = "/* Phase-5BL GXSTAT splice applied: read path + GXFIFO IRQ line */\n"

DMAIRQ_ANCHOR = """    dma_cnt[i] = cnt & ~0x80000000u;         /* clear enable (non-repeating) */
"""

DMAIRQ_BLOCK = """    dma_cnt[i] = cnt & ~0x80000000u;         /* clear enable (non-repeating) */
    /* Phase-5BL: melonDS DMA::End raises IRQ_DMA0+Num when Cnt bit30 is set;
     * the SDK's async-DMA queue retires through that interrupt. */
    if (cnt & (1u << 30)) iflags |= 1u << (8 + i);
"""

READ_ANCHOR = """static uint32_t io_read32(uint32_t a) {
    switch (a) {"""

READ_BLOCK = """static uint32_t io_read32(uint32_t a) {
    /* Phase-5BL: GXSTAT (0x04000600) read path.  The guest waits for
     * "command FIFO less than half full" (bit 25) before every GX submission:
     *   0x0200985C  ldr  r0,[0x04000600]
     *   0x02009860  and  r0,r0,#0x07000000
     *   0x02009864  lsr  r0,r0,#24
     *   0x02009868  tst  r0,#2
     *   0x0200986C  beq  0x0200985C
     * io_read32 had NO case for the register, so it returned 0 and the poll
     * could never end (Phase-5BK 3: 99.6 % of the post-skip trace in that
     * loop, the instruction after it never executed).  The status is modelled
     * in gx_display from this renderer's own FIFO/stack state (never a
     * constant: bits 25/26 and the level follow gx_fifo_level), and the byte
     * offset of a narrower access selects the byte melonDS's Read8/16 would
     * return. */
    if ((a & ~3u) == 0x04000600u) {
        uint32_t w;
#ifdef NNK_GX
        if (gxd) w = gxd_io_read(gxd, 0x04000600u, 4);
        else
#endif
        w = 0x06000000u;      /* no renderer: an idle GXFIFO (bit25|bit26) */
        return w >> ((a & 3u) * 8u);
    }
    switch (a) {"""

IRQ_ANCHOR = """static void io_write32(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 4);
#endif
    io_write32_sw(a, v);
}
static void io_write16(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 2);
#endif
    io_write16_sw(a, v);
}"""

IRQ_BLOCK = """/* Phase-5BL: mirror melonDS GPU3D::CheckFIFOIRQ into IF bit 21 (IRQ_GXFIFO).
 * melonDS re-evaluates the level-sensitive line at exactly three points: a
 * GXFIFO write (WriteToGXFIFO), a GXSTAT write, and an IF acknowledge
 * (NDS.cpp Write32 0x04000214).  All three are 32/16-bit IO writes here, so
 * one hook after the register model covers them.  The condition itself lives
 * in gx_display (GXStat bits 30-31 + our own FIFO level); with bits 30-31
 * never written this is a no-op, so the model costs nothing on paths that do
 * not use the GXFIFO interrupt. */
#ifdef NNK_GX
static void nnk_gx_irq_eval(void) {
    if (!gxd) return;
    if (gxd_gxfifo_irq(gxd)) iflags |= 0x00200000u;
    else                      iflags &= ~0x00200000u;
}
#else
#define nnk_gx_irq_eval() ((void)0)
#endif
static void io_write32(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 4);
#endif
    io_write32_sw(a, v);
    nnk_gx_irq_eval();
}
static void io_write16(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 2);
#endif
    io_write16_sw(a, v);
    nnk_gx_irq_eval();
}"""


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: p5bl_gxstat_splice.py <in.c> <out.c>")
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src, encoding="utf-8").read()
    if "Phase-5BL GXSTAT splice applied" in s:
        print("p5bl gxstat splice: already applied (%s)" % src)
        if dst != src:
            open(dst, "w", encoding="utf-8").write(s)
        return 0
    for what, old, new in (("io_read32 GXSTAT case", READ_ANCHOR, READ_BLOCK),
                           ("io_write32/16 GXFIFO IRQ hook", IRQ_ANCHOR, IRQ_BLOCK),
                           ("io_do_dma completion IRQ", DMAIRQ_ANCHOR, DMAIRQ_BLOCK)):
        n = s.count(old)
        if n != 1:
            sys.exit("p5bl gxstat splice: %s anchor matched %d times (want 1)"
                     % (what, n))
        s = s.replace(old, new, 1)
    open(dst, "w", encoding="utf-8").write(MARKER + s)
    print("p5bl gxstat splice: GXSTAT read case + GXFIFO IRQ hook -> %s" % dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
