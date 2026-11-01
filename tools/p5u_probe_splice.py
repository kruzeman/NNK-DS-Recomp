#!/usr/bin/env python3
"""Phase-5U: splice the `p5u_hook` call into a frozen dispatch image's
`trace_pc()` so the probe sees EVERY dispatched instruction.

Why a splice instead of the generator: the runtime prelude of a frozen image
(`work/phase3a/arm9_thumb_ovl_itcm_p5*.c`) is what the acceptance runs are
built from, and the Phase-5R/5Q probes were spliced the same way
(`tools/p5q_itcm_dump_at_pc.py`).  `trace_pc()` is the one function both
dispatch paths call — the compiled `nnk_part_*` cases and the on-demand
`nnk_smc_exec_any` re-decoder — so hooking it gives a histogram over the whole
program, not just the live-decoded half.

The hook body itself lives in `tools/nnk_smc.inc` (`p5u_hook`, NNK_P5U=1,
off by default); only the call site and its forward declaration are spliced.

usage: p5u_probe_splice.py <image.c> <out.c>
"""
import sys, re

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

if 'p5u_hook' in s:
    sys.exit('already spliced: %s' % src)

# 1. forward declaration, right before trace_pc's definition
decl = 'static void p5u_hook(uint32_t pc); /* p5u probe */\n'
anchor = 'static void trace_pc(uint32_t pc) {'
if anchor not in s:
    sys.exit('trace_pc not found')
s = s.replace(anchor, decl + anchor, 1)

# 2. the call: first statement of trace_pc (before last_pc/cycles++), so the
#    step index the probe prints is the one this instruction will consume.
body_anchor = anchor + '\n    last_pc = pc;'
if body_anchor not in s:
    sys.exit('trace_pc body not found')
s = s.replace(body_anchor,
              anchor + '\n    p5u_hook(pc);\n    last_pc = pc;', 1)

open(dst, 'w').write(s)
print('spliced %s -> %s' % (src, dst))
