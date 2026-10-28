#!/usr/bin/env python3
"""Generate the recovered-function tables for CODE_MAP.md from the TSVs."""
nnk = "work/phase1a/arm9_functions.tsv"
thumb = "work/phase1a/thumb/arm9_functions.tsv"
out = "work/phase1a/code_map_tables.md"

rows = []
with open(nnk) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        rows.append(p)

lines = ["| entry | mode | insns | bytes | blocks | direct calls | unresolved indirects |",
         "|---|---|---|---|---|---|---|"]
for p in rows:
    lines.append("| `%s` | %s | %s | %s | %s | %s | %s |" % (p[0], p[1], p[2], p[3], p[4], p[5], p[6]))
tot_i = sum(int(p[2]) for p in rows)
tot_b = sum(int(p[3]) for p in rows)
tot_bl = sum(int(p[4]) for p in rows)
tot_c = sum(int(p[5]) for p in rows)
tot_ind = sum(int(p[6]) for p in rows)
lines.append("")
lines.append("TOTAL: %d functions, %d instructions, %d bytes, %d basic blocks, %d direct call sites, %d unresolved indirect sites"
             % (len(rows), tot_i, tot_b, tot_bl, tot_c, tot_ind))
lines.append("")

trows = []
with open(thumb) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        trows.append(p)
lines.append("Second recovery rooted at the Thumb entry `0x02060C00` (`--entry-mode thumb`):")
lines.append("")
lines.append("| entry | mode | insns | bytes | blocks | direct calls | unresolved indirects |")
lines.append("|---|---|---|---|---|---|---|")
for p in trows:
    lines.append("| `%s` | %s | %s | %s | %s | %s | %s |" % (p[0], p[1], p[2], p[3], p[4], p[5], p[6]))

with open(out, "w") as fh:
    fh.write("\n".join(lines) + "\n")
print("wrote %s (%d + %d rows)" % (out, len(rows), len(trows)))
