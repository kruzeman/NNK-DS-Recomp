#!/usr/bin/env python3
"""Honest coverage of Ghidra's analysis by nnktool's recovered set.

  python3 tools/coverage_ghidra.py <nnk_decompile.txt> <ghidra_functions.tsv>

`nnk_decompile.txt` is nnktool's listing (one "  <addr>: <enc> <text>" line per
recovered instruction); the Ghidra TSV gives every Ghidra function entry and
body size.  For each Ghidra function we count how many word-aligned addresses
of its body nnktool recovered as instructions, then report:

  * functions whose body is fully covered / >=50% covered,
  * the mean body coverage,
  * byte-weighted coverage of all Ghidra bodies (the honest "explains
    Ghidra's N functions" number — it does not depend on the seed list).
"""
import re
import sys

listing_path = sys.argv[1] if len(sys.argv) > 1 else "work/phase2a/arm9_decompile.txt"
tsv_path = sys.argv[2] if len(sys.argv) > 2 else "work/ghidra_arm9_functions.tsv"

insn_re = re.compile(r"^  ([0-9a-f]{8}):\s")

recovered = set()
with open(listing_path) as fh:
    for line in fh:
        m = insn_re.match(line)
        if m:
            recovered.add(int(m.group(1), 16))

fns = []
with open(tsv_path) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        entry = int(p[0], 16)
        body = max(int(p[2]), 4)
        fns.append((entry, body, p[1]))

full = half = some = 0
covered_bytes = 0
total_bytes = 0
per_fn = []
for entry, body, name in fns:
    words = (body + 3) // 4
    hit = 0
    for i in range(words):
        if entry + 4 * i in recovered:
            hit += 1
    frac = hit / words if words else 0.0
    per_fn.append(frac)
    total_bytes += words * 4
    covered_bytes += hit * 4
    if hit == words:
        full += 1
    if frac >= 0.5:
        half += 1
    if hit > 0:
        some += 1

n = len(fns)
print("recovered instructions in the listing : %d distinct addresses" % len(recovered))
print("Ghidra functions                      : %d" % n)
print("  body fully covered                 : %d (%.1f%%)" % (full, 100.0 * full / n))
print("  body >= 50%% covered                : %d (%.1f%%)" % (half, 100.0 * half / n))
print("  body >= 1 instruction covered      : %d (%.1f%%)" % (some, 100.0 * some / n))
print("  mean body coverage                 : %.1f%%" % (100.0 * sum(per_fn) / n))
print("byte-weighted body coverage          : %d / %d bytes (%.1f%%)" %
      (covered_bytes, total_bytes, 100.0 * covered_bytes / max(1, total_bytes)))
