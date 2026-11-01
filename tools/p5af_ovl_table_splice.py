#!/usr/bin/env python3
"""Phase-5AF: splice the table-driven overlay registry into a frozen image.

`work/phase3a/arm9_thumb_ovl_itcm_p5u.c` (the image every acceptance run is
build from) predates even the Phase-4G overlay registry: grepping it for
`io_ovl_init` finds nothing, so the shipped runtime had NO overlay knowledge at
all -- the slot map in PHASE4G_OVERLAY_SLOTS.md came from a separate run of a
differently generated image.  That is the real explanation of "we only ever
measured loads at 0x020D0A00 and 0x02159FE0".

This splice inserts the canonical block from `nnktool/src/recompile.rs`
(between the BEGIN_NNK_OVL_BLOCK / END_NNK_OVL_BLOCK markers, copied verbatim,
so the two cannot drift) plus its five hook sites:

  1. forward declarations before `io_load_rom`      (io_ovl_init and friends)
  2. `io_ovl_init();` after `fclose(f); rom_loaded = 1;`
  3. `if (card_cmd[0] == 0xB7) io_ovl_note(addr);` after `card_op = card_cmd[0];`
  4. `io_ovl_dump_tick(pc);` at the top of the dispatch loop
  5. `io_ovl_pc_report(pc);` before `halt_reason("dispatch to untranslated pc")`
  6. `io_ovl_dump_exit();` before main's `done steps=` line

usage: p5af_ovl_table_splice.py <image.c> <out.c>
Idempotent: an image that already contains `io_ovl_pc_report` is copied
unchanged (the same policy as the p5x / p5ae splices in tools/build_split.sh).
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CANON = os.path.join(HERE, '..', 'nnktool', 'src', 'recompile.rs')
BEGIN = '/* BEGIN_NNK_OVL_BLOCK'
END = '/* END_NNK_OVL_BLOCK */'

FORWARD = """/* Phase-5AF: table-driven overlay registry (canonical text lives in
 * nnktool/src/recompile.rs between BEGIN/END_NNK_OVL_BLOCK; the splice copies
 * it verbatim, so this frozen image and the emitter cannot drift). */
static void io_ovl_init(void);
static void io_ovl_note(uint32_t addr);
static void io_ovl_pc_report(uint32_t pc);
static void io_ovl_dump_tick(uint32_t pc);
static void io_ovl_dump_exit(void);
"""


def canonical_block():
    src = open(CANON, encoding='utf-8', errors='replace').read()
    a = src.find(BEGIN)
    b = src.find(END)
    if a < 0 or b < 0 or b < a:
        sys.exit('cannot find the overlay block markers in %s' % CANON)
    block = src[a:b + len(END)]
    # the Rust raw string holds the C text verbatim; nothing to unescape.
    if 'io_ovl_pc_report' not in block or 'ctor_hi' not in block:
        sys.exit('overlay block in %s looks incomplete' % CANON)
    return block + '\n'


def insert_after(text, anchor, insert, what):
    if text.count(anchor) != 1:
        sys.exit('splice anchor for %s: %d matches (want 1)' % (what, text.count(anchor)))
    i = text.index(anchor) + len(anchor)
    return text[:i] + insert + text[i:]


def insert_before(text, anchor, insert, what):
    if text.count(anchor) != 1:
        sys.exit('splice anchor for %s: %d matches (want 1)' % (what, text.count(anchor)))
    i = text.index(anchor)
    return text[:i] + insert + text[i:]


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, out = argv[1], argv[2]
    text = open(src, encoding='utf-8', errors='replace').read()
    if 'io_ovl_pc_report' in text:
        print('p5af splice: %s already carries the overlay registry (no-op)' % src)
        if os.path.abspath(src) != os.path.abspath(out):
            shutil.copyfile(src, out)
        return 0
    n_before = len(text)
    text = insert_before(text, 'static void io_load_rom(void) {', FORWARD, 'forward decls')
    text = insert_after(text, '    fclose(f); rom_loaded = 1;\n', '    io_ovl_init();\n',
                        'io_load_rom init call')
    text = insert_after(text, '    card_op = card_cmd[0];\n',
                        '    if (card_cmd[0] == 0xB7) io_ovl_note(addr);  /* Phase-5AF */\n',
                        'overlay load event')
    text = insert_before(text, 'static void run(void) {', canonical_block(), 'registry block')
    text = insert_before(text, '        switch (pc) {\n',
                         '        io_ovl_dump_tick(pc);   /* Phase-5AF */\n', 'dump tick')
    text = insert_before(text, '            halt_reason("dispatch to untranslated pc");\n',
                         '            io_ovl_pc_report(pc);   /* Phase-5AF */\n',
                         'untranslated-pc report')
    # main(): the exit dump; also keep the report on the same halted state.
    m = re.search(r'\n    printf\("done steps=', text)
    if not m:
        sys.exit('splice anchor for the exit dump: main() "done steps" line not found')
    text = text[:m.start()] + '\n    io_ovl_dump_exit();   /* Phase-5AF */\n' + text[m.start():]
    open(out, 'w', encoding='utf-8').write(text)
    print('p5af splice: %s -> %s (+%d bytes, 6 hooks)' % (src, out, len(text) - n_before))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
