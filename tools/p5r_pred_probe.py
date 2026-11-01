#!/usr/bin/env python3
"""Phase-5R: splice a *register + memory* probe into a frozen dispatch image.

The Phase-5Q hook (`tools/p5q_itcm_dump_at_pc.py`) proves *which* PC ran and
dumps whole arrays.  Barrier I6 needs one level finer: the operand values the
intra predictor actually consumed and produced, per block, so the DC seed can
be compared value-for-value instead of inferred from plane contents.

Spliced hook (fires in `trace_pc`, i.e. *before* the instruction executes):

    NNK_PRED_PC=<hex>[,<hex>...]  PCs to log (up to 4)
    NNK_PRED_N=<count>            max hits per PC (default 32)
    NNK_PRED_PATH=<file>          log file (default stderr)
    NNK_PRED_ROWS=<n>             words dumped at fp-0x100 and fp (default 8)

One line per hit holds the full register file the predictor works on
(r6/r7 = the DC words it is about to store, r11 = the block base fp), plus the
8 words of the row ABOVE the block (fp-0x100..) and of the block itself.
"""
import sys

MARK = 'p5r_pred_probe'
FUNC = r'''/* Phase-5R: operand probe for the intra predictor (barrier I6). */
static uint32_t pred_pc[4];
static int pred_npc;
static long pred_hits[4], pred_limit = 32, pred_rows = 8;
static FILE *pred_fp;
static void pred_probe(uint32_t pc) {
    int i;
    if (!pred_npc) return;
    for (i = 0; i < pred_npc; i++) {
        uint32_t fp;
        long k;
        if (pred_pc[i] != pc || pred_hits[i] >= pred_limit) continue;
        pred_hits[i]++;
        if (!pred_fp) {
            const char *p = getenv("NNK_PRED_PATH");
            pred_fp = (p && p[0]) ? fopen(p, "w") : stderr;
            if (!pred_fp) return;
            { const char *n = getenv("NNK_PRED_N");
              if (n && n[0]) pred_limit = strtol(n, NULL, 0);
              const char *rw = getenv("NNK_PRED_ROWS");
              if (rw && rw[0]) pred_rows = strtol(rw, NULL, 0); }
        }
        fp = r[11];
        fprintf(pred_fp, "PRED pc=%08X hit=%ld step=%llu fp=%08X lr=%08X sp=%08X"
                " r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X"
                " r8=%08X r12=%08X nzcv=%d%d%d%d\n",
                pc, pred_hits[i], (unsigned long long)steps, fp, r[14], r[13],
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[12],
                (int)n, (int)z, (int)c, (int)v);
        fprintf(pred_fp, "   above[%08X]:", fp - 0x100u);
        for (k = 0; k < pred_rows; k++) fprintf(pred_fp, " %08X", ld32(fp - 0x100u + 4u * (uint32_t)k));
        fprintf(pred_fp, "\n   block[%08X]:", fp);
        for (k = 0; k < pred_rows; k++) fprintf(pred_fp, " %08X", ld32(fp + 4u * (uint32_t)k));
        fprintf(pred_fp, "\n");
        fflush(pred_fp);
    }
}

'''

ANCHOR_FUNC = "static void trace_pc(uint32_t pc) {\n"
ANCHOR_CALL = "    itcm_dump_once(pc);\n"
ANCHOR_ENV = ('    { const char *s = getenv("NNK_HALT_AT_PC");\n'
              '      if (s && s[0]) halt_at_pc = (uint32_t)strtoul(s, NULL, 0); }\n')

ENV = '''    { const char *s = getenv("NNK_PRED_PC");
      if (s && s[0]) { const char *q = s;
        while (*q && pred_npc < 4) {
          while (*q == ' ' || *q == ',') q++;
          if (!*q) break;
          pred_pc[pred_npc++] = (uint32_t)strtoul(q, (char **)&q, 0); } } }
'''


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    src, dst = argv[1], argv[2]
    text = open(src).read()
    if MARK in text:
        print('%s: already carries the probe, copied through' % dst)
        if src != dst:
            open(dst, 'w').write(text)
        return 0
    for name, anchor in (('trace_pc', ANCHOR_FUNC), ('itcm_dump_once call', ANCHOR_CALL),
                         ('NNK_HALT_AT_PC env', ANCHOR_ENV)):
        n = text.count(anchor)
        if n != 1:
            print('anchor "%s" occurs %d times (want 1)' % (name, n))
            return 1
    text = text.replace('/* ' + MARK, '/* ' + MARK)  # no-op, keeps formatters quiet
    text = text.replace(ANCHOR_FUNC, FUNC + ANCHOR_FUNC, 1)
    text = text.replace(ANCHOR_CALL, ANCHOR_CALL + "    pred_probe(pc);\n", 1)
    text = text.replace(ANCHOR_ENV, ANCHOR_ENV + ENV, 1)
    text = text.replace('#include <string.h>', '/* ' + MARK + ' */\n#include <string.h>', 1)
    open(dst, 'w').write(text)
    print('%s -> %s: predictor operand probe spliced' % (src, dst))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
