#!/usr/bin/env python3
"""Split nnktool's single-TU dispatch image into many small translation units.

nnktool ``recompile-all`` emits one C program whose ``run()`` contains one
giant flat ``switch (pc)`` with one ``case 0xADDRu:`` label per translated
instruction (73,120 cases / 23.6 MB in the current Phase-3A image).  cc needs
many minutes and GiBs of RAM to build that as a single TU, so this script
rewrites the file into:

  nnk_rt.h          extern declarations for the runtime symbols the translated
                    cases reference, the band-shift constants, and prototypes
                    for ALL possible band functions (stable cache keys)
  nnk_main.c        the original runtime prelude (IO engine, guest memory,
                    HLE, trace) + a 12-bit band dispatch table + main()
  nnk_part_NNN.c    one TU per ADDRESS BAND of translated PCs; each part
                    function carries its own flat ``switch (pc)`` with only
                    that band's cases and reports whether it handled the PC

Part identity is keyed by PC ADDRESS, not by position in the file: adding a
recomp root that creates a new translated PC only changes the one band part
that PC falls into (plus nnk_main.c's dispatch table), instead of shifting
every subsequent group and forcing a full recompile.  To keep cache keys
stable, nnk_rt.h always declares all NNK_BAND_COUNT possible band functions
even when only a few bands exist.  Stale nnk_part_*.c files from a previous,
larger manifest are deleted after a successful split.

This is the Ni no Kuni port of DQ8's tools/split_dispatch.py, adapted to the
nnk emitter's flat dispatch format.  Differences forced by the format:

* DQ8's dispatch is ``switch (pc >> 12) { case PAGE: switch (pc) {...} }``;
  nnk's is one flat ``switch (pc)``.  The splitter therefore groups case
  labels by band itself and emits one switch per part.
* DQ8's case bodies never return early.  nnk's contain 17 bare ``return;``
  statements (undefined-instruction traps and failed SWI HLE) that return
  from run().  Inside a part function a bare return would return from the
  PART instead, so every bare ``return;`` is rewritten to ``return 2;`` and
  nnk_main.c maps rc==2 back to a run() return.  A valued return, a goto or
  a continue in a case body is NOT representable this way and aborts the
  split (safety net for future emitter output).

Semantics are preserved exactly: every translated instruction body, every
trace_pc call, the per-instruction steps cadence (nnk_main.c increments
steps once per dispatch, exactly like the mono loop), the default branch's
``untranslated pc=0x%08x`` message + halt, and the step-budget check all
behave like the single-TU build.  The split changes structure only.

Usage:
    python3 tools/split_dispatch.py <generated.c> <outdir> [--band-shift N]

Then compile nnk_main.c with the runtime flag set and nnk_part_*.c with the
guest flag set (see tools/build_split.sh), and link everything together.
"""
import argparse
import os
import re
import shutil
import sys
from typing import NoReturn

# C keywords that can masquerade as declarator names in the crude prelude
# parse (e.g. ``return (uint32_t)...`` inside a one-line function definition).
KEYWORDS = {
    "return", "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "goto", "sizeof", "typeof", "alignof", "inline",
    "static", "extern", "const", "volatile", "register", "restrict", "void",
    "int", "char", "short", "long", "float", "double", "signed", "unsigned",
    "struct", "union", "enum", "typedef", "_Bool", "bool", "NULL",
}

# Part function: takes the current PC by pointer, writes back the next PC when
# it handled the dispatch.  Return codes: 1 = handled (pc updated), 0 = PC not
# in this band's case set (caller reports "untranslated"), 2 = the case body
# executed a bare return (guest halt: caller must return from run()).
PART_FN_TYPE = "typedef int (*NnkPartFn)(uint32_t *pc_io);"
PART_PROTO = "int nnk_part_%03d(uint32_t *pc_io);"

# Phase-4B: the ITCM window sits IMMEDIATELY below GBASE, i.e. exactly one
# 32 KiB band with a negative index ((0x01FF8000 - 0x02000000) >> 15 == -1),
# so it gets its own part file / function instead of a table slot (a
# negative designator is not valid C).  The ITCM part prototype is declared
# unconditionally in nnk_rt.h so the header bytes stay stable whether or not
# the image carries ITCM cases.
ITCM_BAND = -1


def part_fn(bi):
    return "nnk_part_itcm" if bi == ITCM_BAND else "nnk_part_%03d" % bi


def part_file(bi):
    return "nnk_part_itcm.c" if bi == ITCM_BAND else "nnk_part_%03d.c" % bi

CASE_RE = re.compile(r"^        case (0x[0-9A-F]+)u:$")
STATIC_NAME_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?=[\[\(\[=;,\n]|\s*$)")
BARE_RETURN_RE = re.compile(r"\breturn\s*;")
VALUED_RETURN_RE = re.compile(r"\breturn\b\s*[^\s;]")
FORBIDDEN_RE = re.compile(r"\b(goto|continue)\b")


def die(msg) -> NoReturn:
    sys.stderr.write("split_dispatch: %s\n" % msg)
    sys.exit(1)


def strip_comment(line):
    """Remove a trailing ``/* ... */`` or ``// ...`` comment from one line.

    The prelude's declarators carry comments that contain parentheses (e.g.
    ``static uint32_t n, z, c, v;  /* CPSR NZCV (single-bank model) */``).
    ``declaration_for`` must not mistake such a paren for a function's
    parameter list: doing so truncated the line at the comment's ``)`` and
    emitted an unterminated ``/*`` into nnk_rt.h, commenting out every
    following declaration.  Comments are therefore stripped BEFORE the
    function/variable decision.  Output text is unaffected (only analysis
    uses the stripped copy)."""
    out, i, n = [], 0, len(line)
    while i < n:
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            j = line.find("*/", i + 2)
            if j < 0:                      # comment runs past EOL: drop rest
                break
            i = j + 2
            out.append(" ")
            continue
        out.append(line[i])
        i += 1
    return "".join(out)


def declared_names(line):
    """Names declared by a column-0 ``static`` line (crude but precise
    enough: identifiers followed by ``[``, ``(``, ``=``, ``,``, ``;`` or EOL).
    Multi-declarator lines and two-statics-per-line both work."""
    body = strip_comment(line[len("static "):])
    return [m.group(1) for m in STATIC_NAME_RE.finditer(body)
            if m.group(1) not in KEYWORDS]


def declaration_for(name, lines, static_names):
    """Extern declaration for a prelude symbol, derived from its first
    column-0 ``static`` declaration/definition.  Functions become prototypes
    (parameter list only, even for one-line definitions); variables lose their
    initialiser and trailing comment."""
    for i, line in enumerate(lines):
        if not line.startswith("static "):
            continue
        if name not in static_names.get(i, ()):
            continue
        text = strip_comment(line[len("static "):])
        if text.startswith("inline "):
            text = text[len("inline "):]
        if "(" in text:
            start = text.index("(")
            depth = 0
            end = -1
            j = i
            acc = text
            while True:
                depth = 0
                for k in range(start if j == i else 0, len(acc)):
                    if acc[k] == "(":
                        depth += 1
                    elif acc[k] == ")":
                        depth -= 1
                        if depth == 0:
                            end = k
                            break
                if end >= 0:
                    break
                j += 1
                if j >= len(lines):
                    die("unterminated signature for %r at line %d" % (name, i + 1))
                acc += lines[j]
            return acc[:end + 1].strip() + ";"
        decl = text.split(";")[0]
        if "=" in decl:
            decl = decl.split("=")[0]
        return decl.strip() + ";"
    die("shared symbol %r not found in the runtime prelude" % name)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src")
    ap.add_argument("outdir")
    ap.add_argument("--band-shift", type=int, default=15,
                    help="band = (pc - GBASE) >> shift (default 15 = 32 KiB bands)")
    opts = ap.parse_args()
    src, outdir, shift = opts.src, opts.outdir, opts.band_shift
    if not os.path.exists(src):
        die("missing %s" % src)
    if shift < 12 or shift > 24:
        die("--band-shift must be in 12..24")
    os.makedirs(outdir, exist_ok=True)

    with open(src, "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.readlines()
    sys.stderr.write("read %d lines (%.1f MB)\n"
                     % (len(lines), sum(len(x) for x in lines) / 1e6))

    # ---- locate the dispatch structure inside run()
    run_start = next((i for i, l in enumerate(lines)
                      if l == "static void run(void) {\n"), None)
    if run_start is None:
        die("run() not found")
    dispatch_idx = next((i for i in range(run_start, len(lines))
                         if lines[i].strip() == "switch (pc) {"), None)
    if dispatch_idx is None:
        die("dispatch switch not found")
    first_case_idx = next((i for i in range(dispatch_idx, len(lines))
                           if CASE_RE.match(lines[i])), None)
    if first_case_idx is None:
        die("first case label not found")
    default_idx = next((i for i in range(first_case_idx, len(lines))
                        if lines[i] == "        default:\n"), None)
    if default_idx is None:
        die("dispatch default not found")
    if "untranslated pc" not in lines[default_idx + 1]:
        die("unexpected default body at line %d: %r"
            % (default_idx + 1, lines[default_idx + 1][:80]))
    switch_close_idx = next((i for i in range(default_idx + 1, len(lines))
                             if lines[i] == "        }\n"), None)
    if switch_close_idx is None:
        die("switch close brace not found")
    for_close_idx = next((i for i in range(switch_close_idx + 1, len(lines))
                          if lines[i] == "    }\n"), None)
    run_close_idx = next((i for i in range(for_close_idx + 1, len(lines))
                          if lines[i] == "}\n"), None)
    if for_close_idx is None or run_close_idx is None:
        die("run() close braces not found")

    # ---- collect the case blocks (each ends at the next case label)
    blocks = []          # (addr, first_line_index, past_last_line_index)
    i = first_case_idx
    while i < default_idx:
        m = CASE_RE.match(lines[i])
        if not m:
            die("unexpected line %d inside the case region: %r"
                % (i + 1, lines[i][:80]))
        j = i + 1
        while j < default_idx and not CASE_RE.match(lines[j]):
            j += 1
        blocks.append((int(m.group(1), 16), i, j))
        i = j
    addrs = [b[0] for b in blocks]
    if len(set(addrs)) != len(addrs):
        die("duplicate case addresses in the emitter output")
    sys.stderr.write("found %d case blocks (0x%08X..0x%08X)\n"
                     % (len(blocks), addrs[0], addrs[-1]))

    # ---- per-block safety checks: every body must be representable inside a
    # part function.  Bare return; becomes `return 2;` (guest halt sentinel);
    # valued returns, goto and continue cannot be mapped and abort the split.
    n_returns = 0
    for (addr, first, past) in blocks:
        last_stmt = ""
        for k in range(first, past):
            line = lines[k]
            if VALUED_RETURN_RE.search(line.replace("return 2;", "")):
                die("valued return at line %d (pc 0x%08X): %r"
                    % (k + 1, addr, line.strip()[:80]))
            if FORBIDDEN_RE.search(line):
                die("%s in case body at line %d (pc 0x%08X)"
                    % (FORBIDDEN_RE.search(line).group(1), k + 1, addr))
            if BARE_RETURN_RE.search(line):
                n_returns += 1
            if line.strip():
                last_stmt = line.strip()
        if not (last_stmt.endswith("break;") or last_stmt.endswith("return;")
                or last_stmt.endswith("return 2;")):
            die("case 0x%08X does not end in break/return (fall-through is "
                "not splittable): %r" % (addr, last_stmt[:80]))
    sys.stderr.write("rewrote %d bare return statements to the halt sentinel\n"
                     % n_returns)

    # ---- guest window constants (band arithmetic must match the runtime)
    gbase = gsize = None
    itcm_lo = itcm_hi = None
    for line in lines[:run_start]:
        m = re.match(r"#define GBASE (0x[0-9A-Fa-f]+)u?\s*$", line)
        if m:
            gbase = int(m.group(1), 16)
        m = re.match(r"#define GSIZE (0x[0-9A-Fa-f]+)u?\s*$", line)
        if m:
            gsize = int(m.group(1), 16)
        m = re.match(r"#define ITCM_LO (0x[0-9A-Fa-f]+)u?\s*$", line)
        if m:
            itcm_lo = int(m.group(1), 16)
        m = re.match(r"#define ITCM_HI (0x[0-9A-Fa-f]+)u?\s*$", line)
        if m:
            itcm_hi = int(m.group(1), 16)
    if gbase is None or gsize is None:
        die("GBASE/GSIZE #defines not found in the prelude")
    band_count = gsize >> shift
    if band_count << shift != gsize or band_count <= 0:
        die("GSIZE 0x%X is not a multiple of the band size" % gsize)

    # ---- prelude: derive the shared-symbol set from actual usage.
    # `prelude` spans everything before the dispatch switch (DQ8's range) so
    # the main TU keeps run()'s prologue; the runtime statics live before
    # run() itself.  `run` and `main` are dispatch-internal, never shared.
    prelude = lines[:dispatch_idx]
    static_names = {}
    for i, line in enumerate(prelude):
        if line.startswith("static "):
            static_names[i] = tuple(declared_names(line))
    case_idents = set()
    for k in range(first_case_idx, default_idx):
        case_idents.update(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", lines[k]))
    shared = sorted({n for names in static_names.values() for n in names
                     if n in case_idents and n not in ("run", "main")})
    if not shared:
        die("no shared symbols derived from the case region")
    unstatic_lines = [i for i, names in static_names.items()
                      if any(n in shared for n in names)]
    for i in unstatic_lines:
        text = prelude[i][len("static "):]
        if text.startswith("inline "):
            text = text[len("inline "):]
        prelude[i] = text
    sys.stderr.write("shared symbols (%d): %s\n" % (len(shared), " ".join(shared)))
    sys.stderr.write("un-static'd %d prelude lines\n" % len(unstatic_lines))

    # declaration_for() reads the ORIGINAL lines (with `static`), so strip a
    # leading `inline` from each emitted prototype/declaration here.  A shared
    # name declared alongside others on one line (e.g. `n, z, c, v`) yields the
    # same multi-declarator extern several times; dedupe to keep the header tidy.
    seen_decls = set()
    header_decls = []
    for n in shared:
        decl = re.sub(r"^inline\s+", "", declaration_for(n, lines, static_names))
        if decl not in seen_decls:
            seen_decls.add(decl)
            header_decls.append(decl)

    # ---- part files (one TU per address band; band -1 = the ITCM window
    # immediately below GBASE, reachable only when the image carries ITCM
    # cases — tools/merge_itcm_cases.py splices them in)
    bands = {}          # band index -> list of (addr, first, past)
    for blk in blocks:
        addr = blk[0]
        if itcm_lo is not None and itcm_hi is not None and itcm_lo <= addr < itcm_hi:
            bands.setdefault(ITCM_BAND, []).append(blk)
            continue
        band = (addr - gbase) >> shift
        if band < 0 or band >= band_count:
            die("case 0x%08X is outside the guest window 0x%08X..0x%08X"
                % (addr, gbase, gbase + gsize))
        bands.setdefault(band, []).append(blk)

    for bi in sorted(bands):
        chunk = bands[bi]
        path = os.path.join(outdir, part_file(bi))
        with open(path, "w") as fh:
            fh.write("/* generated by tools/split_dispatch.py - do not edit */\n")
            fh.write('#include "nnk_rt.h"\n\n')
            fh.write("int %s(uint32_t *pc_io) {\n" % part_fn(bi))
            fh.write("    uint32_t pc = *pc_io;\n")
            fh.write("    switch (pc) {\n")
            for (addr, first, past) in chunk:
                for k in range(first, past):
                    line = lines[k]
                    line = BARE_RETURN_RE.sub("return 2;", line)
                    fh.write(line if line.endswith("\n") else line + "\n")
            fh.write("    default:\n")
            fh.write("        return 0;\n")
            fh.write("    }\n")
            fh.write("    *pc_io = pc;\n")
            fh.write("    return 1;\n")
            fh.write("}\n")

    # ---- Phase-4J: SMC support files -----------------------------------
    # (1) Thumb-16 map.  The runtime re-decoder (tools/nnk_smc.inc) has to know
    # WHICH state a re-decoded PC is in.  A case whose emitter-written
    # fall-through is addr+2 IS a 16-bit Thumb instruction; that is exact and
    # needs no guest-image input.  Everything else is treated as ARM, and the
    # runtime falls back to a Thumb decode when the ARM decode is undefined.
    thumb16 = set()
    for (addr, first, past) in blocks:
        needle = "pc = 0x%08Xu;" % (addr + 2)
        if any(needle in lines[k] for k in range(first, past)):
            thumb16.add(addr)
    runs = []
    for a in sorted(thumb16):
        if runs and runs[-1][1] == a:
            runs[-1][1] = a + 2
        else:
            runs.append([a, a + 2])
    with open(os.path.join(outdir, "nnk_smc_map.h"), "w") as fh:
        fh.write("/* generated by tools/split_dispatch.py - Phase-4J Thumb map */\n")
        fh.write("#ifndef NNK_SMC_MAP_H\n#define NNK_SMC_MAP_H\n")
        fh.write("static const uint32_t nnk_thumb_runs[%d][2] = {\n" % len(runs))
        for lo, hi in runs:
            fh.write("    { 0x%08Xu, 0x%08Xu },\n" % (lo, hi))
        fh.write("};\n")
        fh.write("#define NNK_THUMB_RUNS %d\n" % len(runs))
        fh.write("static inline int nnk_pc_is_thumb(uint32_t pc)\n{\n")
        fh.write("    int lo = 0, hi = NNK_THUMB_RUNS - 1;\n")
        fh.write("    while (lo <= hi) {\n")
        fh.write("        int mid = (lo + hi) >> 1;\n")
        fh.write("        if (pc < nnk_thumb_runs[mid][0]) hi = mid - 1;\n")
        fh.write("        else if (pc >= nnk_thumb_runs[mid][1]) lo = mid + 1;\n")
        fh.write("        else return 1;\n")
        fh.write("    }\n    return 0;\n}\n")
        fh.write("#endif\n")
    sys.stderr.write("Phase-4J: %d Thumb-16 cases in %d runs -> nnk_smc_map.h\n"
                     % (len(thumb16), len(runs)))
    # (2) the runtime SMC fragment itself, next to the generated TU
    here = os.path.dirname(os.path.abspath(__file__))
    shutil.copyfile(os.path.join(here, "nnk_smc.inc"),
                    os.path.join(outdir, "nnk_smc.inc"))

    # ---- header: band constants + ALL possible band prototypes so the header
    # content does not change when a new band appears (it is part of every
    # .o's cache key; a growing list would invalidate the whole cache).
    with open(os.path.join(outdir, "nnk_rt.h"), "w") as fh:
        fh.write("/* generated by tools/split_dispatch.py - do not edit */\n")
        fh.write("#ifndef NNK_RT_H\n#define NNK_RT_H\n")
        fh.write("#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n"
                 "#include <string.h>\n\n")
        fh.write('#include "nnk_smc_map.h"\n')
        fh.write("#define NNK_BAND_SHIFT %d\n" % shift)
        fh.write("#define NNK_BAND_COUNT %d\n" % band_count)
        fh.write("#define NNK_GBASE 0x%08Xu\n" % gbase)
        fh.write("#define NNK_GSIZE 0x%08Xu\n" % gsize)
        fh.write("#define NNK_ITCM_LO 0x%08Xu\n" % (itcm_lo if itcm_lo is not None else 0x01FF8000))
        fh.write("#define NNK_ITCM_HI 0x%08Xu\n\n" % (itcm_hi if itcm_hi is not None else 0x02000000))
        for decl in header_decls:
            fh.write("extern %s\n" % decl)
        fh.write("\n%s\n" % PART_FN_TYPE)
        for idx in range(band_count):
            fh.write("%s\n" % (PART_PROTO % idx))
        fh.write("int nnk_part_itcm(uint32_t *pc_io);\n")
        fh.write("#endif\n")

    # ---- main TU: prelude + band table + dispatch + tail
    prelude = list(prelude)          # un-static'd copy
    tf_idx = next((i for i, l in enumerate(prelude)
                   if l == "static void run(void) {\n"), None)
    if tf_idx is None:
        die("run() not found in the prelude")
    main_path = os.path.join(outdir, "nnk_main.c")
    with open(main_path, "w") as fh:
        fh.write('#include "nnk_rt.h"\n\n')
        fh.writelines(prelude[:tf_idx])
        fh.write("static const NnkPartFn nnk_band_table[NNK_BAND_COUNT] = {\n")
        for bi in sorted(bands):
            if bi == ITCM_BAND:
                continue    # negative index: dispatched via its own branch
            fh.write("    [%d] = %s,\n" % (bi, part_fn(bi)))
        fh.write("};\n\n")
        # run() prologue verbatim: entry pc, loop, per-instruction step check.
        # `run` was never shared, so it still carries its `static` qualifier.
        fh.writelines(prelude[tf_idx:dispatch_idx])
        # dispatch: band lookup replaces the giant switch.  rc==1 -> next
        # instruction; rc==2 -> case body requested run() return (halt);
        # otherwise the mono default branch, verbatim in effect.
        fh.write("        {\n")
        fh.write("            int rc = 0;\n")
        fh.write("            /* Phase-4J: SMC — if a guest store changed the word this PC\n")
        fh.write("             * dispatches, the compiled case is from a stale image generation:\n")
        fh.write("             * re-decode it from live RAM and skip the band switch. */\n")
        fh.write("            if (nnk_smc_try(&pc)) continue;\n")
        if ITCM_BAND in bands:
            # ITCM-resident code: band -1, below the flat window, dispatched
            # to its own part (Phase-4B execute-from-ITCM).
            fh.write("            if (pc >= NNK_ITCM_LO && pc < NNK_ITCM_HI) {\n")
            fh.write("                rc = nnk_part_itcm(&pc);\n")
            fh.write("            } else if (pc >= NNK_GBASE && pc < NNK_GBASE + NNK_GSIZE) {\n")
        else:
            fh.write("            if (pc >= NNK_GBASE && pc < NNK_GBASE + NNK_GSIZE) {\n")
        fh.write("                NnkPartFn fn = nnk_band_table[(pc - NNK_GBASE) >> NNK_BAND_SHIFT];\n")
        fh.write("                if (fn) rc = fn(&pc);\n")
        fh.write("            }\n")
        fh.write("            if (rc == 1) continue;\n")
        fh.write("            if (rc == 2) return;\n")
        fh.write('            fprintf(stderr, "untranslated pc=0x%08x\\n", pc);\n')
        fh.write('            halt_reason("dispatch to untranslated pc");\n')
        fh.write("            return;\n")
        fh.write("        }\n")
        # closing braces of the for-loop and run(), then main() — these live
        # past the dispatch switch, so index the ORIGINAL line array.
        fh.writelines(lines[for_close_idx:run_close_idx + 1])
        fh.writelines(lines[run_close_idx + 1:])
    n_cases = sum(len(c) for c in bands.values())
    if n_cases != len(blocks):
        die("parts carry %d cases but the source has %d" % (n_cases, len(blocks)))
    sys.stderr.write("wrote %s (+%d part files, %d bands, %d cases)\n"
                     % (main_path, len(bands), band_count, n_cases))

    # ---- prune stale part files (leftovers from a previous, larger manifest)
    fresh = {part_file(bi) for bi in bands}
    removed = 0
    for name in os.listdir(outdir):
        if re.match(r"^nnk_part_(?:\d+|itcm)\.c$", name) and name not in fresh:
            os.remove(os.path.join(outdir, name))
            removed += 1
    if removed:
        sys.stderr.write("pruned %d stale part file(s) not in the fresh manifest\n"
                         % removed)


if __name__ == "__main__":
    main()
