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
    # (3) Phase-4L: the DMA completion/timing shim
    shutil.copyfile(os.path.join(here, "nnk_dma.inc"),
                    os.path.join(outdir, "nnk_dma.inc"))
    # (4) Phase-4M: the VRAMCNT_A..I bank-mapping model
    shutil.copyfile(os.path.join(here, "nnk_vramcnt.inc"),
                    os.path.join(outdir, "nnk_vramcnt.inc"))
    # (5) Phase-4W: the SDL keyboard input model
    shutil.copyfile(os.path.join(here, "nnk_input.inc"),
                    os.path.join(outdir, "nnk_input.inc"))

    # ---- Phase-4M: VRAMCNT_A..I model hooks -----------------------------
    # The generated image's IO model is store-only AND 8-bit-only for
    # 0x04000240..0x04000249: io_write16_sw/io_write32_sw have no case (a
    # W32/W16 store is dropped) and io_read16/io_read32 have no case (reads
    # return 0).  melonDS uses both wide forms (W32 clear of A..D, W16 clear
    # of H..I) before programming the banks byte-wise, and the display
    # pipeline depends on the resulting bank map.  The substitutions below
    # route every width through tools/nnk_vramcnt.inc (same semantics as
    # nnktool/src/recompile.rs carries for the next regeneration).  Each
    # anchor must match EXACTLY once or the split dies: a silently missing
    # hook would leave the store-only class in place.
    _vrc_modern = any("static int vrc_in(uint32_t a)" in l for l in prelude)
    def inject(lines_, needle, repl, what):
        hits = [i for i, l in enumerate(lines_) if needle in l]
        if len(hits) != 1:
            # Modern nnktool images embed the VRAMCNT model (vrc_*) and the IO
            # delegations directly (recompile.rs is authoritative since
            # Phase-4M); the splices below only exist for older pre-generated
            # images.  Skip the vramcnt-family splice when its anchor is absent
            # on such an image instead of dying — the semantics are already there.
            if (not hits and _vrc_modern
                    and ("nnk_vrc_" in repl or "vramcnt" in repl or "powerctl" in repl
                         or "irq_trig" in repl or "card_end_pending" in repl)):
                return list(lines_)
            die("Phase-4M: %s anchor %r matched %d lines"
                % (what, needle, len(hits)))
        out = list(lines_)
        nl = "\n" if lines_[hits[0]].endswith("\n") else ""
        if nl and not repl.endswith("\n"):
            repl += nl          # keep the file's line structure intact
        out[hits[0]] = repl
        return out

    VRC_PROTO = (
        "static int nnk_vrc_in(uint32_t a);\n"
        "static void nnk_vrc_write(uint32_t a, uint32_t v, int width);\n"
        "static uint32_t nnk_vrc_read(uint32_t a, int width);\n"
        "static uint32_t nnk_vrc_stat(void);\n"
        "static void nnk_vrc_mem(uint32_t a);\n"
        "static void nnk_vrc_set_dispcnt(int n, uint32_t v);\n"
        "static void nnk_vrc_powcnt(uint32_t v);\n"
        "static void nnk_sh_write(uint32_t a, uint32_t v, int width);\n"
        "static int nnk_sh_read(uint32_t a, int width, uint32_t *out);\n"
        "static void nnk_vrc_report(void);\n")

    prelude = inject(prelude, "void halt_reason(const char *msg);",
                     "void halt_reason(const char *msg);\n" + VRC_PROTO,
                     "vramcnt prototypes")
    prelude = inject(prelude, "static void io_write32_sw(uint32_t a, uint32_t v) {",
                     "static void io_write32_sw(uint32_t a, uint32_t v) {\n"
                     "    if (nnk_vrc_in(a)) { nnk_vrc_write(a, v, 4); return; }\n",
                     "io_write32_sw width-4")
    prelude = inject(prelude, "    case 0x04000000: dispcnt = v; return;",
                     "    case 0x04000000: dispcnt = v; nnk_vrc_set_dispcnt(0, v); return;\n"
                     "    case 0x04001000: nnk_vrc_set_dispcnt(1, v); return;",
                     "DISPCNT A/B shadow")
    prelude = inject(prelude, "static void io_write16_sw(uint32_t a, uint32_t v) {",
                     "static void io_write16_sw(uint32_t a, uint32_t v) {\n"
                     "    if (nnk_vrc_in(a)) { nnk_vrc_write(a, v, 2); return; }\n",
                     "io_write16_sw width-2")
    prelude = inject(prelude, "static uint32_t io_read32(uint32_t a) {",
                     "static uint32_t io_read32(uint32_t a) {\n"
                     "    { uint32_t _shv; if (nnk_sh_read(a, 4, &_shv)) return _shv; }\n"
                     "    if (nnk_vrc_in(a)) return nnk_vrc_read(a, 4);\n"
                     "    if (a == 0x0400024Eu) return nnk_vrc_stat();\n"
                     "    /* Phase-4M: POWCNT1 readback (melonDS GPU::Read16 case 0x304).\n"
                     "     * The guest's display-init sequence at 0x02002A3C..0x02002A64 is a\n"
                     "     * read-modify-write over 0x04000304; with no readback it reads 0 and\n"
                     "     * the final store is 0x0001 instead of melonDS's 0x820F, i.e. the\n"
                     "     * screen and both 2D engines stay off -> uniform black frames. */\n"
                     "    if ((a & ~3u) == 0x04000304u) return powerctl & 0xFFFFu;\n",
                     "io_read32 width-4 + VRAMSTAT + POWCNT1")
    prelude = inject(prelude, "    case 0x04000304: powerctl = v; return;",
                     "    case 0x04000304: powerctl = v & 0xFFFFu; nnk_vrc_powcnt(v); return;",
                     "POWCNT1 write accounting")
    prelude = inject(prelude,
                     "    if (a >= 0x04000240u && a <= 0x04000248u) return vramcnt[a - 0x04000240u];",
                     "    if (nnk_vrc_in(a)) return nnk_vrc_read(a, 1);",
                     "io_read8 readback")
    prelude = inject(prelude,
                     "    if (a >= 0x04000240u && a <= 0x04000248u) { vramcnt[a - 0x04000240u] = v; return; }",
                     "    if (nnk_vrc_in(a)) { nnk_vrc_write(a, v, 1); return; }",
                     "io_write8_sw width-1")
    for _w in (4, 2, 1):
        prelude = inject(
            prelude,
            "    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, %d); return; }" % _w,
            "    if (gxd && gxd_mem_io(a)) { nnk_vrc_mem(a); gxd_io_write(gxd, a, v, %d); return; }" % _w,
            "st%d window accounting" % (_w * 8))
    # Phase-4M: the 2D register-block readback shadow.  Hooked at the io_*
    # ENTRY points (not the *_sw models) so the shadow always sees the guest's
    # exact address and width — the *_sw 8-bit path funnels through the
    # halfword RMW and would misplace the high byte.
    prelude = inject(prelude, "static uint32_t io_read16(uint32_t a) {",
                     "static uint32_t io_read16(uint32_t a) {\n"
                     "    { uint32_t _shv; if (nnk_sh_read(a, 2, &_shv)) return _shv; }\n",
                     "io_read16 2D shadow")
    prelude = inject(prelude, "static uint32_t io_read8(uint32_t a) {",
                     "static uint32_t io_read8(uint32_t a) {\n"
                     "    { uint32_t _shv; if (nnk_sh_read(a, 1, &_shv)) return _shv; }\n",
                     "io_read8 2D shadow")
    for _w, _fn in ((4, "io_write32"), (2, "io_write16"), (1, "io_write8")):
        prelude = inject(prelude,
                         "static void %s(uint32_t a, uint32_t v) {" % _fn,
                         "static void %s(uint32_t a, uint32_t v) {\n"
                         "    nnk_sh_write(a, v, %d);\n" % (_fn, _w),
                         "%s 2D shadow" % _fn)
    sys.stderr.write("Phase-4M: VRAMCNT_A..I width-correct model spliced into the "
                     "IO accessors (7 hooks + 3 window counters)\n")

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

    # ---- Phase-4N: cart transfer-done IRQ + rule-based delivery ----------
    # Measurement (p4n_wake.txt, melonDS LoopWatch hook): the ARM9 WFI idle
    # loop at 0x0200712C is woken by IRQ bit 19 (IRQ_CartXferDone) --
    # "irqwake ... ie=0x000C0009 if=0x00280000 ime=1 enabled=0x00080000" --
    # once per card transfer, every ~21,700 card-bus cycles.  melonDS raises
    # it in NDSCart::Interface::ROMEndTransfer when AUXSPICNT bit14 is set.
    # The generated image has two gaps that make that unreachable:
    #   (a) a data-read transfer never schedules ROMTransfer_End, so ROMCnt
    #       bit31 stays set and no end-of-transfer event ever fires;
    #   (b) card_end_transfer() drops the cart IRQ entirely (Phase-4D note);
    #   (c) IRQ delivery was gated to the 3 scripted triggers (IRQ_TRIG_N),
    #       so nothing after step 9,097,801 is ever delivered.
    # Phase-4T: the FIRST Phase-4N inject ((a), "card end-of-transfer
    # scheduling") is REMOVED: it made card_advance_receive schedule
    # ROMTransfer_End after the last RECEIVED word, but that is melonDS's
    # ROMAdvanceSend (write mode) semantics.  ROMAdvanceReceive (read mode)
    # returns without scheduling — the read transfer ends only when the CPU
    # pops the last FIFO word (ReadROMData -> ROMEndTransfer).  With the end
    # scheduled, busy+DRQ were cleared ~20 cycles after the last word was
    # produced, so the guest's B8 dummy-read DRQ wait at 0x02016974 deadlocked
    # and the whole card stream froze after 1398 B7 starts (Phase-4T, measured
    # against p4q_mel_io.txt: melonDS continues streaming to 2279 B7 starts).
    # The generated image already carries the correct line; it stays verbatim.
    if not any("iflags |= 0x00080000u" in l for l in prelude):
        # Modern images already emit the ROMEndTransfer IRQ_CartXferDone raise;
        # re-splicing it would duplicate the line and break the card-trace
        # anchor below (which must match exactly once).
        prelude = inject(prelude,
                         "    card_next = CARD_NEVER; card_end_pending = 0;",
                         "    card_next = CARD_NEVER; card_end_pending = 0;\n"
                         "    if (spicnt & (1u << 14)) iflags |= 0x00080000u;"
                         "   /* ROMEndTransfer: IRQ_CartXferDone */",
                         "card transfer-done IRQ raise")
    prelude = inject(prelude,
                     "        else if (!irq_active && irq_trig_n < IRQ_TRIG_N"
                     " && steps > irq_trig_log[irq_trig_n]",
                     "        else if (!irq_active"
                     " && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])",
                     "rule-based IRQ delivery")
    sys.stderr.write("Phase-4N: cart IRQ + rule-based delivery spliced\n")

    # ---- Phase-5F: periodic VBlank IRQ producer (unbounded) ---------------
    # Measurement (PHASE5F.md): the runtime raises IF only from (a) the finite
    # Phase-4C script table, whose last entry is step 9,097,800, and (b) the
    # card-transfer-end raise of Phase-4N.  There is no periodic, guest-visible
    # interrupt source at all, while melonDS keeps the ARM9's idle WFI supplied
    # for the whole boot (`p5f/wfi3.txt`: 25,232 wakes at 0x0200911C over
    # ts 56.5M..2.16B, IE=0x00040019, enabled bits 0/3/4/18/19).
    #
    # melonDS source (GPU.cpp:1272 SetDispStatIRQ(0,0) at VCount==192):
    # IF bit 0 is raised when the DISPSTAT VBlank flag makes its 0->1 edge AND
    # DISPSTAT bit 3 (VBlank IRQ enable) is set -- edge-triggered, not a level.
    # This runtime's DISPSTAT flag is derived from VCOUNT (io_dispstat_read),
    # so the same edge is the VCOUNT==192 transition.  The producer is bounded
    # only by the frame clock (CYCLES_PER_SCANLINE * SCANLINES), i.e. unbounded
    # in run time, and it does not depend on any captured trace.
    prelude = inject(prelude,
                     "    cycles++;                       /* coarse timing: ~1 cycle per instruction */",
                     "    cycles++;                       /* coarse timing: ~1 cycle per instruction */\n"
                     "    {   /* Phase-5F: VBlank IRQ producer (melonDS GPU.cpp VCount==192\n"
                     "         * -> SetDispStatIRQ(0,0), gated on DISPSTAT bit 3). */\n"
                     "        static uint32_t p5f_vc_last = 0xFFFFFFFFu;\n"
                     "        uint32_t p5f_vc = vcount();\n"
                     "        if (p5f_vc != p5f_vc_last) {\n"
                     "            if (p5f_vc == 192u && (dispstat & 0x8u)) iflags |= 0x1u;\n"
                     "            p5f_vc_last = p5f_vc;\n"
                     "        }\n"
                     "    }",
                     "Phase-5F VBlank IRQ producer")
    sys.stderr.write("Phase-5F: periodic VBlank IRQ producer spliced\n")

    # ---- Phase-5F: WFI / idle-loop state probe (env NNK_WFI_PROBE) --------
    # The Phase-5E stop-report could only say "the guest spins at 0x02009110".
    # This probe prints the delivery-relevant state at that PC (and at the WFI
    # instruction itself) so the next worker can read WHY no wake happens
    # instead of inferring it: IE/IF/CPSR.I, the card state machine (card_next
    # / ROMCNT / SPI CNT / DMA2 CNT) and DISPSTAT.  Inactive unless
    # NNK_WFI_PROBE is set; capped so a runaway run cannot fill the disk.
    prelude = inject(prelude,
                     "static uint32_t dispstat = 0, dispcnt = 0;",
                     "static uint32_t dispstat = 0, dispcnt = 0;\n"
                     "/* Phase-5F: NNK_WFI_PROBE=1 -> state dump at the ARM9 WFI\n"
                     " * helper 0x02009118 and the idle-loop head 0x02009110. */\n"
                     "static int nnk_wfi_probe = -1;\n"
                     "static int nnk_wfi_n = 0;",
                     "Phase-5F WFI probe statics")
    prelude = inject(prelude,
                     "        else if (!irq_active && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])",
                     "        if (nnk_wfi_probe < 0) {\n"
                     "            const char *s = getenv(\"NNK_WFI_PROBE\");\n"
                     "            nnk_wfi_probe = (s && s[0]) ? 1 : 0;\n"
                     "        }\n"
                     "        if (nnk_wfi_probe && nnk_wfi_n < 20000\n"
                     "            && (pc == 0x02009110u || pc == 0x02009118u)) {\n"
                     "            nnk_wfi_n++;\n"
                     "            fprintf(stderr, \"wfiprobe n=%d pc=0x%08X step=%llu ime=%u ie=0x%08X if=0x%08X cpsr=%02X \"\n"
                     "                    \"cardnext=%llu romcnt=0x%08X spicnt=0x%04X dma2=0x%08X dispstat=0x%04X\\\\n\",\n"
                     "                    nnk_wfi_n, pc, (unsigned long long)steps, ime, ie, iflags, cpsr_ctl,\n"
                     "                    (unsigned long long)card_next, romcnt, (unsigned)spicnt,\n"
                     "                    dma_cnt[2], (unsigned)dispstat);\n"
                     "        }\n"
                     "        else if (!irq_active && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])",
                     "Phase-5F WFI probe hook")
    sys.stderr.write("Phase-5F: WFI state probe spliced (NNK_WFI_PROBE)\n")

    # ---- Phase-4O: full cart-event trace (env NNK_CARDLOG) ---------------
    # Phase-4N's card evidence was limited to the image's own 4 start logs, so
    # "no start after card_cycles=7,812,062" was an artifact of the log cap,
    # not of the guest.  This splices an unbounded start/end trace with the
    # card-bus clock and the ARM9 PC of the instruction that caused it, so the
    # native producer stream can be diffed against the melonDS ground truth
    # (MELONDS_CARDLOG_PATH, NDSCart.cpp).  Inactive unless NNK_CARDLOG is set.
    prelude = inject(prelude,
                     "#include <string.h>",
                     "#include <string.h>\n#include <stdarg.h>",
                     "stdarg include")
    prelude = inject(prelude,
                     "static int card_xfer_log_n;",
                     "static int card_xfer_log_n;          /* transfer-start logs emitted */\n"
                     "/* Phase-4O: unbounded card-event trace, NNK_CARDLOG=path */\n"
                     "static FILE* cardlog_fp = NULL;\n"
                     "static int cardlog_tried = 0;\n"
                     "static int irq_probe_n = 0;\n"
                     "static void cardlog_ev(const char* fmt, ...) {\n"
                     "    va_list ap;\n"
                     "    if (!cardlog_tried) {\n"
                     "        cardlog_tried = 1;\n"
                     "        const char* p = getenv(\"NNK_CARDLOG\");\n"
                     "        if (p && p[0]) { cardlog_fp = fopen(p, \"w\");\n"
                     "            if (cardlog_fp) setvbuf(cardlog_fp, NULL, _IOLBF, 0); }\n"
                     "    }\n"
                     "    if (!cardlog_fp) return;\n"
                     "    va_start(ap, fmt); vfprintf(cardlog_fp, fmt, ap); va_end(ap);\n"
                     "}",
                     "card event log helper")
    prelude = inject(prelude,
                     "    card_poll_n = 0;",
                     "    card_poll_n = 0;\n"
                     "    cardlog_ev(\"start step=%llu cyc=%llu pc=0x%08X cmd=%02X%02X%02X%02X"
                     "%02X%02X%02X%02X size=0x%X spicnt=0x%04X romcnt=0x%08X\\n\",\n"
                     "                (unsigned long long)steps,\n"
                     "                (unsigned long long)card_cycles, last_pc,\n"
                     "                card_cmd[0], card_cmd[1], card_cmd[2], card_cmd[3],\n"
                     "                card_cmd[4], card_cmd[5], card_cmd[6], card_cmd[7],\n"
                     "                datasize, spicnt, val);",
                     "card start trace")
    prelude = inject(prelude,
                     "    if (spicnt & (1u << 14)) iflags |= 0x00080000u;",
                     "    if (spicnt & (1u << 14)) iflags |= 0x00080000u;"
                     "   /* ROMEndTransfer: IRQ_CartXferDone */\n"
                     "    cardlog_ev(\"end step=%llu cyc=%llu pc=0x%08X iflags=0x%08X"
                     " spicnt=0x%04X polls=%d\\n\",\n"
                     "                (unsigned long long)steps,\n"
                     "                (unsigned long long)card_cycles, last_pc, iflags,\n"
                     "                spicnt, card_poll_n);",
                     "card end trace")
    prelude = inject(prelude,
                     "    case 0x04000214: iflags &= ~v; return;",
                     "    case 0x04000214:\n"
                     "        if (v & 0x00080000u) cardlog_ev(\"if_clear step=%llu"
                     " pc=0x%08X write=0x%08X old=0x%08X\\n\",\n"
                     "            (unsigned long long)steps, last_pc, v, iflags);\n"
                     "        iflags &= ~v; return;",
                     "IF ack trace")
    sys.stderr.write("Phase-4O: full card-event trace spliced\n")

    # ---- Phase-4O: idle-loop IRQ state probe ----------------------------
    # Records the delivery-relevant state the first few times the dispatch
    # reaches the SDK idle loop 0x0200712C, so the "why does no IRQ wake it"
    # question is answered with state (ime/ie/if/cpsr/irq_trig_n) rather than
    # inferred from a halt PC.
    prelude = inject(prelude,
                     "        irq_tick();",
                     "        irq_tick();\n"
                     "        if (pc == 0x0200712Cu && irq_probe_n < 12) {\n"
                     "            irq_probe_n++;\n"
                     "            fprintf(stderr, \"irqprobe step=%llu ime=%u ie=0x%08X"
                     " iflags=0x%08X cpsr=0x%02X irq_trig_n=%d irq_active=%d"
                     " ieandif=0x%08X\\n\",\n"
                     "                    (unsigned long long)steps, ime, ie, iflags,"
                     " cpsr_ctl, irq_trig_n, irq_active, ie & iflags);\n"
                     "        }",
                     "idle-loop probe")
    sys.stderr.write("Phase-4O: idle-loop IRQ probe spliced\n")

    # ---- Phase-4Q: ARM7 IPC-FIFO responder (measured request -> reply) ---
    # The u16[0x0206E93A] barrier (native step 18,554,864): the guest's card
    # driver sets handler-mask bit 1 (pc 0x0200FB20) and then SENDS the ARM7
    # two IPC-FIFO requests at pc 0x0200BAD4.  melonDS's ARM7 answers the
    # first (0x80004046) with 0xC0204006 2,076 cycles later, and that "done"
    # event runs the driver's finish tail (0x0200F784..0x0200F7A0) which
    # clears the mask so the waiter at 0x0200FEFC proceeds.  This runtime
    # dropped both requests ("unscripted IPCFIFO request"), so the reply, the
    # IPC-recv IRQ and the mask clear never happened.  The measured table and
    # the modelled latency live in tools/nnk_dma.inc (Phase-4Q section).
    # Prototype: appended to the prelude element that carries the
    # halt_reason forward declaration (that element also carries the
    # VRAMCNT prototype block, so it must be extended, not replaced).
    _hi = [i for i, l in enumerate(prelude)
           if "void halt_reason(const char *msg);" in l]
    if len(_hi) != 1:
        die("Phase-4Q: halt_reason anchor matched %d lines" % len(_hi))
    prelude[_hi[0]] = (prelude[_hi[0]].rstrip("\n")
                       + "\nvoid nnk_ipc_send(uint32_t v);\n")
    prelude = inject(prelude,
                     '        halt_reason("ARM7 responder: unscripted IPCFIFO '
                     'request (see log)");',
                     "        nnk_ipc_send(v);",
                     "Phase-4Q IPC responder hook")
    prelude = inject(prelude,
                     '        fprintf(stderr, "io: unscripted IPCFIFO request '
                     '0x%08X at pc=0x%08X\\n", v, last_pc);',
                     "        /* handled/notable log lives in nnk_ipc_send */",
                     "Phase-4Q IPC responder log moved")
    sys.stderr.write("Phase-4Q: ARM7 IPC-FIFO responder spliced\n")

    # ---- Phase-4T: card-stall probe (ROMCNT-busy hang diagnosis) ----------
    # The native guest spins in the ROMCNT poll loop at 0x02016974 with
    # busy=1 / DRQ=0 forever (150M steps, distinct_pcs frozen at 13,828): the
    # card transfer behind the overlay load never completes, so the guest
    # never reaches the overlay-resident display switch (melonDS pc 0x0207CB10,
    # DISPCNT=0x0021021B at ts 54,276,936).  The probe in tools/nnk_vramcnt.inc
    # dumps the whole card state machine once the guest has polled a busy,
    # DRQ-less ROMCNT 5,000 times; the counters below track how many transfers
    # were started at all (the io_card_start log line is capped at 4).
    prelude = inject(prelude, "static void io_card_start(uint32_t val) {",
                     "static void nnk_card_probe(void);\n"
                     "static unsigned nnk_card_starts;\n"
                     "static uint32_t nnk_card_last_op;\n"
                     "static uint32_t nnk_card_last_addr;\n"
                     "static uint64_t nnk_card_last_cyc;\n"
                     "static uint64_t nnk_blkcrc = 0xcbf29ce484222325ull;\n"
                     "static unsigned nnk_blk_n;\n"
                     "static void io_card_start(uint32_t val) {",
                     "Phase-4T card probe prototypes")
    prelude = inject(prelude,
                     "        return (romcnt & ~0x00800000u) | (card_drq ? 0x00800000u : 0u);",
                     "        nnk_card_probe();\n"
                     "        return (romcnt & ~0x00800000u) | (card_drq ? 0x00800000u : 0u);",
                     "Phase-4T card probe call")
    prelude = inject(prelude, "        card_xfer_log_n++;",
                     "        card_xfer_log_n++;",
                     "Phase-4T card start counter (moved, see below)")
    prelude = inject(prelude, "    card_op = card_cmd[0];",
                     "    if (card_op == 0xB7 && nnk_blk_n) {\n"
                     "        fprintf(stderr, \"p4t: blk addr=0x%08X crc=0x%08llX n=%u steps=%llu\\n\",\n"
                     "                card_addr, (unsigned long long)nnk_blkcrc, nnk_blk_n,\n"
                     "                (unsigned long long)steps);\n"
                     "        nnk_blk_n = 0; nnk_blkcrc = 0xcbf29ce484222325ull;\n"
                     "    }\n"
                     "    card_op = card_cmd[0];",
                     "Phase-4T per-block FIFO checksum")
    prelude = inject(prelude,
                     "        uint32_t ret = card_fifo[card_poscpu];",
                     "        uint32_t ret = card_fifo[card_poscpu];\n"
                     "        if (card_op == 0xB7) { nnk_blkcrc = (nnk_blkcrc ^ ret) * 16777619ull; nnk_blk_n++; }",
                     "Phase-4T FIFO checksum accumulate")
    prelude = inject(prelude, "    card_poll_n = 0;",
                     "    card_poll_n = 0;\n"
                     "    nnk_card_starts++;\n"
                     "    nnk_card_last_op = card_cmd[0];\n"
                     "    nnk_card_last_addr = addr;\n"
                     "    nnk_card_last_cyc = card_cycles;\n"
                     "    if (nnk_card_starts <= 50000u)\n"
                     "        fprintf(stderr, \"p4t: start op=0x%02X addr=0x%08X size=%u cyc=%llu steps=%llu\\n\",\n"
                     "                card_cmd[0], addr, (unsigned)datasize,\n"
                     "                (unsigned long long)card_cycles,\n"
                     "                (unsigned long long)steps);",
                     "Phase-4T B7 stream log")
    sys.stderr.write("Phase-4T: card-stall probe spliced\n")

    # ---- Phase-4K: firmware boot state (console MAC) --------------------
    # The guest reads the console WIFI MAC from 0x02FFFCF4; it is a console /
    # firmware property (melonDS serves DEFAULT_MAC there), not ROM data, and
    # the boot state this runtime seeds did not carry it.  tools/nnk_boot.inc
    # implements the write; it needs the prelude's guest-memory accessors, so
    # the include goes AFTER the prelude and guest_init() gets a forward
    # declaration plus the call.
    shutil.copyfile(os.path.join(here, "nnk_boot.inc"),
                    os.path.join(outdir, "nnk_boot.inc"))
    boot_call = "bios_boot_seed(); }"
    if sum(1 for l in prelude if boot_call in l) != 1:
        die("guest_init() line with %r not found in the prelude" % boot_call)
    prelude = [l.replace(boot_call, "bios_boot_seed(); nnk_boot_firmware(); }")
               for l in prelude]
    sys.stderr.write("Phase-4K: console MAC seeding spliced into guest_init\n")

    # ---- Phase-4W: SDL keyboard input (tools/nnk_input.inc) ---------------
    # io_read16() gets one early-out line at the top: observe guest KEYINPUT
    # (0x04000130) reads for the input trace and serve EXTKEYIN (0x04000136)
    # from the runtime's key state (melonDS NDS.cpp:3892 semantics).
    io16 = "static uint32_t io_read16(uint32_t a) {"
    if sum(1 for l in prelude if io16 in l) != 1:
        die("io_read16() definition line not found in the prelude")
    prelude = [l.replace(io16, io16 + "\n    { uint32_t _k; if (nnk_input_ioread16(a, &_k)) return _k; }")
               if io16 in l else l for l in prelude]
    sys.stderr.write("Phase-4W: KEYINPUT read trace + EXTKEYIN spliced into io_read16\n")

    # ---- main TU: prelude + band table + dispatch + tail
    prelude = list(prelude)          # un-static'd copy
    tf_idx = next((i for i, l in enumerate(prelude)
                   if l == "static void run(void) {\n"), None)
    if tf_idx is None:
        die("run() not found in the prelude")
    main_path = os.path.join(outdir, "nnk_main.c")
    with open(main_path, "w") as fh:
        fh.write('#include "nnk_rt.h"\n\n')
        fh.write("static void nnk_boot_firmware(void);\n")
        fh.write("static int nnk_input_ioread16(uint32_t a, uint32_t *out);\n")
        fh.write("static void nnk_input_tick(void);\n")
        fh.writelines(prelude[:tf_idx])
        fh.write('#include "nnk_boot.inc"\n\n')
        # Phase-4L: DMA completion/timing shim (tools/nnk_dma.inc) — included
        # here so it sees the runtime statics, called once per dispatch below.
        fh.write('#include "nnk_dma.inc"\n\n')
        # Phase-4M: VRAMCNT_A..I model (tools/nnk_vramcnt.inc) — its hooks are
        # spliced into the prelude above; it sees the same runtime statics.
        fh.write('#include "nnk_vramcnt.inc"\n\n')
        # Phase-4W: SDL keyboard -> KEYINPUT/EXTKEYIN (tools/nnk_input.inc) —
        # same runtime statics visible; io_read16 hook spliced into the
        # prelude above, tick called once per dispatch below.
        fh.write('#include "nnk_input.inc"\n\n')
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
        fh.write("            nnk_dma_tick();\n")
        fh.write("            nnk_input_tick();\n")
        fh.write("            if (pc >= 0x02900000u && pc < 0x02A00000u && p4x_band_log < 24u) {\n")
        fh.write("                p4x_band_log++;\n")
        fh.write("                fprintf(stderr, \"p4x band: pc=0x%08X last=0x%08X r0=%08X \"\n")
        fh.write("                        \"r1=%08X r2=%08X r3=%08X r5=%08X r12=%08X r13=%08X \"\n")
        fh.write("                        \"r14=%08X step=%llu\\\\n\",\n")
        fh.write("                        pc, last_pc, r[0], r[1], r[2], r[3], r[5], r[12],\n")
        fh.write("                        r[13], r[14], (unsigned long long)steps);\n")
        fh.write("            }\n")
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
        # Phase-4L: decode-on-demand.  The generation-0 image only carries the
        # recompiled PC set (the recovery roots); a PC that melonDS executes
        # but the roots never reached (measured: 0x0215C034, the tail of the
        # arena routine at 0x0215C014) lands here with rc==0.  Decode the LIVE
        # word instead of halting — the same decoder the SMC generation
        # tracker uses (tools/nnk_smc.inc); it returns 0 only when the word
        # does not decode as ARM or Thumb, and then the run halts honestly.
        fh.write("            if (nnk_smc_exec_any(&pc)) continue;\n")
        fh.write('            fprintf(stderr, "untranslated pc=0x%08x\\n", pc);\n')
        fh.write('            halt_reason("dispatch to untranslated pc");\n')
        fh.write("            return;\n")
        fh.write("        }\n")
        # closing braces of the for-loop and run(), then main() — these live
        # past the dispatch switch, so index the ORIGINAL line array.
        fh.writelines(lines[for_close_idx:run_close_idx + 1])
        # Phase-4M: print the VRAMCNT bank-map report before the run summary
        # (the same line group every run, so two runs are diffable).
        tail = list(lines[run_close_idx + 1:])
        tail_repl = [l for l in tail if 'fprintf(stderr, "done steps=' in l]
        if len(tail_repl) != 1:
            die("Phase-4M: run-summary anchor matched %d lines" % len(tail_repl))
        tail = inject(tail, 'fprintf(stderr, "done steps=',
                      "    nnk_vrc_report();\n" + tail_repl[0], "vramcnt report call")
        fh.writelines(tail)
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
