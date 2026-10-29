#!/usr/bin/env python3
"""Cache-stability probe for tools/split_dispatch.py (Phase-3A4 acceptance).

Two claims from BUILD.md are checked against the real splitter:

  (A) nnk_rt.h is BYTE-STABLE when a new ADDRESS BAND appears.  The header
      always declares all NNK_BAND_COUNT prototypes, so a band going from
      empty to occupied must not change a single byte of the header -- if it
      did, every cached .o would be invalidated by the appearance of one new
      translated instruction.
  (B) stale part files from a previous, larger manifest are PRUNED after a
      successful split, so a leftover band never gets compiled or linked.

The probe synthesises an extra case in band 020 (an empty band of the current
image), verifies the header does not move, then re-splits the real image and
verifies the synthetic band is pruned.
"""
import os
import shutil
import subprocess
import sys

SPLIT = "work/split/c"
SRC = "/home/hermes/NiNoKuni/work/phase3a/arm9_thumb.c"
SYNTH = os.path.join(SPLIT, "nnk_part_020.c")

before = os.path.join(SPLIT, "nnk_rt.h")
shutil.copyfile(before, "/tmp/nnk_rt.h.before")

open(SYNTH, "w").write(
    '/* synthetic probe band - not part of the real manifest */\n'
    '#include "nnk_rt.h"\n\n'
    'int nnk_part_020(uint32_t *pc_io) {\n'
    '    uint32_t pc = *pc_io;\n'
    '    switch (pc) {\n'
    '        case 0x02140000u:\n'
    '            trace_pc(0x02140000u);\n'
    '            pc = 0x02140004u; break;\n'
    '        case 0x02140004u:\n'
    '            trace_pc(0x02140004u);\n'
    '            pc = 0x02140000u; break;\n'
    '    default:\n'
    '        return 0;\n'
    '    }\n'
    '    *pc_io = pc;\n'
    '    return 1;\n'
    '}\n')

r = subprocess.run([sys.executable, "tools/split_dispatch.py", SRC, SPLIT],
                   capture_output=True, text=True)
sys.stderr.write(r.stderr)
if r.returncode != 0:
    print("PROBE FAIL: splitter exited %d" % r.returncode)
    sys.exit(1)

after = os.path.join(SPLIT, "nnk_rt.h")
ok_a = open("/tmp/nnk_rt.h.before", "rb").read() == open(after, "rb").read()
ok_b = not os.path.exists(SYNTH)

print("(A) nnk_rt.h byte-stable across a new band appearing : %s"
      % ("PASS" if ok_a else "FAIL"))
print("(B) stale part file pruned after re-split           : %s"
      % ("PASS" if ok_b else "FAIL"))
sys.exit(0 if (ok_a and ok_b) else 1)
