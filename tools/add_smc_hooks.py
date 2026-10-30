#!/usr/bin/env python3
"""Phase-4J: splice the SMC hooks into an EXISTING generated dispatch image.

`nnktool/src/recompile.rs` now emits the Phase-4J hooks itself:

    * `smc_note(a, 4u/2u/1u);` at the top of st32/st16/st8   (code-window
      write tracking, tools/nnk_smc.inc),
    * `#include "nnk_smc.inc"` before `guest_init()`,
    * `smc_arm();` after the guest image has been loaded in `main()`,
    * `smc_stats();` after `run()`.

This script performs exactly those edits on an image C that was generated
BEFORE the recompiler change, so the SMC work does not require regenerating the
37 MB merged dispatch image (and re-deriving its seeds) just to recompile the
same instruction set.  The inserted text is byte-identical to what the emitter
writes, so a later full regeneration produces the same file.

usage: tools/add_smc_hooks.py <in.c> <out.c>
"""
import sys

INCLUDE = (
    '/* Phase-4J: SMC runtime image-state tracking (generation-0 snapshot, per-band\n'
    ' * generation counters and the runtime re-decoder).  Kept in tools/nnk_smc.inc\n'
    ' * so the mechanism is reviewable as C instead of Rust string escaping; the\n'
    ' * build copies it next to the generated TU. */\n'
    '#include "nnk_smc.inc"\n'
)
NOTE_DECL = (
    '/* Phase-4J: code-window write tracking (SMC).  Defined in tools/nnk_smc.inc,\n'
    ' * which the dispatch image pulls in at the end of this prelude; every guest\n'
    ' * store reports its address so the affected 32 KiB band\'s generation counter\n'
    ' * advances and compiled cases for that band are invalidated at dispatch. */\n'
    'static inline void smc_note(uint32_t a, uint32_t n);\n\n'
)


def one(s, needle, repl, what):
    if s.count(needle) != 1:
        sys.exit("smc-hooks: expected exactly one %s anchor, found %d"
                 % (what, s.count(needle)))
    return s.replace(needle, repl, 1)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    s = open(src, encoding="utf-8", errors="replace").read()
    if "nnk_smc.inc" in s:
        print("smc-hooks: already spliced")
        open(out, "w", encoding="utf-8").write(s)
        return

    s = one(s, "static inline void st32(uint32_t a, uint32_t v) {\n",
            NOTE_DECL + "static inline void st32(uint32_t a, uint32_t v) {\n",
            "st32")
    gx32 = ("#ifdef NNK_GX\n"
            "    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 4); return; }\n"
            "#endif\n")
    s = one(s, gx32, gx32 + "    smc_note(a, 4u);\n", "st32 gx guard")
    gx16 = ("#ifdef NNK_GX\n"
            "    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 2); return; }\n"
            "#endif\n")
    s = one(s, gx16, gx16 + "    smc_note(a, 2u);\n", "st16 gx guard")
    gx8 = ("#ifdef NNK_GX\n"
           "    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 1); return; }\n"
           "#endif\n")
    s = one(s, gx8, gx8 + "    smc_note(a, 1u);\n", "st8 gx guard")

    s = one(s, "static void guest_init(void) {", INCLUDE + "\nstatic void guest_init(void) {",
            "guest_init")
    s = one(s, "    guest_init();\n",
            "    smc_arm();          /* Phase-4J: snapshot the compiled generation */\n"
            "    guest_init();\n", "smc_arm call")
    s = one(s, "    run();\n",
            "    run();\n    smc_stats();       /* Phase-4J: SMC generation accounting */\n",
            "run()")
    open(out, "w", encoding="utf-8").write(s)
    print("smc-hooks: spliced SMC hooks into %s" % out)


if __name__ == "__main__":
    main()
