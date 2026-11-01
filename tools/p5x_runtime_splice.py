#!/usr/bin/env python3
"""Phase-5X: splice the VRAM/palette/OAM READ SHADOW into a frozen dispatch
image.

The canonical change lives in `nnktool/src/recompile.rs` (the guest accessor
template), which is what a regenerated image picks up.  This script applies the
IDENTICAL text transform to the frozen Phase-5U image so the fix can be
measured against the exact binary the previous phases used, without a full
recompile-all (arm9 + overlay + ITCM merge) round trip.

The defect being fixed (measured): the merged display API is write-only for the
windows it owns.  `gxd_io_write` routes palette 0x05000000, VRAM 0x06000000..
0x0689FFFF and OAM 0x07000000 into its VRAMCNT-mapped banks, `gxd_io_read`
serves only KEYINPUT/KEYCNT/VRAMSTAT (0 for every window byte), and the
runtime's ld32/ld16/ld8 never consulted the window at all — so a guest read of
VRAM returned 0 unconditionally.  On the post-skip path a C++ constructor
stores its vptr at 0x06006004 and the virtual call 267 steps later reads 0
back, loads [0x24] and branches to 0xE3A03102 ("dispatch to untranslated pc").

usage: p5x_runtime_splice.py <image.c> <out.c>
"""
import sys

SHADOW = r'''/* Phase-5X: read shadow for the GX-routed memory windows.
 *
 * WHY (measured): the merged display API is WRITE-ONLY for the windows it
 * owns.  `gxd_io_write` routes palette (0x05000000), VRAM (0x06000000..
 * 0x0689FFFF) and OAM (0x07000000) bytes into its VRAMCNT-mapped banks, but
 * `gxd_io_read` serves only KEYINPUT/KEYCNT/VRAMSTAT and returns 0 for every
 * window address - and the runtime's own ld32/ld16/ld8 never looked at the
 * window at all, so a guest read of VRAM returned 0 unconditionally.
 *   The post-skip path keeps a C++ object in the window: the constructor at
 * 0x020E1AC4 stores its vptr into [0x06006004] (step 331,707,2xx) and the
 * virtual call `blx r4` 267 steps later read 0x00000000 back from that very
 * word, loaded [0x24] instead of a vtable slot and branched to 0xE3A03102.
 * The shadow makes the guest's read-modify-write self-consistent.  Writes
 * still go to gx_display unchanged (rendering path untouched), and reads fall
 * through to the old behaviour for every address outside the three windows. */
#ifdef NNK_GX
#define GXD_SH_VRAM 0x8A0000u          /* 0x06000000..0x0689FFFF */
#define GXD_SH_SIZE (0x800u + GXD_SH_VRAM + 0x800u)
static uint8_t *gxd_shadow;            /* lazily allocated on the first write */
static uint32_t gxd_sh_off(uint32_t a) {
    if (a >= 0x05000000u && a < 0x05000800u) return a - 0x05000000u;
    if (a >= 0x06000000u && a < 0x06000000u + GXD_SH_VRAM) return 0x800u + (a - 0x06000000u);
    if (a >= 0x07000000u && a < 0x07000800u) return 0x800u + GXD_SH_VRAM + (a - 0x07000000u);
    return 0xFFFFFFFFu;
}
/* write side: allocate on demand, NULL when the address is outside the windows */
static uint8_t *gxd_sh_wr(uint32_t a) {
    uint32_t o = gxd_sh_off(a);
    if (o == 0xFFFFFFFFu) return NULL;
    if (!gxd_shadow) gxd_shadow = (uint8_t *)calloc(1, GXD_SH_SIZE);
    return gxd_shadow ? gxd_shadow + o : NULL;
}
/* read side: an n-byte access that stays inside one window, else NULL */
static const uint8_t *gxd_sh_rd(uint32_t a, uint32_t n) {
    uint32_t o;
    if (!gxd_shadow) return NULL;
    o = gxd_sh_off(a);
    if (o == 0xFFFFFFFFu) return NULL;
    if (gxd_sh_off(a + n - 1u) != o + (n - 1u)) return NULL;
    return gxd_shadow + o;
}
#endif

'''

LD32_OLD = """static inline uint32_t ld32(uint32_t a) {
    if (a < itcm_size) {"""
LD32_NEW = """static inline uint32_t ld32(uint32_t a) {
#ifdef NNK_GX
    { const uint8_t *q = gxd_sh_rd(a, 4u);
      if (q) return (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24); }
#endif
    if (a < itcm_size) {"""

LD16_OLD = """static inline uint32_t ld16(uint32_t a) {
    if (a < itcm_size) {
        const uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);"""
LD16_NEW = """static inline uint32_t ld16(uint32_t a) {
#ifdef NNK_GX
    { const uint8_t *q = gxd_sh_rd(a, 2u);
      if (q) return (uint32_t)q[0] | ((uint32_t)q[1] << 8); }
#endif
    if (a < itcm_size) {
        const uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);"""

LD8_OLD = "static inline uint32_t ld8(uint32_t a) {\n    if (a < itcm_size) return itcm[a & (ITCM_PHYS - 1u)];"
LD8_NEW = """static inline uint32_t ld8(uint32_t a) {
#ifdef NNK_GX
    { const uint8_t *q = gxd_sh_rd(a, 1u); if (q) return (uint32_t)q[0]; }
#endif
    if (a < itcm_size) return itcm[a & (ITCM_PHYS - 1u)];"""

ST32_OLD = """#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 4); return; }
#endif"""
ST32_NEW = """#ifdef NNK_GX
    { uint8_t *q = gxd_sh_wr(a);        /* Phase-5X: shadow the guest's own write */
      if (q) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); q[2] = (uint8_t)(v >> 16); q[3] = (uint8_t)(v >> 24); } }
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 4); return; }
#endif"""

ST16_OLD = """#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 2); return; }
#endif"""
ST16_NEW = """#ifdef NNK_GX
    { uint8_t *q = gxd_sh_wr(a);
      if (q) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); } }
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 2); return; }
#endif"""

ST8_OLD = """#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 1); return; }
#endif"""
ST8_NEW = """#ifdef NNK_GX
    { uint8_t *q = gxd_sh_wr(a); if (q) q[0] = (uint8_t)v; }
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 1); return; }
#endif"""


def main():
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src).read()
    if 'gxd_sh_rd' in s:
        sys.exit('already spliced: %s' % src)
    for old, new, label in ((LD32_OLD, LD32_NEW, 'ld32'),
                            (LD16_OLD, LD16_NEW, 'ld16'),
                            (LD8_OLD, LD8_NEW, 'ld8'),
                            (ST32_OLD, ST32_NEW, 'st32'),
                            (ST16_OLD, ST16_NEW, 'st16'),
                            (ST8_OLD, ST8_NEW, 'st8')):
        if s.count(old) != 1:
            sys.exit('%s anchor matched %d times' % (label, s.count(old)))
        s = s.replace(old, new, 1)
    anchor = 'static inline uint32_t ld32(uint32_t a) {\n#ifdef NNK_GX'
    if s.count(anchor) != 1:
        sys.exit('prelude anchor matched %d times' % s.count(anchor))
    s = s.replace(anchor, SHADOW + anchor, 1)
    open(dst, 'w').write(s)
    print('spliced %s -> %s (%d bytes)' % (src, dst, len(s)))


if __name__ == '__main__':
    main()
