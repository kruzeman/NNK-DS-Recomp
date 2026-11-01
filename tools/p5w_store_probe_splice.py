#!/usr/bin/env python3
"""Phase-5W: find the WRITER of the OSi_ThreadInfo corruption.

Phase-5V measured that the SDK thread block at 0x0206C684 is healthy at the
boot park (step 18,846,719) and destroyed at the movie park (2,256,356,590):
the current-thread slot reads 0 and a stale wait flag holds 0xE92D4000 (the
ARM encoding of `push {lr}`, a CODE word where a thread pointer belongs).
The narrow store watch 2.25e9..2.257e9 logged zero hits, so the writer is
earlier.  This probe widens the watch to the WHOLE run and adds three things
the narrow watch lacked:

1. the watch covers st8/st16/st32 (all guest stores and DMA go through them),
   so an 8/16-bit wipe cannot hide;
2. every hit is kept in a 512-entry RING BUFFER with PC/step/lr/r0-r3, so the
   64 stores that immediately precede a failure are available even when the
   full log's line budget is exhausted;
3. a HEALTH check runs on every hit and on a periodic heartbeat: it watches
   the six header words plus the current-thread slot and prints a DESTROYED /
   GARBAGE line the moment a pointer slot turns implausible (or a plausible
   pointer turns to 0), flushing the ring at that instant.

Also hooks the Phase-5T HLE plane copy (`p5t_plane`, a raw memcpy that does
NOT go through st32) and logs any copy whose destination range overlaps the
block, so the movie path cannot corrupt the block invisibly.

Env:
  NNK_P5W=1            enable
  NNK_P5W_FROM=<step>  only log stores at/after this step
  NNK_P5W_N=<lines>    full-log budget (default 200000)
  NNK_P5W_HEART=<mask> heartbeat period mask (default 0x3FFFFFF = 67.1 M)

usage: p5w_store_probe_splice.py <image.c> <out.c>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

if 'p5w_store' in s:
    sys.exit('already spliced: %s' % src)

BLOCK = r'''/* ---- Phase-5W: the writer of the OSi_ThreadInfo corruption --------------
 * OSi_ThreadInfo header at 0x0206C684.  Words measured at the healthy boot
 * park (Phase-5V): +0/+4 guards, +8 -> 0x0206C6A4 (the current-thread slot),
 * +0x20 the slot itself, +0x24/+0x28 the ready-queue head/tail.  Every store
 * into 0x0206C680..0x0206C6B0 during the whole run is logged (bounded),
 * ring-buffered and health-checked. */
#define P5W_LO 0x0206C680u
#define P5W_HI 0x0206C6B0u
#define P5W_RING 512
struct p5w_rec { uint64_t step; uint32_t pc, a, v, lr, r0, r1, r2, r3; unsigned n; };
static struct p5w_rec p5w_ring[P5W_RING];
static unsigned p5w_ri, p5w_rn;
static int p5w_st;                 /* 0 unparsed, 1 off, 2 on */
static uint64_t p5w_from, p5w_max = 200000, p5w_gap = 0x3FFFFFFu, p5w_ticks;
static long p5w_lines, p5w_evil, p5w_flips;
static uint32_t p5w_last[7];       /* +0 +4 +8 +20 +24 +28 and [ +8 ] */
static int p5w_have_last;

static void p5w_init(void) {
    const char *q;
    p5w_st = 1;
    q = getenv("NNK_P5W");
    if (!q || !q[0] || q[0] == '0') return;
    q = getenv("NNK_P5W_FROM");  if (q && q[0]) p5w_from = strtoull(q, NULL, 0);
    q = getenv("NNK_P5W_N");     if (q && q[0]) p5w_max = strtoull(q, NULL, 0);
    q = getenv("NNK_P5W_HEART"); if (q && q[0]) p5w_gap = strtoull(q, NULL, 0);
    p5w_st = 2;
    fprintf(stderr, "p5w: on from=%llu maxlines=%llu heart=%llu\n",
            (unsigned long long)p5w_from, (unsigned long long)p5w_max,
            (unsigned long long)p5w_gap);
}

/* A thread pointer must land in the SDK's thread .bss (0x02060000..0x02090000
 * at the boot park) or in main RAM at all; anything else is a code/stack word
 * where a pointer belongs.  Small integers (flags, counts) and 0 are legal
 * values for the header words and are not flagged on their own. */
static int p5w_susp(uint32_t v) {
    if (v == 0u) return 0;
    if (v <= 0xFFFFu) return 0;                     /* flag / count */
    if (v >= 0x02000000u && v < 0x03000000u) return 0;  /* main RAM pointer */
    return 1;
}
static void p5w_snap(uint32_t *h) {
    uint32_t G = 0x0206C684u, p8 = ld32(G + 8u);
    h[0] = ld32(G); h[1] = ld32(G + 4u); h[2] = p8;
    h[3] = ld32(G + 0x20u); h[4] = ld32(G + 0x24u); h[5] = ld32(G + 0x28u);
    h[6] = ((p8 >= 0x02000000u && p8 < 0x03000000u) ? ld32(p8) : 0xFFFFFFFFu);
}
static void p5w_dump_ring(const char *why) {
    unsigned k, i;
    p5w_flips++;
    fprintf(stderr, "p5w: === %s step=%llu; ring of last %u region stores ===\n",
            why, (unsigned long long)steps, p5w_rn);
    for (k = 0; k < p5w_rn && k < P5W_RING; k++) {
        i = (p5w_ri + P5W_RING - p5w_rn + k) % P5W_RING;
        fprintf(stderr, "p5w:   ring step=%llu pc=0x%08X [0x%08X]=0x%08X n=%u"
                        " lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
                (unsigned long long)p5w_ring[i].step, p5w_ring[i].pc,
                p5w_ring[i].a, p5w_ring[i].v, p5w_ring[i].n, p5w_ring[i].lr,
                p5w_ring[i].r0, p5w_ring[i].r1, p5w_ring[i].r2, p5w_ring[i].r3);
    }
    fflush(stderr);
}
/* Health check: a plausible pointer slot turning 0 or garbage is the
 * corruption signature; report the first few and flush the ring. */
static void p5w_health(const char *when) {
    uint32_t h[7];
    int k;
    p5w_snap(h);
    if (!p5w_have_last) { for (k = 0; k < 7; k++) p5w_last[k] = h[k];
                          p5w_have_last = 1; return; }
    for (k = 0; k < 7; k++) {
        uint32_t o = p5w_last[k], nv = h[k];
        if (o == nv) continue;
        if (p5w_susp(nv) || (p5w_susp(o) && nv == 0u)) {
            fprintf(stderr, "p5w: %s word[%d] 0x%08X -> 0x%08X step=%llu"
                            " (%s) hdr=%08X %08X %08X %08X %08X %08X cur=%08X\n",
                    when, k, o, nv, (unsigned long long)steps,
                    p5w_susp(nv) ? "garbage" : "zeroed",
                    h[0], h[1], h[2], h[3], h[4], h[5], h[6]);
            if (p5w_flips < 8) p5w_dump_ring(when);
            p5w_last[k] = nv;
        } else {
            p5w_last[k] = nv;
        }
    }
}
static void p5w_store(uint32_t a, uint32_t v, unsigned n) {
    if (p5w_st == 0) p5w_init();
    if (p5w_st != 2 || steps < p5w_from) return;
    p5w_ring[p5w_ri].step = steps; p5w_ring[p5w_ri].pc = last_pc;
    p5w_ring[p5w_ri].a = a; p5w_ring[p5w_ri].v = v; p5w_ring[p5w_ri].n = n;
    p5w_ring[p5w_ri].lr = r[14]; p5w_ring[p5w_ri].r0 = r[0];
    p5w_ring[p5w_ri].r1 = r[1]; p5w_ring[p5w_ri].r2 = r[2];
    p5w_ring[p5w_ri].r3 = r[3];
    p5w_ri = (p5w_ri + 1u) % P5W_RING;
    if (p5w_rn < P5W_RING) p5w_rn++;
    if (p5w_lines++ < (long)p5w_max)
        fprintf(stderr, "p5w: s step=%llu pc=0x%08X [0x%08X]=0x%08X n=%u"
                        " lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
                (unsigned long long)steps, last_pc, a, v, n,
                r[14], r[0], r[1], r[2], r[3]);
    if (p5w_susp(v) && p5w_evil++ < 200)
        fprintf(stderr, "p5w: SUSPICIOUS step=%llu pc=0x%08X [0x%08X]=0x%08X"
                        " n=%u lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X"
                        " r3=0x%08X r4=0x%08X r5=0x%08X sp=0x%08X\n",
                (unsigned long long)steps, last_pc, a, v, n, r[14], r[0], r[1],
                r[2], r[3], r[4], r[5], r[13]);
    p5w_health("store");
}
/* Heartbeat from trace_pc: the block's words over time, so the healthy ->
 * destroyed transition is visible even if the writer bypassed the watch.
 * Fine heartbeat (every 2e5 steps from 2.25e9) reads the block BOTH through
 * ld32 and through a raw gmem pointer, and prints the TCM configuration: if
 * the two disagree, the "corruption" is a READ-path aliasing bug (DTCM/ITCM
 * routing), not a stray store. */
static void p5w_tick(void) {
    uint32_t h[7];
    static uint64_t next;
    if (p5w_st == 0) p5w_init();
    if (p5w_st != 2) return;
    if ((steps & p5w_gap) == 0u) {
        p5w_snap(h);
        if (p5w_ticks++ < 4096)
            fprintf(stderr, "p5w: t step=%llu hdr=%08X %08X %08X %08X %08X %08X"
                            " cur=%08X\n", (unsigned long long)steps,
                    h[0], h[1], h[2], h[3], h[4], h[5], h[6]);
        p5w_health("tick");
    }
    if (next == 0) next = 2249800000ull;
    if (steps >= next) {
        const uint32_t *g = (const uint32_t *)main_ram_ptr(0x0206C680u);
        next += 200000ull;
        p5w_snap(h);
        fprintf(stderr, "p5w: fine step=%llu ld=%08X %08X %08X %08X %08X %08X %08X"
                        " gmem=%08X %08X %08X %08X %08X %08X %08X"
                        " itcm_size=%08X dtcm_base=%08X dtcm_mask=%08X pc=0x%08X\n",
                (unsigned long long)steps, ld32(0x0206C680u), h[0], h[1], h[2],
                h[3], h[4], h[5], h[6],
                g[0], g[1], g[2], g[3], g[4], g[5], g[6],
                itcm_size, dtcm_base, dtcm_mask, last_pc);
        fflush(stderr);
    }
}
/* Park dump: the SDK yield loop 0x020070B4 (Phase-5V's measurement point),
 * now with BOTH read paths so a read-path alias cannot masquerade as a
 * destroyed thread block. */
static void p5w_park(uint32_t pc) {
    static long n;
    const uint32_t *g = (const uint32_t *)main_ram_ptr(0x0206C680u);
    if (p5w_st == 0) p5w_init();
    if (p5w_st != 2 || pc != 0x020070B4u || n >= 3) return;
    n++;
    fprintf(stderr, "p5w: park#%ld step=%llu cpsr=%02X ld=%08X %08X %08X %08X %08X %08X"
                    " gmem=%08X %08X %08X %08X %08X %08X"
                    " itcm_size=%08X dtcm_base=%08X dtcm_mask=%08X pc=0x%08X\n",
            n, (unsigned long long)steps, cpsr_ctl,
            ld32(0x0206C684u), ld32(0x0206C688u), ld32(0x0206C68Cu),
            ld32(0x0206C6A4u), ld32(0x0206C6A8u), ld32(0x0206C6ACu),
            g[1], g[2], g[3], g[9], g[10], g[11],
            itcm_size, dtcm_base, dtcm_mask, last_pc);
    fflush(stderr);
}
/* Per-instruction STATE DIFF (the writer finder that cannot be bypassed).
 * trace_pc() runs before each instruction, so the snapshot taken here is the
 * block as the PREVIOUS instruction left it: the first snapshot that differs
 * from the last one names that previous instruction (prev_pc) as the writer,
 * whatever path it used (st8/16/32, DMA, a C memcpy in a probe/HLE helper, a
 * TCM alias effect).  Enabled from step 2.25e9 where the block is still
 * measured healthy.  Prints the 8 preceding PCs so a store inside a helper
 * can be read off the caller chain. */
static void p5w_diff(uint32_t pc) {
    static uint32_t prev[7], pcs[8];
    static unsigned pci;
    static int ok;
    uint32_t h[7];
    int k, bad = 0;
    if (p5w_st == 0) p5w_init();
    if (p5w_st != 2 || steps < 2250000000ull) return;
    p5w_snap(h);
    if (!ok) { for (k = 0; k < 7; k++) prev[k] = h[k]; ok = 1; }
    else {
        for (k = 0; k < 7; k++) if (h[k] != prev[k]) bad = 1;
        if (bad) {
            fprintf(stderr, "p5w: DIFF at step=%llu pc=0x%08X prev_pc=0x%08X"
                            " old=%08X %08X %08X %08X %08X %08X %08X"
                            " new=%08X %08X %08X %08X %08X %08X %08X"
                            " lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X"
                            " sp=0x%08X pcs=",
                    (unsigned long long)steps, pc, last_pc,
                    prev[0], prev[1], prev[2], prev[3], prev[4], prev[5], prev[6],
                    h[0], h[1], h[2], h[3], h[4], h[5], h[6],
                    r[14], r[0], r[1], r[2], r[3], r[13]);
            for (k = 7; k >= 0; k--) {
                unsigned j = (pci + 8u - (unsigned)k) % 8u;
                fprintf(stderr, " 0x%08X", pcs[j]);
            }
            fprintf(stderr, "\n");
            for (k = 0; k < 7; k++) prev[k] = h[k];
            fflush(stderr);
        }
    }
    pcs[pci] = pc; pci = (pci + 1u) % 8u;
}
'''

anchor = 'static inline void smc_note(uint32_t a, uint32_t n);\n'
if s.count(anchor) != 1:
    sys.exit('smc_note anchor not unique (%d)' % s.count(anchor))
s = s.replace(anchor, BLOCK + anchor, 1)

def hook(sig, body_first, call):
    global s
    if s.count(sig) != 1:
        sys.exit('anchor not unique: %r (%d)' % (sig, s.count(sig)))
    s = s.replace(sig, sig + call, 1)

hook('static inline void st32(uint32_t a, uint32_t v) {\n', None,
     '    if (a - P5W_LO < (P5W_HI - P5W_LO)) p5w_store(a, v, 4u);\n')
hook('static inline void st16(uint32_t a, uint16_t v) {\n', None,
     '    if (a - P5W_LO < (P5W_HI - P5W_LO)) p5w_store(a, (uint32_t)v, 2u);\n')
hook('static inline void st8(uint32_t a, uint32_t v) {\n', None,
     '    if (a - P5W_LO < (P5W_HI - P5W_LO)) p5w_store(a, (uint32_t)v, 1u);\n')

# heartbeat inside trace_pc (per instruction, one mask test)
t_anchor = 'static void trace_pc(uint32_t pc) {\n'
if s.count(t_anchor) != 1:
    sys.exit('trace_pc anchor not unique (%d)' % s.count(t_anchor))
body = t_anchor + '    p5u_hook(pc);\n    last_pc = pc;'
if body not in s:
    sys.exit('trace_pc body anchor not found')
s = s.replace(body, t_anchor + '    p5u_hook(pc);\n    p5w_diff(pc);\n    p5w_tick();\n    p5w_park(pc);\n    last_pc = pc;', 1)

# the HLE plane copy bypasses st32: log any copy overlapping the block
p_anchor = 'static int p5t_plane(uint32_t dst, const uint8_t *src, uint32_t n) {\n'
if s.count(p_anchor) == 1:
    s = s.replace(p_anchor, p_anchor + '''    /* Phase-5W: this path writes gmem directly; catch an overlap with the
     * SDK thread block instead of letting it corrupt invisibly. */
    if (dst - P5W_LO < (P5W_HI - P5W_LO) || (dst < P5W_LO && dst + n > P5W_LO))
        fprintf(stderr, "p5w: PLANE-CLASH dst=0x%08X n=%u step=%llu pc=0x%08X\\n",
                dst, n, (unsigned long long)steps, last_pc);
''', 1)
else:
    fprintf_note = 'plane anchor not found (movie HLE absent?)'

open(dst, 'w').write(s)
print('spliced %s -> %s' % (src, dst))
