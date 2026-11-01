#!/usr/bin/env python3
"""Phase-5AB: probe the VRAM read shadow's round trip on the watched window.

Splices a bounded, PC/step-tagged access log into the guest accessors of a
frozen dispatch image (after the Phase-5X read shadow, which this tool also
applies when it is missing).

Why: the halt at 0x020E1B32 reads [0x06006004] (Thumb `ldr r4,[r0]`, r0 =
object+4 = 0x06006004), then `ldr r4,[r4,#0x24]`, then `blx r4`.  The operator
sees r4 = 0xE3A03102.  Two very different causes produce that:
  (a) the shadow hands back someone else's bytes for [0x06006004] (so the
      "vptr" is garbage and [vptr+0x24] is whatever that lands on), or
  (b) [0x06006004] is right and the vtable slot at +0x24 is wrong.
The log distinguishes them: every access to the watched range prints the
address, width, the stored/loaded value AND what the shadow currently holds at
0x06006004 (0xDEADBEEF when no shadow is allocated).

Env:
  NNK_P5AB=1            enable
  NNK_P5AB_LO=<addr>    watch window low (default 0x06006000)
  NNK_P5AB_HI=<addr>    watch window high, exclusive (default 0x06006010)
  NNK_P5AB_WN=<n>       writes to log (default 200)
  NNK_P5AB_RN=<n>       reads to log (default 400)
  NNK_P5AB_FROM=<step>  ignore accesses before this step (default 0)

usage: p5ab_shadow_probe.py <image.c> <out.c>
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

BLOCK = r'''/* ---- Phase-5AB: VRAM read-shadow round-trip probe ----------------------
 * Bounded, PC/step-tagged log of every guest access to one watched window
 * (default 0x06006000..0x06006010, the C++ object the halt at 0x020E1B32
 * dereferences), plus what the Phase-5X shadow currently holds at 0x06006004.
 * Armed only by NNK_P5AB=1; two compares per guest access when off. */
static int p5ab_on = -1;
static uint32_t p5ab_lo = 0x06006000u, p5ab_hi = 0x06006010u;
static long p5ab_wleft = 200, p5ab_rleft = 400;
static unsigned long long p5ab_from;
static void p5ab_hit(uint32_t a, uint32_t n, int wr, uint32_t v);
static void p5ab_init(void) {
    const char *s;
    p5ab_on = 0;
    s = getenv("NNK_P5AB");
    if (!s || !s[0]) return;
    p5ab_on = 1;
    if ((s = getenv("NNK_P5AB_LO"))   && s[0]) p5ab_lo = (uint32_t)strtoul(s, NULL, 0);
    if ((s = getenv("NNK_P5AB_HI"))   && s[0]) p5ab_hi = (uint32_t)strtoul(s, NULL, 0);
    if ((s = getenv("NNK_P5AB_WN"))   && s[0]) p5ab_wleft = atol(s);
    if ((s = getenv("NNK_P5AB_RN"))   && s[0]) p5ab_rleft = atol(s);
    if ((s = getenv("NNK_P5AB_FROM")) && s[0]) p5ab_from = strtoull(s, NULL, 0);
    fprintf(stderr, "p5ab: probe armed range=0x%08X..0x%08X from_step=%llu\n",
            p5ab_lo, p5ab_hi, p5ab_from);
}
static uint32_t p5ab_peek(void) {
    const uint8_t *q = gxd_sh_rd(0x06006004u, 4u);
    if (!q) return 0xDEADBEEFu;                 /* shadow not allocated yet */
    return (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
           ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
}
static void p5ab_hit(uint32_t a, uint32_t n, int wr, uint32_t v) {
    if (p5ab_on < 0) p5ab_init();               /* env-gated, one-time */
    if (p5ab_on != 1) return;
    if (a < p5ab_lo || a >= p5ab_hi) return;
    if (steps < p5ab_from) return;
    if (wr) { if (p5ab_wleft <= 0) return; p5ab_wleft--; }
    else    { if (p5ab_rleft <= 0) return; p5ab_rleft--; }
    fprintf(stderr, "p5ab %s step=%llu pc=0x%08X a=0x%08X n=%u v=0x%08X sh04=0x%08X\n",
            wr ? "w" : "r", (unsigned long long)steps, last_pc, a,
            (unsigned)n, v, p5ab_peek());
}
'''

HOOKS = (
    ("static inline void st32(uint32_t a, uint32_t v) {",
     "static inline void st32(uint32_t a, uint32_t v) {\n    p5ab_hit(a, 4u, 1, v);"),
    ("static inline void st16(uint32_t a, uint16_t v) {",
     "static inline void st16(uint32_t a, uint16_t v) {\n    p5ab_hit(a, 2u, 1, (uint32_t)v);"),
    ("static inline void st8(uint32_t a, uint32_t v) {",
     "static inline void st8(uint32_t a, uint32_t v) {\n    p5ab_hit(a, 1u, 1, v);"),
    ("static inline uint32_t ld32(uint32_t a) {",
     "static inline uint32_t ld32(uint32_t a) {\n    p5ab_hit(a, 4u, 0, 0u);"),
    ("static inline uint32_t ld16(uint32_t a) {",
     "static inline uint32_t ld16(uint32_t a) {\n    p5ab_hit(a, 2u, 0, 0u);"),
    ("static inline uint32_t ld8(uint32_t a) {",
     "static inline uint32_t ld8(uint32_t a) {\n    p5ab_hit(a, 1u, 0, 0u);"),
)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    tmp = src
    s = open(src).read()
    if 'gxd_sh_rd' not in s:
        tmp = tempfile.mktemp(suffix='.p5x.c')
        subprocess.check_call([sys.executable, os.path.join(HERE, 'p5x_runtime_splice.py'),
                               src, tmp])
        s = open(tmp).read()
        print('applied p5x read shadow')
    if 'p5ab_hit' in s:
        sys.exit('already spliced: %s' % src)
    anchor = 'static inline uint32_t ld32(uint32_t a) {\n#ifdef NNK_GX'
    if s.count(anchor) != 1:
        sys.exit('ld32 anchor matched %d times' % s.count(anchor))
    s = s.replace(anchor, BLOCK + '\n' + anchor, 1)
    for old, new in HOOKS:
        if s.count(old) != 1:
            sys.exit('hook anchor %r matched %d times' % (old, s.count(old)))
        s = s.replace(old, new, 1)
    open(dst, 'w').write(s)
    if tmp != src:
        os.unlink(tmp)
    print('spliced p5ab probe -> %s' % dst)


if __name__ == '__main__':
    main()
