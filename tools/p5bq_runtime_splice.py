#!/usr/bin/env python3
"""Phase-5BQ: splice the 64-bit divider + FIFO producer tag into a frozen image.

The canonical change lives in `nnktool/src/recompile.rs` (the prelude template),
which is what a regenerated image picks up.  This script applies the IDENTICAL
text transforms to a frozen dispatch image so the fix can be measured on the
exact guest parts the previous phases used, without a full recompile-all round
trip.  Same precedent as tools/p5ae_runtime_splice.py and
tools/p5x_runtime_splice.py.

Transforms:

  1. the DIV unit keeps the FULL 64-bit signed quotient/remainder
     (melonDS NDS::DivDone writes `*(s64*)&DivQuotient[0]`), exposes the high
     words at 0x040002A4 / 0x040002AC (NDS.cpp:3117-3120, 3339-3342), treats
     DIVCNT mode 3 as 64/32 like mode 1, and answers divide-by-zero with
     quotient 1/-1 by the numerator's sign + remainder = numerator (bit14 set)
     exactly as DivDone does;
  2. the guest store path stamps the producing pc (runtime `last_pc`) into
     gxd_note_pc so gxrender's FIFO producer ring can name the writer.

Why (measured, Phase-5BQ): on the operator save the 3D camera basis builder
0x0200172C produced an all-zero 3x3, so the projection 4x3 handed to the GX
FIFO was twelve zero words.  Its three vector helpers are 1.19.12 fixed point
built on the DS math unit; the SDK normalise at 0x02002604 divides 2^56 by
|v|^2 and multiplies the SQRT result by the 64-bit quotient, so a truncated
32-bit quotient loses the whole result whenever |v|^2 < 2^24 (the common case
for these basis vectors).

usage: p5bq_runtime_splice.py <image.c> <out.c>
"""
import sys

T1_OLD = "static uint32_t div_ctrl = 0, div_quot = 0, div_rem = 0;"

T1_NEW = """static uint32_t div_ctrl = 0, div_quot = 0, div_rem = 0;
/* Phase-5BQ DIV64 splice applied */
/* Phase-5BQ: the DS divider produces a FULL 64-bit signed quotient and
 * remainder.  melonDS NDS::DivDone writes `*(s64*)&DivQuotient[0]` and
 * `*(s64*)&DivRemainder[0]`, and Read32 exposes DivQuotient[1] at
 * 0x040002A4 and DivRemainder[1] at 0x040002AC (NDS.cpp:3117-3120,
 * 3339-3342).  Keeping only the low words silently zeroed every 64-bit
 * reciprocal the SDK builds (0x02002604 computes 2^56/|v|^2 and multiplies it
 * by the SQRT result), which collapsed the 3D camera basis to zeros. */
static uint32_t div_quot_hi = 0, div_rem_hi = 0;"""

T2_OLD = """static void io_div_do(void) {
    uint32_t mode = div_ctrl & 3u;
    int64_t num, den; int64_t q, rm;
    if (mode == 0) { num = (int32_t)div_num_lo; den = (int32_t)div_den_lo; }
    else if (mode == 1) { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int32_t)div_den_lo; }
    else { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int64_t)(((uint64_t)div_den_hi << 32) | div_den_lo); }
    if (den == 0) { div_ctrl |= 0x4000u; div_quot = 0; div_rem = 0; return; }
    q = num / den; rm = num % den;
    div_quot = (uint32_t)q; div_rem = (uint32_t)rm;
    div_ctrl |= 0x8000u;   /* busy then immediately clear (approximation) */
    div_ctrl &= ~0x8000u;
}"""

T2_NEW = """static void io_div_do(void) {
    uint32_t mode = div_ctrl & 3u;
    int64_t num, den; int64_t q, rm;
    if (mode == 0) { num = (int32_t)div_num_lo; den = (int32_t)div_den_lo; }
    else if (mode == 1 || mode == 3) { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int32_t)div_den_lo; }
    else { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int64_t)(((uint64_t)div_den_hi << 32) | div_den_lo); }
    div_ctrl &= ~0x4000u;
    if (den == 0) {
        /* melonDS NDS::DivDone: divide-by-zero answers quotient 1/-1 by the
         * numerator's sign and remainder = numerator, then sets bit14. */
        q = (num < 0) ? 1 : -1;
        rm = num;
        div_ctrl |= 0x4000u;
    } else if ((uint64_t)num == 0x8000000000000000ull && den == -1) {
        q = (int64_t)0x8000000000000000ull; rm = 0;
    } else {
        q = num / den; rm = num % den;
    }
    div_quot = (uint32_t)(uint64_t)q; div_quot_hi = (uint32_t)((uint64_t)q >> 32);
    div_rem  = (uint32_t)(uint64_t)rm; div_rem_hi = (uint32_t)((uint64_t)rm >> 32);
    div_ctrl |= 0x8000u;   /* busy then immediately clear (approximation) */
    div_ctrl &= ~0x8000u;
}"""

T3_OLD = """    case 0x040002A0: return div_quot;
    case 0x040002A8: return div_rem;"""

T3_NEW = """    case 0x040002A0: return div_quot;
    case 0x040002A4: return div_quot_hi;
    case 0x040002A8: return div_rem;
    case 0x040002AC: return div_rem_hi;"""

T4_OLD = """#else
#define nnk_display_tick() 0"""

T4_NEW = """#else
/* no display bridge: the Phase-5BQ FIFO producer tag has nowhere to go */
#define gxd_note_pc(pc) ((void)0)
#define nnk_display_tick() 0"""

TRANSFORMS = [
    ("DIV 64-bit result fields", T1_OLD, T1_NEW),
    ("io_div_do 64-bit + melonDS edge cases", T2_OLD, T2_NEW),
    ("DIV_RESULT_H / DIVREM_RESULT_H reads", T3_OLD, T3_NEW),
    ("gxd_note_pc fallback", T4_OLD, T4_NEW),
]

for _w in ("32", "16", "8"):
    TRANSFORMS.append((
        "FIFO producer tag (io_write%s)" % _w,
        "if (a >= IO_LO && a < IO_HI) { io_write%s(a, v); return; }" % _w,
        "if (a >= IO_LO && a < IO_HI) {            /* Phase-5BQ: tag the FIFO producer */\n"
        "        gxd_note_pc(last_pc);\n"
        "        io_write%s(a, v); return; }" % _w,
    ))


def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    txt = open(src).read()
    for name, old, new in TRANSFORMS:
        n = txt.count(old)
        if n != 1:
            sys.stderr.write("p5bq splice: %s: expected 1 match, found %d\n" % (name, n))
            return 1
        txt = txt.replace(old, new)
    open(dst, "w").write(txt)
    sys.stderr.write("Phase-5BQ: %d prelude transforms spliced into %s -> %s\n"
                     % (len(TRANSFORMS), src, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
