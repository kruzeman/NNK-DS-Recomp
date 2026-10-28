#!/usr/bin/env python3
"""Compare `nnktool disasm` output against arm-none-eabi-objdump.

Normalisation applied to BOTH sides before comparison:
  * drop everything from the first '<' (symbol) onwards
  * drop '@' and ';' comments
  * lowercase
  * collapse whitespace runs to one space
  * render every numeric literal (0x-prefixed or decimal) as 0x%x

Usage:
  cmp_objdump.py NNK.txt OBJDUMP.txt [--report MISMATCHES.txt]
Exit code 0 when every sampled instruction matches (address + word + text).
"""
import argparse
import re
import sys

LINE = re.compile(r"^\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{4,8})\s*(.*?)\s*$")
NUM = re.compile(r"(0x[0-9a-fA-F]+|\b\d+\b)")


def norm(text: str) -> str:
    text = text.split("<", 1)[0]
    text = text.split("@", 1)[0]
    text = text.split(";", 1)[0]
    text = text.lower()
    text = NUM.sub(lambda m: "0x%x" % (int(m.group(0), 16) if m.group(0).lower().startswith("0x") else int(m.group(0))), text)
    return " ".join(text.split())


def parse(path):
    out = {}
    with open(path) as fh:
        for line in fh:
            m = LINE.match(line.rstrip("\n"))
            if not m:
                continue
            addr = int(m.group(1), 16)
            word = m.group(2).lower()
            out[addr] = (word, norm(m.group(3)))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("nnk")
    ap.add_argument("objdump")
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    mine = parse(args.nnk)
    theirs = parse(args.objdump)
    if not mine:
        print("no instructions parsed from %s" % args.nnk)
        return 2
    if not theirs:
        print("no instructions parsed from %s" % args.objdump)
        return 2

    addrs = sorted(set(mine) & set(theirs))
    only_mine = sorted(set(mine) - set(theirs))
    only_obj = sorted(set(theirs) - set(mine))
    word_ok = text_ok = both_ok = 0
    bad = []
    for a in addrs:
        mw, mt = mine[a]
        ow, ot = theirs[a]
        wmatch = mw == ow
        tmatch = mt == ot
        word_ok += wmatch
        text_ok += tmatch
        both_ok += wmatch and tmatch
        if not (wmatch and tmatch):
            bad.append((a, mw, ow, mt, ot, wmatch, tmatch))

    print("compared addresses : %d" % len(addrs))
    print("encoding match     : %d/%d" % (word_ok, len(addrs)))
    print("text match         : %d/%d" % (text_ok, len(addrs)))
    print("both match         : %d/%d" % (both_ok, len(addrs)))
    if only_mine:
        print("addresses only in nnktool : %d (%s...)" % (len(only_mine), " ".join("%x" % a for a in only_mine[:5])))
    if only_obj:
        print("addresses only in objdump : %d (%s...)" % (len(only_obj), " ".join("%x" % a for a in only_obj[:5])))
    if bad:
        print("\nmismatches (up to 40):")
        for a, mw, ow, mt, ot, wm, tm in bad[:40]:
            print("  %08x  word %s %s | text %s" % (a, "ok " if wm else "BAD", "ok " if tm else "BAD", ""))
            if not wm:
                print("      nnk    : %s" % mw)
                print("      objdump: %s" % ow)
            if not tm:
                print("      nnk    : %s" % mt)
                print("      objdump: %s" % ot)
    if args.report:
        with open(args.report, "w") as fh:
            for a, mw, ow, mt, ot, wm, tm in bad:
                fh.write("%08X\tword:%s\ttext:%s\n" % (a, "ok" if wm else "bad", "ok" if tm else "bad"))
                if not wm:
                    fh.write("  nnk    : %s\n  objdump: %s\n" % (mw, ow))
                if not tm:
                    fh.write("  nnk    : %s\n  objdump: %s\n" % (mt, ot))
        print("\nwrote %s" % args.report)
    return 0 if (both_ok == len(addrs) and not only_mine and not only_obj) else 1


if __name__ == "__main__":
    sys.exit(main())
