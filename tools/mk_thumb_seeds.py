#!/usr/bin/env python3
"""Build the Phase-3A3 seed table: the Ghidra ARM9 function list plus the
Thumb entries the melonDS ground-truth trace proves execute in Thumb state.

  python3 tools/mk_thumb_seeds.py [out.tsv]     (default work/phase3a/seeds_thumb.tsv)

Thumb seeds and their evidence (melonDS ARM9 PC trace, 10M PCs):
  0x020004A8  `svc 11; bx lr`  - first Thumb callee after the Phase-3A2
              frontier (native halt at 0x0201C178); trace line
              "# swi thumb imm=0x00000B at 0x020004a8".
  0x02000648  `svc 3; bx lr`   - WaitByLoop thunk, 14478 PCs after the
              frontier; "# swi thumb imm=0x000003 at 0x02000648".
  0x02000E58 / 0x02000E74 / 0x02000E90
              genuine Thumb functions (bx lr tails) the CFG walk has
              recovered from ARM blx sites since Phase-2A; seeded
              explicitly so they survive a recovery that misses the site.

0x0200A56C is deliberately NOT seeded: the trace steps 0x0200A57C/0x580 in
4-aligned (ARM-state) increments and never 0x0200A56C..0x0200A57A, so there
is no evidence it ever executes as Thumb.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GHIDRA = os.path.join(HERE, "..", "work", "ghidra_arm9_functions.tsv")
OUT = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(HERE, "..", "work", "phase3a", "seeds_thumb.tsv")

THUMB = [0x020004A8, 0x02000648, 0x02000E58, 0x02000E74, 0x02000E90]

with open(GHIDRA) as fh:
    body = fh.read()
if not body.startswith("entry\t"):
    raise SystemExit("unexpected Ghidra TSV header: %s" % GHIDRA)
rows = [l for l in body.splitlines()[1:] if l.strip()]
have = {l.split("\t")[0].strip().lower().lstrip("0x") for l in rows}
added = 0
for a in THUMB:
    key = "%08X" % a
    if key.lower() in have:
        continue
    rows.append("%s\tFUN_thumb_%08X\t0\tfalse\tunknown\t2" % (key, a))
    added += 1
os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as fh:
    fh.write("entry\tname\tbody_bytes\tis_thunk\tcc\tinsn_len\n")
    fh.write("\n".join(rows) + "\n")
print("wrote %s: %d ghidra rows + %d thumb seeds (total %d)"
      % (OUT, len(rows) - added, added, len(rows)))
