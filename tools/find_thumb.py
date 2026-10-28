#!/usr/bin/env python3
"""Heuristically locate Thumb code regions in the ARM9 image.

For every Ghidra function entry, decode 12 instructions with
`nnktool disasm --mode thumb` and score the range by how few `und`
(undefined) slots it produces.  Prints the best candidates.
"""
import subprocess
import sys

NNK = "nnktool/target/release/nnktool"
IMG = "work/ghidra_src/arm9.bin"
TSV = "work/ghidra_arm9_functions.tsv"


def score(addr):
    r = subprocess.run(
        [NNK, "disasm", IMG, "--base", "0x02000000", "--start", hex(addr),
         "--count", "12", "--mode", "thumb"],
        capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    und = sum(1 for l in lines if l.rstrip().endswith("und") or "udf" in l)
    tail = lines[-1] if lines else ""
    return und, len(lines), tail, lines


def main():
    addrs = []
    with open(TSV) as fh:
        next(fh)
        for line in fh:
            a = int(line.split("\t")[0], 16)
            addrs.append(a)
    best = []
    for a in addrs[::7]:
        und, n, tail, lines = score(a)
        if n and und == 0:
            best.append((a, tail, lines))
    for a, tail, lines in best[:12]:
        print("0x%08X  tail=%s" % (a, tail.strip()))
        for l in lines[:6]:
            print("   ", l.strip())
    print("candidates: %d" % len(best))


if __name__ == "__main__":
    main()
