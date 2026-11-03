#!/usr/bin/env python3
"""Phase-5BJ splice: correct the ARMv5TE DSP multiplies in a FROZEN image.

    python3 tools/p5bj_dsp_mul_splice.py <in.c> <out.c> [--image <arm9.bin>]

WHY
---
`work/phase3a/arm9_thumb_ovl_itcm_p5u.c` is a frozen `nnktool recompile-all`
artifact.  Before Phase-5BJ `nnktool/src/decode.rs` had no ARMv5TE DSP-multiply
rule (SMULxy / SMLAxy / SMULWx / SMLAWx).  Those words live inside the
data-processing encoding space and are distinguished only by bit 7, so the
frozen image decoded them as CMN/TST-style flag-only operations — i.e. as a
NO-OP on the destination register.

The measured consequence (Phase-5BJ): the guest's glyph blitter computes its
1-bpp source row stride with `smulbb r1, r1, r5` at 0x02030784 (1 byte/row x 11
rows).  The instruction left r1 = 1 instead of 11, so each glyph row was read
one bit further into the bitmap instead of eleven, and the "Skip the
cutscene?" text lost ~6/7 of its ink at shifted positions.

WHAT
----
The replacement bodies are NOT hand-written.  For each address of the committed
table below this script asks the fixed recompiler itself for that single case:

    nnktool recompile-cases <arm9.bin> <tmp.c> --base 0x02000000 \
            --entry <addr> --mode arm --max-insns 1

checks that the emitted mnemonic is the one the table names, and grafts that
case over the frozen image's case for the same address.  The table is the
result of scanning the decoded ARM9 image (`tools/p5bj_dsp_mul_splice.py
--scan <arm9.bin> <image.c>`, kept as a subcommand): every DSP-multiply word in
the image window that the frozen image has a case for — 21 of them, all
smulbb/smlabb.

Exit status is non-zero if any expected case is missing or emits a different
instruction, so a wrong table cannot silently ship.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MARKER = "/* Phase-5BJ dsp-multiply splice applied: %d cases (nnktool recompile-cases) */\n"
CASE_RE = re.compile(r"^        case (0x[0-9A-F]{8})u:(?: /\* nnk_state=(?:thumb|arm) \*/)?$")
PC_RE = re.compile(r"pc = (0x[0-9A-F]{8})u;")
DEFAULT_IMAGE = os.path.join(ROOT, "work", "arm9_dec.bin")
R = ['r0', 'r1', 'r2', 'r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10',
     'r11', 'r12', 'sp', 'lr', 'pc']

# (address, mnemonic, operands) — generated once from the decoded ARM9 image by
# `p5bj_dsp_mul_splice.py --scan`; the splice re-checks each row against the
# recompiler's own output before grafting it.
TABLE = [
    (0x02015764, "smulbb", "r6, r8, r4"),
    (0x0201576C, "smulbb", "r4, r5, r7"),
    (0x020305E0, "smulbb", "r0, r0, r1"),
    (0x02030784, "smulbb", "r1, r1, r5"),
    (0x020351B8, "smulbb", "r1, fp, r1"),
    (0x02036DD8, "smulbb", "r1, r2, r1"),
    (0x02039800, "smulbb", "r7, r7, r4"),
    (0x02050740, "smulbb", "r0, r2, r0"),
    (0x020509A4, "smulbb", "r0, r1, r0"),
    (0x020520F4, "smulbb", "r4, r7, r6"),
    (0x02052390, "smulbb", "r6, r4, r5"),
    (0x020569F0, "smlabb", "r2, r4, r3, r2"),
    (0x0205718C, "smulbb", "r1, r4, r1"),
    (0x02057A00, "smulbb", "r1, ip, r1"),
    (0x02058910, "smulbb", "r3, r1, r4"),
    (0x02058CB0, "smulbb", "r6, r6, r3"),
    (0x02058E28, "smulbb", "r4, r4, r1"),
    (0x02059180, "smulbb", "r0, r3, r0"),
    (0x02059780, "smulbb", "r0, r1, r0"),
    (0x020597B0, "smulbb", "r0, r1, r0"),
    (0x0205CB84, "smlabb", "r0, r1, r0, r2"),
]


def die(msg):
    sys.stderr.write("p5bj_dsp_mul_splice: " + msg + "\n")
    sys.exit(2)


def case_block(lines, addr):
    """[start, end) of the case block for addr, or None."""
    head = "        case 0x%08Xu:" % addr
    for i, ln in enumerate(lines):
        if ln.rstrip("\n") == head or ln.startswith(head + " "):
            for j in range(i + 1, min(i + 64, len(lines))):
                if PC_RE.search(lines[j]):
                    return i, j + 1
            die("case 0x%08X has no terminating pc= line" % addr)
    return None


def scan(image, frozen):
    import struct
    d = open(image, "rb").read()
    pat = open(frozen, "rb").read()
    base = 0x02000000
    rows = []
    for off in range(0, len(d) - 3, 4):
        w = struct.unpack_from("<I", d, off)[0]
        if (w & 0xF0000000) != 0xE0000000:
            continue
        low, loww = w & 0x0FF00090, w & 0x0FF000B0
        if low == 0x01600080:
            stem, xy = "smul", True
        elif low == 0x01000080:
            stem, xy = "smla", True
        elif loww == 0x012000A0:
            stem, xy = "smulw", False
        elif loww == 0x01200080:
            stem, xy = "smlaw", False
        else:
            continue
        a = base + off
        if ("case 0x%08Xu:" % a).encode() not in pat:
            continue
        name = stem
        if xy:
            name += "t" if w & 0x20 else "b"
        name += "t" if w & 0x40 else "b"
        rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
        ops = [R[rd], R[rm], R[rs]] + ([R[rn]] if stem in ("smla", "smlaw") else [])
        rows.append((a, name, ", ".join(ops)))
    return rows


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("in_c")
    ap.add_argument("out_c", nargs="?")
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--scan", action="store_true")
    args = ap.parse_args(argv)

    if args.scan:
        for a, n, o in scan(args.image, args.in_c):
            print('    (0x%08X, "%s", "%s"),' % (a, n, o))
        return 0
    if not args.out_c:
        die("need <out.c>")
    if not os.path.exists(args.image):
        die("guest image %s is missing (needed to re-emit the cases)" % args.image)

    nnk = os.path.join(ROOT, "nnktool", "target", "release", "nnktool")
    if not os.access(nnk, os.X_OK):
        die("nnktool is not built (%s); build_split.sh builds it for the Phase-5AT splice" % nnk)

    lines = open(args.in_c, encoding="utf-8").readlines()
    grafted = 0
    for addr, name, ops in sorted(TABLE):
        with tempfile.NamedTemporaryFile("r", suffix=".c", delete=False) as tf:
            tmp = tf.name
        try:
            r = subprocess.run(
                [nnk, "recompile-cases", args.image, tmp, "--base", "0x02000000",
                 "--entry", "0x%08X" % addr, "--mode", "arm", "--max-insns", "1"],
                capture_output=True, text=True)
            if r.returncode != 0:
                die("recompile-cases failed for 0x%08X: %s" % (addr, r.stderr.strip()))
            gen = open(tmp, encoding="utf-8").readlines()
        finally:
            os.unlink(tmp)
        blk = case_block(gen, addr)
        if blk is None:
            die("recompile-cases emitted no case for 0x%08X" % addr)
        body = gen[blk[0]:blk[1]]
        text = "".join(body)
        if ("%s %s" % (name, ops)) not in text:
            die("0x%08X: expected '%s %s', recompiler emitted %r"
                % (addr, name, ops, text.splitlines()[3] if len(text.splitlines()) > 3 else text))
        # keep the frozen image's dialect (no `nnk_state` marker on the label)
        body[0] = body[0].replace(" /* nnk_state=arm */", "")
        cur = case_block(lines, addr)
        if cur is None:
            die("frozen image has no case for 0x%08X" % addr)
        lines[cur[0]:cur[1]] = body
        grafted += 1

    with open(args.out_c, "w", encoding="utf-8") as fh:
        fh.write(MARKER % grafted)
        fh.writelines(lines)
    print("p5bj splice: %d DSP-multiply cases re-emitted -> %s" % (grafted, args.out_c))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
