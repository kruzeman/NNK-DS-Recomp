#!/usr/bin/env python3
"""Phase-5BV: splice the GX matrix READBACK into a frozen dispatch image.

The canonical change lives in `nnktool/src/recompile.rs` (the prelude template),
which is what a regenerated image picks up.  This script applies the IDENTICAL
text transform to a frozen dispatch image so the fix can be measured on the
exact guest parts the previous phases used, without a recompile-all round trip.
Same precedent as tools/p5bq_runtime_splice.py, tools/p5bl_gxstat_splice.py,
tools/p5ae_runtime_splice.py and tools/p5x_runtime_splice.py.

WHY (measured, Phase-5BU/5BV): the guest's SDK matrix helpers read the hardware
back instead of keeping a copy in RAM:

    0x02003360  push {r3,lr}; r2 = GXSTAT (0x04000600)
                if (r2 bit27) return -1            ; GX busy
                copy 16 words from r2+0x40 (0x04000640) to the caller's buffer
    0x02003390  same, copying 12 words from r2+0x80 (0x04000680)

melonDS answers those windows from the live matrix state (GPU3D::Read32):

    0x04000640..0x0400067F   UpdateClipMatrix(); ClipMatrix[(addr & 0x3C) >> 2]
    0x04000680..0x040006A0   VecMatrix[0,1,2,4,5,6,8,9,10]

with no Read8/Read16 case (those answer 0).  io_read32 had no case for either
window, so every read returned 0, the guest copied zeros into its node-matrix
table (`0x02009C64` copy leaf -> `0x020735B0 + node*100`), the interpolation at
`0x02035134` then accumulated zeros, and the SDK sent

    MTX_LOAD_4x3  mode 2  pc 0x02035808  (buffer sp+0xE0)
    MTX_LOAD_4x3  mode 1  pc 0x0203581C  (buffer sp+0xB0)

with nine zero basis words.  Vertices submitted in that state collapse to the
projection's translation column (Phase-5BU: clip = -614400, 177418, 32768 for
W = 131072) and the homogeneous clip turns that into the full-width sliver
polygons that paint the blue band.

usage: p5bv_gx_matrix_readback_splice.py <image.c> <out.c>
"""

import sys

MARKER = 'Phase-5BV matrix-readback splice applied'

OLD = """        w = 0x06000000u;      /* no renderer: an idle GXFIFO (bit25|bit26) */
        return w >> ((a & 3u) * 8u);
    }
    switch (a) {
"""

NEW = """        w = 0x06000000u;      /* no renderer: an idle GXFIFO (bit25|bit26) */
        return w >> ((a & 3u) * 8u);
    }
    /* Phase-5BV matrix-readback splice applied */
    /* Phase-5BV: GX matrix readback (melonDS GPU3D::Read32 0x04000640 and
     * 0x04000680).  The SDK's GX_GetPosMatrix leaf 0x02003360 busy-checks
     * GXSTAT and then copies the sixteen words at 0x04000640 into the caller's
     * buffer (its vector twin 0x02003390 copies twelve words at 0x04000680);
     * the hardware answers with its CURRENT matrix, i.e. ClipMatrix =
     * ProjMatrix * PosMatrix.  io_read32 had no case for either window, so the
     * copy produced zeros, every node matrix rebuilt from the hardware was
     * zero (0x02009C64 copies the clip matrix into 0x020735B0 + node*100), and
     * the guest then sent MTX_LOAD_4x3 with nine zero basis words (Phase-5BU:
     * pcs 0x02035808/0x0203581C under the copy leaf 0x02005CB0), collapsing
     * those strips' clip to the projection's translation column -- the blue
     * band.  melonDS has no Read8/Read16 case for this window and answers 0,
     * hence the 32-bit-only condition. */
    if (a >= 0x04000640u && a < 0x040006A4u && (a & 3u) == 0u) {
#ifdef NNK_GX
        if (gxd) return gxd_io_read(gxd, a, 4);
#endif
        return 0;
    }
    switch (a) {
"""


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: p5bv_gx_matrix_readback_splice.py <image.c> <out.c>')
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src, encoding='utf-8').read()
    if MARKER in text:
        print('%s already carries the Phase-5BV readback; writing %s unchanged'
              % (src, dst))
        open(dst, 'w', encoding='utf-8').write(text)
        return
    if text.count(OLD) != 1:
        sys.exit('p5bv: anchor found %d times in %s (expected 1)'
                 % (text.count(OLD), src))
    open(dst, 'w', encoding='utf-8').write(text.replace(OLD, NEW, 1))
    print('p5bv splice: GX matrix readback -> %s' % dst)


main()
