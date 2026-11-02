#!/usr/bin/env python3
"""Phase-5AT splice: correct a wrong-mode function body in a FROZEN image.

    python3 tools/p5at_mode_splice.py <in.c> <out.c> [options]

WHY
---
`work/phase3a/arm9_thumb_ovl_itcm_p5u.c` is a frozen `nnktool recompile-all`
artifact: the guest function bodies in it were emitted at some earlier phase
from the seed table of that phase.  One row of that table is wrong
(`0x02062C44`: Ghidra reported insn_len = -1, so it defaulted to ARM) and the
ARM reading of the Thumb bytes is `cdpne` + a trap at 0x02062C48
(`undefined instruction 0xF94AF000`).  Regenerating the whole image is not a
free operation -- it re-derives every function and would drop the frozen
artifact every other phase measured against -- so the correction is spliced,
exactly like the Phase-5X/5AE/5AF/5AJ/5AP transforms.

WHAT
----
The replacement body is NOT hand-written.  For every row of the committed
override file `tools/nnk_mode_overrides.txt` this script asks the recompiler
itself for the function's dispatch cases:

    nnktool recompile-cases <image> <tmp.c> --base <base> --entry <row addr>
            --mode <row mode> --mode-overrides <override file>

and then

  1. drops every case of the frozen image whose label lies inside the
     generated body's `[first, past_end)` range (the wrong-mode cases plus any
     16-bit address the ARM walk could not produce),
  2. inserts the generated cases at that position, in ascending order,
  3. writes the marker the build's prelude audit looks for,
  4. reports the direct-call targets of the new body that the effective image
     does NOT translate -- i.e. the frontier the fix hands to the next phase.

The override file is the single source of truth for both paths: a future
`recompile-all --mode-overrides tools/nnk_mode_overrides.txt` therefore emits
the same body this splice inserts.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MARKER = "/* Phase-5AT mode-override splice applied: %s (tools/nnk_mode_overrides.txt) */\n"
CASE_RE = re.compile(r"^        case (0x[0-9A-F]{8})u:(?: /\* nnk_state=(thumb|arm) \*/)?$")
PC_RE = re.compile(r"pc = (0x[0-9A-F]{8})u;")
DEFAULT_IMAGE = os.path.join(ROOT, "work", "arm9_dec.bin")


def die(msg):
    sys.stderr.write("p5at_mode_splice: " + msg + "\n")
    sys.exit(2)


def load_overrides(path):
    """<address> <arm|thumb> <reason> -- the same grammar as nnktool/modeov.rs."""
    rows = []
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split(None, 2)
            if len(parts) != 3:
                die("%s:%d: expected '<address> <arm|thumb> <reason>'" % (path, n))
            addr_s, mode, reason = parts
            addr = int(addr_s, 16)
            if addr & 1:
                die("%s:%d: 0x%08X has bit0 set (name the entry)" % (path, n, addr))
            if mode not in ("arm", "thumb"):
                die("%s:%d: mode must be arm or thumb" % (path, n))
            if not reason.strip():
                die("%s:%d: 0x%08X %s has no reason" % (path, n, addr, mode))
            rows.append((addr, mode, reason))
    if not rows:
        die("%s has no rows" % path)
    return rows


def case_blocks(lines):
    """[(addr, first_idx, past_idx)] for the dispatch cases of an image."""
    out = []
    i = 0
    while i < len(lines):
        m = CASE_RE.match(lines[i])
        if not m:
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not CASE_RE.match(lines[j]) and lines[j] != "        default:\n":
            j += 1
        out.append((int(m.group(1), 16), i, j))
        i = j
    return out


def generate(nnktool, image, base, addr, mode, overrides):
    """Run the recompiler for one function; return (summary, body_text, addrs)."""
    tmpdir = os.path.join(ROOT, "work", "phase5at", "gen")
    os.makedirs(tmpdir, exist_ok=True)
    dst = os.path.join(tmpdir, "fn_%08X_%s.c" % (addr, mode))
    cmd = [nnktool, "recompile-cases", image, dst,
           "--base", "0x%08X" % base, "--entry", "0x%08X" % addr,
           "--mode", mode, "--mode-overrides", overrides]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        die("recompile-cases failed for 0x%08X:\n%s%s" % (addr, r.stdout, r.stderr))
    summary = [l for l in r.stdout.splitlines() if l.startswith("recompile-cases:")]
    with open(dst, encoding="utf-8") as fh:
        gtext = fh.read()
    glines = gtext.splitlines(keepends=True)
    gblocks = case_blocks(glines)
    if not gblocks:
        die("recompile-cases emitted no case for 0x%08X" % addr)
    body = "".join(glines[gblocks[0][1]:gblocks[-1][2]])
    return (summary[0] if summary else ""), body, [b[0] for b in gblocks], gtext


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--overrides", default=os.path.join(HERE, "nnk_mode_overrides.txt"))
    ap.add_argument("--nnktool", default=os.path.join(ROOT, "nnktool", "target", "release", "nnktool"))
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--base", default="0x02000000")
    args = ap.parse_args()

    base = int(args.base, 16)
    if not os.path.isfile(args.nnktool):
        die("nnktool not built at %s (cargo build --release in nnktool/)" % args.nnktool)
    if not os.path.isfile(args.image):
        die("guest image not found at %s" % args.image)
    rows = load_overrides(args.overrides)

    with open(args.src, encoding="utf-8", errors="surrogateescape") as fh:
        lines = fh.readlines()
    frozen = case_blocks(lines)
    if not frozen:
        die("no dispatch cases found in %s" % args.src)
    have = {a for (a, _, _) in frozen}

    drop = set()
    inserts = []   # (first_addr, body_text, addr, mode, summary, dropped, missing)
    for addr, mode, _reason in rows:
        summary, body, new_addrs, gtext = generate(
            args.nnktool, args.image, base, addr, mode, args.overrides)
        first = min(new_addrs)
        past_end = max(new_addrs) + (2 if mode == "thumb" else 4)
        dropped = sorted(a for a in have if first <= a < past_end)
        if addr not in dropped:
            die("0x%08X is not a case of the frozen image: an override corrects an "
                "existing row, it does not add a function (that needs a seed)" % addr)
        missing = sorted({int(m.group(1), 16) for m in PC_RE.finditer(gtext)}
                         - have - set(new_addrs))
        drop.update(dropped)
        inserts.append((first, body, addr, mode, summary, dropped, missing, new_addrs))

    inserts.sort()
    out = []
    i = 0
    while i < len(lines):
        m = CASE_RE.match(lines[i])
        if m and int(m.group(1), 16) in drop:
            if int(m.group(1), 16) == inserts[0][0]:
                for ins in inserts:
                    out.append(ins[1])
            j = i + 1
            while (j < len(lines) and not CASE_RE.match(lines[j])
                   and lines[j] != "        default:\n"):
                j += 1
            i = j
            continue
        out.append(lines[i])
        i += 1

    head = MARKER % ", ".join("0x%08X %s" % (i[2], i[3]) for i in inserts)
    with open(args.dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(head)
        fh.writelines(out)

    new_blocks = case_blocks(out)
    addrs = [b[0] for b in new_blocks]
    if len(set(addrs)) != len(addrs):
        die("spliced image has duplicate case addresses")
    # The frozen union image is NOT globally ascending (the overlay/ITCM case
    # blocks were appended out of address order), so only the local ordering
    # around the insertion is meaningful.
    inserted_all = {a for ins in inserts for a in ins[7]}
    for k in range(len(addrs) - 1):
        if addrs[k] in inserted_all and addrs[k + 1] < addrs[k]:
            die("case 0x%08X inserted before a lower case 0x%08X" % (addrs[k], addrs[k + 1]))
        if addrs[k + 1] in inserted_all and addrs[k] > addrs[k + 1]:
            die("case 0x%08X follows a higher case 0x%08X" % (addrs[k + 1], addrs[k]))

    for (first, body, addr, mode, summary, dropped, missing, new_addrs) in inserts:
        print("p5at: 0x%08X -> %s: dropped %d frozen case(s) [%s], inserted %d case(s) [%s]"
              % (addr, mode, len(dropped),
                 " ".join("0x%08X" % a for a in dropped), len(new_addrs),
                 " ".join("0x%08X" % a for a in new_addrs)))
        print("p5at:    %s" % summary)
        if missing:
            print("p5at:    frontier: direct call target(s) this image does not "
                  "translate: %s" % " ".join("0x%08X" % a for a in missing))
    print("p5at mode-override splice: %d function(s) corrected, %d frozen case(s) dropped, "
          "%d case(s) in the image -> %s"
          % (len(inserts), len(drop), len(new_blocks), args.dst))


if __name__ == "__main__":
    main()
