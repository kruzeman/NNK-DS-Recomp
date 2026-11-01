#!/usr/bin/env python3
"""Phase-5AP helper: query the generated pc->form map.

    tools/pcform_query.py <map.h> 0xADDR [0xADDR ...]
    tools/pcform_query.py <map.h> --hole-report <pcs.txt>

Reads the generated C header back (it is the artifact the runtime uses, so a
query answers the same question the runtime asks: 1 Thumb, 0 ARM, -1 hole).
"""
import re
import sys

ROW = re.compile(r"\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u\s*\}")


def parse(path):
    text = open(path).read()
    tabs = {}
    for name in ("nnk_pcform_thumb", "nnk_pcform_arm"):
        i = text.index("static const uint32_t %s[" % name)
        j = text.index("};", i)
        rows = [(int(a, 16), int(b, 16)) for a, b in ROW.findall(text[i:j])]
        tabs[name] = rows
    return tabs["nnk_pcform_thumb"], tabs["nnk_pcform_arm"]


def lookup(tab, pc):
    lo, hi = 0, len(tab) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if pc < tab[mid][0]:
            hi = mid - 1
        elif pc >= tab[mid][1]:
            lo = mid + 1
        else:
            return True
    return False


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    thumb, arm = parse(sys.argv[1])
    args = sys.argv[2:]
    if args[0] == "--hole-report":
        pcs = []
        for line in open(args[1]):
            for tok in line.replace(",", " ").split():
                try:
                    pcs.append(int(tok, 16) if tok.lower().startswith("0x") else int(tok))
                except ValueError:
                    pass
        holes = [p for p in pcs if not lookup(thumb, p) and not lookup(arm, p)]
        print("holes %d / %d" % (len(holes), len(pcs)))
        for p in holes[:200]:
            print("  0x%08X" % p)
        return
    for a in args:
        pc = int(a, 16) if a.lower().startswith("0x") else int(a)
        if lookup(thumb, pc):
            f = "thumb"
        elif lookup(arm, pc):
            f = "arm"
        else:
            f = "HOLE"
        print("0x%08X %s" % (pc, f))


if __name__ == "__main__":
    main()
