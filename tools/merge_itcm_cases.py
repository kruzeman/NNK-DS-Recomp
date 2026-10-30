#!/usr/bin/env python3
"""Phase-4B: splice the ITCM dispatch cases into the flat-window image.

    python3 tools/merge_itcm_cases.py <main.c> <itcm.c> <out.c> [trace]

<main.c> is the nnktool recompile-all output for the flat window
(base 0x02000000); <itcm.c> is the same emitter's output for the ITCM blob
(base 0x01FF8000).  Every `case 0x01FF8xxxu:` block from <itcm.c> is
inserted into <main.c>'s run() switch right after `switch (pc) {`, keeping
the file structure byte-identical to what tools/split_dispatch.py expects
(case labels first, then `default:`).

With [trace] (the extended-window melonDS capture), prints which observed
ITCM PCs got no case — those are honest known gaps that halt the native run.
"""
import re
import sys

CASE_RE = re.compile(r"^        case (0x[0-9A-F]+)u:$")
ITCM_LO, ITCM_HI = 0x01FF8000, 0x02000000


def load(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.readlines()


def structure(lines, name):
    run_start = next((i for i, l in enumerate(lines) if l == "static void run(void) {\n"), None)
    if run_start is None:
        sys.exit("%s: run() not found" % name)
    disp = next((i for i in range(run_start, len(lines)) if lines[i].strip() == "switch (pc) {"), None)
    if disp is None:
        sys.exit("%s: dispatch switch not found" % name)
    first_case = next((i for i in range(disp, len(lines)) if CASE_RE.match(lines[i])), None)
    if first_case is None:
        sys.exit("%s: first case label not found" % name)
    default = next((i for i in range(first_case, len(lines)) if lines[i] == "        default:\n"), None)
    if default is None:
        sys.exit("%s: default not found" % name)
    return run_start, disp, first_case, default


def case_blocks(lines, first_case, default):
    blocks = []
    i = first_case
    while i < default:
        m = CASE_RE.match(lines[i])
        if not m:
            sys.exit("unexpected line %d in case region: %r" % (i + 1, lines[i][:80]))
        j = i + 1
        while j < default and not CASE_RE.match(lines[j]):
            j += 1
        blocks.append((int(m.group(1), 16), i, j))
        i = j
    return blocks


if len(sys.argv) not in (4, 5):
    sys.exit(__doc__)

main_c, itcm_c, out_c = sys.argv[1:4]
trace_path = sys.argv[4] if len(sys.argv) == 5 else None

m = load(main_c)
_, m_disp, m_first, m_default = structure(m, "main")
m_blocks = case_blocks(m, m_first, m_default)
for addr, _, _ in m_blocks:
    if addr < 0x02000000:
        sys.exit("main image already contains a non-window case 0x%08X" % addr)

t = load(itcm_c)
_, t_disp, t_first, t_default = structure(t, "itcm")
t_blocks = case_blocks(t, t_first, t_default)
itcm_lines = []
itcm_addrs = set()
for addr, first, past in t_blocks:
    if not (ITCM_LO <= addr < ITCM_HI):
        sys.exit("itcm image has a non-ITCM case 0x%08X" % addr)
    if addr in itcm_addrs:
        continue
    itcm_addrs.add(addr)
    for k in range(first, past):
        itcm_lines.append(t[k])

merged = m[:m_first] + itcm_lines + m[m_first:]

# report: observed ITCM PCs without a case
if trace_path:
    observed = set()
    with open(trace_path) as fh:
        for line in fh:
            s = line.strip()
            if len(s) == 10 and s.startswith("0x"):
                v = int(s, 16)
                if ITCM_LO <= v < ITCM_HI:
                    observed.add(v)
    missing = sorted(observed - itcm_addrs)
    print("ITCM observed PCs : %d" % len(observed))
    print("ITCM cases spliced: %d (from %d itcm-image cases)" % (len(itcm_addrs), len(t_blocks)))
    print("observed but NO case: %d" % len(missing))
    for v in missing[:40]:
        print("  missing 0x%08X" % v)

with open(out_c, "w", encoding="utf-8") as fh:
    fh.writelines(merged)
print("wrote %s: %d ITCM case lines, %d main cases" %
      (out_c, len(itcm_lines), len(m_blocks)))
