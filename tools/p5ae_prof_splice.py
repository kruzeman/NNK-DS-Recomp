#!/usr/bin/env python3
"""Measurement-only TSC instrumentation for the split runtime (Phase-5AE).

Takes a split output dir (nnk_main.c from tools/split_dispatch.py) and writes an
instrumented copy that attributes wall time to the per-dispatch regions.  A mark
attributes the interval that ENDS at it, and every successfully executed guest
instruction leaves the dispatch block through `continue`, so the labels mean:

  PF_PRE     previous instruction's body (stores, card_tick) + rc tests + loop
             head (step check, arm7_evt_tick, irq_tick, WFI probe)
  PF_DMA     nnk_dma_tick (VBlank-IF producer, timer model, IPC, card stall,
             DMA channel loop) + nnk_input_tick
  PF_SMC     nnk_smc_try (dirty-block probe + live re-decode), plus the tiny
             p4x-band-log test it swallows
  PF_ENTRY   band-table lookup + the part call + the switch + trace_pc's entry
  PF_P5U     trace_pc's p5u_hook
  PF_DISP    trace_pc up to and including nnk_display_tick (vcount + frames)
  PF_DUMP    trace_pc's checkpoint/debug/itcm/dc/pred probe chain
  PF_BAND    the part-body remainder after trace_pc (only reached when the
             instruction did not `continue`; ~0 for compiled instructions)
  PF_ANY     nnk_smc_exec_any (decode-on-demand: ITCM / unrooted PCs)

usage: p5ae_prof_splice.py <split_src_dir> <instrumented_dir>
       NNK_GX=1 NNK_SPLIT_SKIP_REGEN=1 SPLIT_DIR=<instrumented_dir> \
           bash tools/build_split.sh <out>

NOT part of the shipped build: this exists to answer "where does the time go"
honestly with counters when perf is unavailable (perf_event_paranoid=4 here).
"""
import os, sys, shutil

src = sys.argv[1]
dst = sys.argv[2]

if os.path.isdir(dst):
    shutil.rmtree(dst)
shutil.copytree(src, dst)
p = os.path.join(dst, "nnk_main.c")
txt = open(p).read()

GLOBALS = r'''
/* ---- Phase-5AE measurement-only TSC regions ---------------------------- */
#include <x86intrin.h>
enum { PF_PRE, PF_DMA, PF_SMC, PF_BAND, PF_ANY, PF_P5U, PF_DISP, PF_DUMP, PF_ENTRY, PF_N };
static unsigned long long pf_acc[PF_N];
static unsigned long long pf_last;
static unsigned long long pf_cnt;
static inline unsigned long long pf_rd(void) { return __rdtsc(); }
static inline void pf_mark(int i) {
    unsigned long long now = pf_rd();
    if (pf_last == 0) { pf_last = now; return; }   /* first mark: start clock */
    pf_acc[i] += now - pf_last;
    pf_last = now;
}
static void pf_report(void) {
    static const char *nm[PF_N] = {"pre-loop","dma+input","smc-try","band-exec",
                                   "exec-any","p5u-hook","display-tick","dump-probe",
                                   "case-entry"};
    unsigned long long tot = 0;
    int i;
    for (i = 0; i < PF_N; i++) tot += pf_acc[i];
    fprintf(stderr, "prof: steps=%llu dispatches=%llu frames=%u tsc_total=%llu\n",
            (unsigned long long)steps, (unsigned long long)pf_cnt,
            (unsigned)gxd_frames, tot);
    for (i = 0; i < PF_N; i++)
        fprintf(stderr, "prof: %-12s %6.2f%%  %8.1f cyc/disp\n", nm[i],
                100.0 * (double)pf_acc[i] / (double)(tot ? tot : 1),
                (double)pf_acc[i] / (double)(pf_cnt ? pf_cnt : 1));
    fprintf(stderr, "prof: %.1f cyc/dispatch total (guest instr = 1 dispatch)\n",
            (double)tot / (double)(pf_cnt ? pf_cnt : 1));
}
'''
anchor = "void trace_pc(uint32_t pc) {\n"
assert txt.count(anchor) == 1
txt = txt.replace(anchor, GLOBALS + "\n" + anchor)

# ---- dispatch loop: mark PRE begin, DMA, SMC, BAND/ANY ---------------------
a = """            if (cycles >= nnk_tick_due) nnk_dma_tick();
            nnk_input_tick();
"""
assert txt.count(a) == 1
txt = txt.replace(a, """            pf_mark(PF_PRE);
            if (cycles >= nnk_tick_due) nnk_dma_tick();
            nnk_input_tick();
            pf_mark(PF_DMA);
            pf_cnt++;
""")

a = "            if (nnk_smc_try(&pc)) continue;\n"
assert txt.count(a) == 1
txt = txt.replace(a, """            pf_mark(PF_SMC);
            if (nnk_smc_try(&pc)) { pf_mark(PF_BAND); continue; }
            pf_mark(PF_SMC);        /* attrib: nnk_smc_try itself */
""")

a = """            if (nnk_smc_exec_any(&pc)) continue;
"""
assert txt.count(a) == 1
txt = txt.replace(a, """            pf_mark(PF_BAND);
            if (nnk_smc_exec_any(&pc)) { pf_mark(PF_ANY); continue; }
""")

# close the loop body: after the exec_any mark, before the for-loop's closing
a = """            halt_reason("dispatch to untranslated pc");
            return;
        }
    }
}
"""
assert txt.count(a) == 1
txt = txt.replace(a, """            halt_reason("dispatch to untranslated pc");
            return;
        }
        pf_cnt++;
    }
}
""")

# ---- trace_pc: p5u hook, display tick, probe chain ------------------------
a = """void trace_pc(uint32_t pc) {
    if (nnk_p5u_on) p5u_hook(pc);
"""
assert txt.count(a) == 1
txt = txt.replace(a, """void trace_pc(uint32_t pc) {
    pf_mark(PF_ENTRY);
    if (nnk_p5u_on) p5u_hook(pc);
    pf_mark(PF_P5U);
""")

a = "    if (nnk_display_tick()) { halt_reason(\"SDL window closed\"); step_limit = steps; }\n"
assert txt.count(a) == 1
txt = txt.replace(a, "    if (nnk_display_tick()) { halt_reason(\"SDL window closed\"); step_limit = steps; }\n    pf_mark(PF_DISP);\n")

a = """    pred_probe(pc);
"""
assert txt.count(a) == 1
txt = txt.replace(a, """    pred_probe(pc);
    pf_mark(PF_DUMP);
""")

# ---- report after run() ---------------------------------------------------
a = "    run();\n"
assert txt.count(a) == 1
txt = txt.replace(a, "    run();\n    pf_report();\n")

open(p, "w").write(txt)
print("instrumented %s -> %s" % (p, dst))
