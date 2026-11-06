#!/usr/bin/env python3
"""Phase-5BZ: make a normal run quiet and let it run past the harness budget.

The canonical change lives in `nnktool/src/recompile.rs` (the prelude template),
which is what a regenerated image picks up.  This script applies the IDENTICAL
text transform to a frozen dispatch image, same precedent as
tools/p5bv_gx_matrix_readback_splice.py and the splices before it.

WHY (measured on the operator's build): a plain `./run.sh` printed, per event,

    irq enter step=... / irq return step=...
    io: EXMEMCNT=0x0000E080 pc=0x020064CC gba_decay=0xFE08

13 219 such lines in one 200M-step run, and then the run STOPPED:

    done steps=200000001 ... halt=step budget exhausted

because STEP_LIMIT (200M) is a harness guard, not a session length.  With a
window open the run must continue until the window is closed; NNK_STEP_LIMIT
still overrides both ways, and the headless default keeps the guard so scripts
cannot hang.

The per-event probes stay available: NNK_IRQ_TRACE=1 and NNK_IO_TRACE=1 put
them back (NNK_STORE_TRACE=1 covers the store probes in tools/nnk_smc.inc,
which is edited in the tree rather than spliced).
"""
import sys

MARK = "/* Phase-5BZ quiet-splice applied */"

HELPER_ANCHOR = "static uint64_t step_limit = STEP_LIMIT;"
HELPER_ADD = HELPER_ANCHOR + """

/* Phase-5BZ quiet-splice applied: per-event debug chatter is opt-in. */
static int nnk_io_trace_c = -1;
static int nnk_irq_trace_c = -1;
static int nnk_dbg_on(int *c, const char *name) {
    if (*c < 0) *c = (getenv(name) != NULL) ? 1 : 0;
    return *c;
}"""

EXMEM_OLD = """    case 0x04000204:
        exmemcnt = v;
        fprintf(stderr, "io: EXMEMCNT=0x%08X pc=0x%08X gba_decay=0x%04X\\n",
                v, last_pc, gba_openbus_tbl[(v >> 2) & 3u]);
        return;"""
EXMEM_NEW = """    case 0x04000204:
        exmemcnt = v;
        if (nnk_dbg_on(&nnk_io_trace_c, "NNK_IO_TRACE"))
            fprintf(stderr, "io: EXMEMCNT=0x%08X pc=0x%08X gba_decay=0x%04X\\n",
                    v, last_pc, gba_openbus_tbl[(v >> 2) & 3u]);
        return;"""

IRQE_OLD = """    fprintf(stderr, "irq enter step=%llu resume=0x%08X vector=0x%08X sp_irq=0x%08X spsr=0x%08X ime=%u ie=0x%08X if=0x%08X\\n",
            (unsigned long long)steps, resume, vec, r[13], spsr_bank[MODE_IRQ], ime, ie, iflags);"""
IRQE_NEW = """    if (nnk_dbg_on(&nnk_irq_trace_c, "NNK_IRQ_TRACE"))
        fprintf(stderr, "irq enter step=%llu resume=0x%08X vector=0x%08X sp_irq=0x%08X spsr=0x%08X ime=%u ie=0x%08X if=0x%08X\\n",
                (unsigned long long)steps, resume, vec, r[13], spsr_bank[MODE_IRQ], ime, ie, iflags);"""
IRQE_REJ_OLD = """        fprintf(stderr, "irq enter step=%llu: user vector 0x%08X rejected\\n",
                (unsigned long long)steps, vec);"""
IRQE_REJ_NEW = """        if (nnk_dbg_on(&nnk_irq_trace_c, "NNK_IRQ_TRACE"))
        fprintf(stderr, "irq enter step=%llu: user vector 0x%08X rejected\\n",
                (unsigned long long)steps, vec);"""

IRQR_OLD = """    fprintf(stderr, "irq return step=%llu pc=0x%08X cpsr_ctl=0x%08X mode=%d sp=0x%08X\\n",
            (unsigned long long)steps, *pc, cpsr_ctl, mode_cur, r[13]);"""
IRQR_NEW = """    if (nnk_dbg_on(&nnk_irq_trace_c, "NNK_IRQ_TRACE"))
        fprintf(stderr, "irq return step=%llu pc=0x%08X cpsr_ctl=0x%08X mode=%d sp=0x%08X\\n",
                (unsigned long long)steps, *pc, cpsr_ctl, mode_cur, r[13]);"""

LIMIT_OLD = """    { const char *s = getenv("NNK_STEP_LIMIT");
      if (s && s[0]) step_limit = strtoull(s, NULL, 0); }"""
LIMIT_NEW = """    { const char *s = getenv("NNK_STEP_LIMIT");
      if (s && s[0]) step_limit = strtoull(s, NULL, 0);
      else { const char *m = getenv("NNK_GX");
             /* an interactive window must not stop after STEP_LIMIT steps:
              * the default budget is a harness guard, not a session length */
             if (m && strcmp(m, "window") == 0) step_limit = ~0ull; } }"""


def main():
    if len(sys.argv) != 3:
        print("usage: p5bz_quiet_splice.py <in.c> <out.c>", file=sys.stderr)
        return 2
    src = open(sys.argv[1], encoding="utf-8").read()
    if MARK in src:
        print("phase-5bz: already applied, copying")
        open(sys.argv[2], "w", encoding="utf-8").write(src)
        return 0
    edits = [
        ("irq-enter probe", IRQE_OLD, IRQE_NEW),
        ("irq-enter reject probe", IRQE_REJ_OLD, IRQE_REJ_NEW),
        ("irq-return probe", IRQR_OLD, IRQR_NEW),
        ("EXMEMCNT probe", EXMEM_OLD, EXMEM_NEW),
        ("step budget", LIMIT_OLD, LIMIT_NEW),
    ]
    for name, old, new in edits:
        if old not in src:
            print("phase-5bz: anchor not found: %s" % name, file=sys.stderr)
            return 2
        src = src.replace(old, new, 1)
    if HELPER_ANCHOR not in src:
        print("phase-5bz: step_limit anchor not found", file=sys.stderr)
        return 2
    src = src.replace(HELPER_ANCHOR, HELPER_ADD, 1)
    open(sys.argv[2], "w", encoding="utf-8").write(src)
    print("phase-5bz: quiet-splice applied (%d probes gated, interactive budget unlimited)"
          % (len(edits) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
