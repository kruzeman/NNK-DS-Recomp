#!/usr/bin/env python3
"""Cross-check nnktool's recovered functions against Ghidra's function table.

Inputs:
  work/phase1a/arm9_functions.tsv     (nnktool: entry, mode, insns, ...)
  work/ghidra_arm9_functions.tsv       (Ghidra: entry, name, body, thunk, cc, insn_len)

Reports agreement/disagreement counts honestly:
  * agreed      : a Ghidra function starts at exactly the same address
  * nnk-only    : nnktool entry with no Ghidra function at that address
                  (sub-classified: inside a Ghidra function body / in data)
  * ghidra-only : Ghidra functions inside the address span nnktool covered
  * mode        : ARM vs Thumb split on both sides (Ghidra Thumb = insn_len 2)
"""
import sys

NNK = sys.argv[1] if len(sys.argv) > 1 else "work/phase1a/arm9_functions.tsv"
GH = sys.argv[2] if len(sys.argv) > 2 else "work/ghidra_arm9_functions.tsv"

nnk = {}
with open(NNK) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        nnk[int(p[0], 16)] = dict(mode=p[1], insns=int(p[2]), bytes=int(p[3]),
                                  blocks=int(p[4]), calls=int(p[5]),
                                  indirects=int(p[6]), undef=int(p[7]))

gh = {}
spans = []
with open(GH) as fh:
    next(fh)
    for line in fh:
        p = line.rstrip("\n").split("\t")
        a = int(p[0], 16)
        gh[a] = dict(name=p[1], body=int(p[2]), thunk=p[3] == "true",
                     cc=p[4], insn_len=int(p[5]))
        spans.append((a, a + max(int(p[2]), 1)))


def span_of(addr):
    for s, e in spans:
        if s <= addr < e:
            return (s, e)
    return None


agreed = []
nnk_only = []
for a, info in sorted(nnk.items()):
    if a in gh:
        agreed.append((a, info, gh[a]))
    else:
        nnk_only.append((a, info, span_of(a)))

lo = min(nnk) if nnk else 0
hi = max(nnk) if nnk else 0
ghidra_in_span = [a for a in gh if lo <= a <= hi]

nnk_arm = sum(1 for i in nnk.values() if i["mode"] == "arm")
ghidra_arm = sum(1 for a, g in gh.items() if g["insn_len"] != 2)
ghidra_thumb = sum(1 for a, g in gh.items() if g["insn_len"] == 2)

print("nnktool recovered functions : %d (%d arm, %d thumb)" % (len(nnk), nnk_arm, len(nnk) - nnk_arm))
print("Ghidra functions (whole image): %d (%d arm-sized entry insn, %d thumb-sized entry insn)"
      % (len(gh), ghidra_arm, ghidra_thumb))
print("nnktool address span        : 0x%08X..0x%08X" % (lo, hi))
print("Ghidra functions in span    : %d" % len(ghidra_in_span))
print()
print("agreed (same entry address) : %d / %d recovered (%.1f%%)"
      % (len(agreed), len(nnk), 100.0 * len(agreed) / max(1, len(nnk))))
mode_ok = sum(1 for a, i, g in agreed if (g["insn_len"] == 2) == (i["mode"] == "thumb"))
print("  of which mode agrees      : %d" % mode_ok)
print("nnktool-only entries        : %d" % len(nnk_only))
inside = sum(1 for a, i, s in nnk_only if s)
print("  inside a Ghidra body      : %d" % inside)
for a, i, s in nnk_only[:12]:
    print("    0x%08X %-5s insns=%-4d %s" % (a, i["mode"], i["insns"],
                                            ("inside Ghidra body 0x%08X..0x%08X" % s) if s else "no Ghidra body"))
print("Ghidra-only in span         : %d (nnktool follows direct calls only; indirect/jump-table targets are not expanded)"
      % (len(ghidra_in_span) - len([a for a in ghidra_in_span if a in nnk])))
