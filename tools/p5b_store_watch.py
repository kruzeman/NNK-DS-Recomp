#!/usr/bin/env python3
"""Insert the Phase-5B guest-store watch into an ALREADY-GENERATED dispatch image.

    python3 tools/p5b_store_watch.py <in_image.c> <out_image.c>

Why a patcher and not a recompile: like `tools/p4v_fix_umull.py`, the frozen
Phase-3A image (`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`) carries the Thumb
dispatch, the overlay unions and the ITCM cases that the seed tables no longer
reproduce byte-for-byte.  Re-running `nnktool recompile-all` to change the
runtime prelude would silently change the guest code under test, so the
instrumentation is applied to the frozen image instead.  The same facility is
emitted by `nnktool/src/recompile.rs` for future regenerations, so the patcher
is a stop-gap for this image only (it refuses to run on an image that already
has the watch).

The watch logs every guest store whose address falls inside NNK_ST_WATCH
("lo:hi", hex, half-open) to NNK_ST_WATCH_PATH:

    st<width> addr=0x<ea> val=0x<value> pc=0x<dispatching pc> step=<n>

It logs BEFORE the write, and also logs stores that the memory model drops
(that distinction is the whole point of the instrument).
"""
import sys

WATCH = r'''/* ---- Phase-5B guest-store watch -------------------------------------------
 * NNK_ST_WATCH="lo:hi" (hex, half-open) + NNK_ST_WATCH_PATH + optional
 * NNK_ST_WATCH_MAX (default 20000) logs every guest store landing in the
 * window with the dispatching PC and the step index, BEFORE the write is
 * applied (so stores the memory model drops are still visible).  Pure
 * instrumentation: no store semantics change. */
static uint32_t stw_lo, stw_hi;
static int stw_on, stw_n;
static int stw_max = 20000;
static FILE *stw_fp;
static void st_watch_open(void) {
    const char *s = getenv("NNK_ST_WATCH");
    const char *p = getenv("NNK_ST_WATCH_PATH");
    const char *m = getenv("NNK_ST_WATCH_MAX");
    char *e;
    if (!s || !s[0] || !p || !p[0]) return;
    stw_lo = (uint32_t)strtoul(s, &e, 0);
    if (e == s || *e != ':') return;
    stw_hi = (uint32_t)strtoul(e + 1, NULL, 0);
    if (m && m[0]) stw_max = (int)strtol(m, NULL, 0);
    stw_fp = fopen(p, "w");
    stw_on = (stw_fp != NULL);
    fprintf(stderr, "st_watch: 0x%08X..0x%08X -> %s (max %d)\n",
            stw_lo, stw_hi, p, stw_max);
}
static void st_watch(uint32_t a, uint32_t v, unsigned n) {
    if (!stw_on) return;
    if (a < stw_lo || a >= stw_hi) return;
    if (stw_n >= stw_max) { stw_on = 0; fclose(stw_fp); stw_fp = NULL;
                            fprintf(stderr, "st_watch: max reached\n"); return; }
    stw_n++;
    fprintf(stw_fp, "st%u addr=0x%08X val=0x%08X pc=0x%08X step=%llu\n",
            n, a, v, last_pc, (unsigned long long)steps);
}
'''

ANCHOR_DECL = "static inline void smc_note(uint32_t a, uint32_t n);\n"
ANCHOR_MAIN = "    guest_init();\n"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    s = open(src).read()
    if 'NNK_ST_WATCH' in s:
        sys.exit('image already carries the store watch')
    assert s.count(ANCHOR_DECL) == 1, 'smc_note decl anchor: %d' % s.count(ANCHOR_DECL)
    s = s.replace(ANCHOR_DECL, ANCHOR_DECL + WATCH, 1)
    for w, tag in ((4, 'st32'), (2, 'st16'), (1, 'st8')):
        anchor = '    smc_note(a, %du);\n' % w
        assert s.count(anchor) == 1, '%s anchor: %d' % (tag, s.count(anchor))
        s = s.replace(anchor, anchor + '    st_watch(a, v, %du);\n' % w, 1)
    assert s.count(ANCHOR_MAIN) == 1, 'guest_init anchor: %d' % s.count(ANCHOR_MAIN)
    s = s.replace(ANCHOR_MAIN, ANCHOR_MAIN + "    st_watch_open();\n", 1)
    open(dst, 'w').write(s)
    print('wrote %s (store watch inserted)' % dst)


if __name__ == '__main__':
    main()
