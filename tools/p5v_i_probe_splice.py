#!/usr/bin/env python3
"""Phase-5V: splice the CPSR.I edge probe + the scheduler-park dump + the
OSi_ThreadInfo store watch into a frozen dispatch image.

Three injections, all env-gated and inert when unset:

1. `cpsr_set_ctl()` (every architectural CPSR-control write: compiled MSR,
   live-decoder MSR, exception entry/return) -> `p5v_i_edge()` logs every I
   transition with the writing instruction's PC.       NNK_P5V / _FROM / _N
2. `trace_pc()` -> `p5v_sched(pc)` dumps the SDK scheduler state
   (OSi_ThreadInfo at 0x0206C684: guards, current-thread chain, ready queue)
   at the yield loop 0x020070B4 and at OS_RescheduleThread 0x020067E0.
                                     NNK_P5V_SCHED / NNK_P5V_SCHED_FROM
3. `st32()` -> `p5v_watch()` logs every 32-bit store into the
   OSi_ThreadInfo block 0x0206C680..0x0206C6B0 with PC/step.
                                     NNK_P5V_WATCH / NNK_P5V_WATCH_FROM

usage: p5v_i_probe_splice.py <image.c> <out.c>
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

if 'p5v_i_edge' in s:
    sys.exit('already spliced: %s' % src)

# ---- 1. CPSR.I edge probe inside cpsr_set_ctl -----------------------------
anchor = '/* A write to CPSR control: switch banks when the mode changes. */\n'
if anchor not in s:
    sys.exit('cpsr_set_ctl banner not found')

probe = anchor + r'''/* Phase-5V: CPSR.I edge probe (NNK_P5V=1).  last_pc is defined below; the
 * tentative definition here makes the writer's PC visible to the probe. */
static uint32_t last_pc;
static int p5v_state;                       /* 0 unparsed, 1 off, 2 on */
static uint64_t p5v_from, p5v_n = 4096, p5v_lines;
static void p5v_i_edge(uint32_t oldc, uint32_t newc) {
    if (p5v_state == 0) {
        const char *q = getenv("NNK_P5V");
        p5v_state = (q && q[0] && q[0] != '0') ? 2 : 1;
        q = getenv("NNK_P5V_FROM");
        if (q && q[0]) p5v_from = strtoull(q, NULL, 0);
        q = getenv("NNK_P5V_N");
        if (q && q[0]) p5v_n = strtoull(q, NULL, 0);
        fprintf(stderr, "p5v: on from=%llu maxlines=%llu\n",
                (unsigned long long)p5v_from, (unsigned long long)p5v_n);
    }
    if (p5v_state != 2 || steps < p5v_from) return;
    if (++p5v_lines <= p5v_n)
        fprintf(stderr, "p5v: i %u->%u step=%llu pc=0x%08X ctl=%02X->%02X"
                        " mode=%d lr=0x%08X sp=0x%08X r0=0x%08X\n",
                (oldc >> 7) & 1u, (newc >> 7) & 1u,
                (unsigned long long)steps, last_pc, oldc, newc,
                mode_cur, r[14], r[13], r[0]);
}
'''
s = s.replace(anchor, probe, 1)

old = '    cpsr_ctl = ctl & 0xFFu;\n}'
if s.count(old) != 1:
    sys.exit('cpsr_set_ctl body anchor not unique (%d)' % s.count(old))
s = s.replace(old,
              '    {   /* Phase-5V: observe every I-bit transition with its PC */\n'
              '        uint32_t p5v_o = cpsr_ctl;\n'
              '        cpsr_ctl = ctl & 0xFFu;\n'
              '        if (((p5v_o ^ cpsr_ctl) & 0x80u) != 0u)\n'
              '            p5v_i_edge(p5v_o, cpsr_ctl);\n'
              '    }\n}', 1)

# ---- 3. store watch on the OSi_ThreadInfo block (before st32) -------------
w_anchor = 'static inline void st32(uint32_t a, uint32_t v) {\n'
if s.count(w_anchor) != 1:
    sys.exit('st32 anchor not unique (%d)' % s.count(w_anchor))
watch = w_anchor + r'''    /* Phase-5V: NNK_P5V_WATCH=1 -> log stores into OSi_ThreadInfo. */
    if (a >= 0x0206C680u && a < 0x0206C6B0u) p5v_watch(a, v, 4u);
'''
s = s.replace(w_anchor, watch, 1)

# p5v_watch itself: define it right before st32 (ld32/steps/last_pc exist)
wd = r'''/* Phase-5V: OSi_ThreadInfo store watch (NNK_P5V_WATCH=1).  Logs every
 * write to the block HEADER (0x0206C684..0x0206C69F) and only IMPLAUSIBLE
 * writes to the current-thread slot 0x0206C6A4 (a value outside the thread
 * .bss range 0x02060000..0x02090000), so normal scheduler churn cannot
 * consume the line budget. */
static void p5v_watch(uint32_t a, uint32_t v, unsigned n) {
    static int st; static uint64_t wf; static long cnt;
    if (st == 0) {
        const char *q = getenv("NNK_P5V_WATCH");
        st = (q && q[0] && q[0] != '0') ? 2 : 1;
        q = getenv("NNK_P5V_WATCH_FROM");
        if (q && q[0]) wf = strtoull(q, NULL, 0);
    }
    if (st != 2 || steps < wf) return;
    if (!(a >= 0x0206C684u && a < 0x0206C6A0u)) {
        if (a != 0x0206C6A4u) return;
        if (v >= 0x02060000u && v < 0x02090000u) return;
    }
    if (cnt++ >= 400) return;
    fprintf(stderr, "p5v: w [0x%08X]=0x%08X n=%u step=%llu pc=0x%08X mode=%d"
                    " lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
            a, v, n, (unsigned long long)steps, last_pc, mode_cur, r[14], r[0], r[1], r[2], r[3]);
}
'''
s = s.replace(w_anchor, wd + w_anchor, 1)

# ---- 2. scheduler state dump, hooked from trace_pc -----------------------
t_anchor = 'static void trace_pc(uint32_t pc) {'
if s.count(t_anchor) != 1:
    sys.exit('trace_pc anchor not unique (%d)' % s.count(t_anchor))

sched = r'''/* Phase-5V: SDK scheduler park dump (NNK_P5V_SCHED=1). */
static int p5v_sch_state; static uint64_t p5v_sch_from;
static long p5v_sch_n, p5v_sch_e;
static void p5v_sched(uint32_t pc) {
    if (p5v_sch_state == 0) {
        const char *q = getenv("NNK_P5V_SCHED");
        p5v_sch_state = (q && q[0] && q[0] != '0') ? 2 : 1;
        q = getenv("NNK_P5V_SCHED_FROM");
        if (q && q[0]) p5v_sch_from = strtoull(q, NULL, 0);
    }
    if (p5v_sch_state != 2 || steps < p5v_sch_from) return;
    if (pc == 0x020070B4u && p5v_sch_n < 3) {
        uint32_t G = 0x0206C684u, p8 = ld32(G + 8u), cur = p8 ? ld32(p8) : 0u;
        uint32_t nd = ld32(G + 0x24u);
        int i;
        p5v_sch_n++;
        fprintf(stderr, "p5v: park#%ld step=%llu cpsr=%02X ime=%u ie=%08X if=%08X"
                        " G0=%08X G4=%08X G8=%08X G20=%08X G24=%08X G28=%08X"
                        " p8=0x%08X cur=0x%08X\n",
                p5v_sch_n, (unsigned long long)steps, cpsr_ctl, ime, ie, iflags,
                ld32(G), ld32(G + 4u), ld32(G + 8u), ld32(G + 0x20u),
                ld32(G + 0x24u), ld32(G + 0x28u), p8, cur);
        if (cur)
            fprintf(stderr, "p5v:   cur st=%u +120=%08X +176=%08X +104=%08X\n",
                    ld32(cur + 100u), ld32(cur + 120u), ld32(cur + 176u),
                    ld32(cur + 104u));
        for (i = 0; i < 8 && nd; i++) {
            fprintf(stderr, "p5v:   ready[%d]=0x%08X st=%u next=0x%08X +120=%08X\n",
                    i, nd, ld32(nd + 100u), ld32(nd + 104u), ld32(nd + 120u));
            nd = ld32(nd + 104u);
        }
        fprintf(stderr, "p5v:   r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X"
                        " r6=%08X r7=%08X lr=%08X sp=%08X [sp+4]=%08X\n",
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[14], r[13],
                ld32(r[13] + 4u));
        fflush(stderr);
    }
    if (pc == 0x020067E0u && p5v_sch_e < 6) {
        uint32_t G = 0x0206C684u, p8 = ld32(G + 8u), cur = p8 ? ld32(p8) : 0u;
        p5v_sch_e++;
        fprintf(stderr, "p5v: resched#%ld step=%llu cpsr=%02X G4=%08X G8=%08X"
                        " cur=0x%08X curst=%u\n",
                p5v_sch_e, (unsigned long long)steps, cpsr_ctl, ld32(G + 4u),
                ld32(G + 8u), cur, cur ? ld32(cur + 100u) : 0xFFFFFFFFu);
    }
}
'''
s = s.replace(t_anchor, sched + t_anchor, 1)

body = t_anchor + '\n    last_pc = pc;'
if body in s:
    s = s.replace(body, t_anchor + '\n    p5v_sched(pc);\n    last_pc = pc;', 1)
else:
    # the Phase-5U image already calls p5u_hook() first
    b2 = t_anchor + '\n    p5u_hook(pc);\n    last_pc = pc;'
    if b2 not in s:
        sys.exit('trace_pc body anchor not found')
    s = s.replace(b2, t_anchor + '\n    p5u_hook(pc);\n    p5v_sched(pc);\n    last_pc = pc;', 1)

open(dst, 'w').write(s)
print('spliced %s -> %s' % (src, dst))
