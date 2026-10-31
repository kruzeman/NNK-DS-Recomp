#!/usr/bin/env python3
"""Phase-4V: repair long-multiply (UMULL/SMULL/UMLAL/SMLAL) case bodies in a
GENERATED dispatch image C so they match the fixed nnktool decoder's output.

Defect (task t_38c55bf9): nnktool's decoder passed Kind::Mul { rd: rdhi,
rn: rdlo } while the codegen assigns `r[rd] = _lo; r[rn] = _hi;` — every
static-image long multiply had its RdLo/RdHi destinations SWAPPED (and the
UMLAL/SMLAL accumulate pair with them).  tools/nnk_smc.inc's runtime SMC
re-decoder always had the correct order.  Measured effect: the SDK
divide-by-10 loop at 0x0201E814 computed (low32(x*0xCCCCCCCD))>>3 instead of
(high32(x*0xCCCCCCCD))>>3, never converged, and its digit store loop ran from
sp+24=0x027E3888 up through the DTCM IRQ vector at 0x027E3FFC (step 25,214,182).

nnktool/src/decode.rs is fixed at source; this script rewrites the ALREADY
GENERATED image (its regen inputs are not all preserved) to the exact text the
fixed codegen emits:
    r[A] = _lo;  ->  r[B] = _lo;      (A was RdHi, B was RdLo)
    r[B] = _hi;  ->  r[A] = _hi;
    ((uint64_t)r[B] << 32) | r[A]  ->  ((uint64_t)r[A] << 32) | r[B]

Usage: p4v_fix_umull.py <image.c> [--apply]
"""
import re
import sys

MNEM = re.compile(r"/\* 0x[0-9A-Fa-f]{8}: (umull|smull|umlal|smlal)s? ")
LO_DST = re.compile(r"^(\s*)r\[(\d+)\] = _lo;$")
HI_DST = re.compile(r"^(\s*)r\[(\d+)\] = _hi;$")
ACCUM = re.compile(r"\(\(uint64_t\)r\[(\d+)\] << 32\) \| r\[(\d+)\]")


def fix_block(block: list[str]) -> tuple[list[str], int]:
    a = b = None
    for line in block:
        m = LO_DST.match(line)
        if m:
            a = m.group(2)
        m = HI_DST.match(line)
        if m:
            b = m.group(2)
    if a is None or b is None or a == b:
        return block, 0
    out = []
    n = 0
    for line in block:
        m = LO_DST.match(line)
        if m:
            out.append(f"{m.group(1)}r[{b}] = _lo;")
            n += 1
            continue
        m = HI_DST.match(line)
        if m:
            out.append(f"{m.group(1)}r[{a}] = _hi;")
            n += 1
            continue
        if ACCUM.search(line):
            out.append(ACCUM.sub(f"((uint64_t)r[{a}] << 32) | r[{b}]", line))
            n += 1
            continue
        out.append(line)
    return out, n


def main() -> int:
    path = sys.argv[1]
    apply_ = "--apply" in sys.argv[2:]
    lines = open(path, encoding="utf-8").read().split("\n")
    out: list[str] = []
    block: list[str] | None = None
    total = 0
    for line in lines:
        if block is not None:
            block.append(line)
            if line.strip() == "}":
                fixed, n = fix_block(block)
                out.extend(fixed)
                total += n
                block = None
            continue
        out.append(line)
        if MNEM.search(line):
            block = []
    print(f"p4v_fix_umull: {total} line(s) rewritten in {path}"
          f"{'' if apply_ else ' (dry run)'}")
    if apply_:
        open(path, "w", encoding="utf-8").write("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
