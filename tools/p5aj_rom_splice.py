#!/usr/bin/env python3
"""
tools/p5aj_rom_splice.py — Phase-5AJ: make the ROM the only input.

The frozen dispatch image (work/phase3a/arm9_thumb_ovl_itcm_p5u.c) loads its
guest image from argv[1] with fopen/fread and fills the card model only from
NNK_ROM_PATH.  Phase-5AJ derives every guest image from the card image passed
as argv[1] (tools/nnk_rom.inc), so the two prelude sites must change:

  * main():  argv[1] is the .nds; nnk_rom_image_load() derives the 445,304-byte
             boot image out of it, or falls back to the legacy raw image;
  * io_load_rom(): attach the argv card image to `rom`/`rom_len` instead of
             requiring NNK_ROM_PATH (which stays a debug override).

Both edits are text-identical to the canonical prelude in
nnktool/src/recompile.rs, and each is a no-op when the marker is already
present, so splicing a rebuilt image cannot double-apply.

usage: python3 tools/p5aj_rom_splice.py <in.c> <out.c>
"""
import sys

OLD_MAIN = """    const char *path = argc > 1 ? argv[1] : "arm9_image.bin";
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open guest image %s\\n", path); return 2; }
    size_t got = fread(gmem, 1, IMAGE_LEN, fp);
    fclose(fp);
    if (got != IMAGE_LEN) { fprintf(stderr, "short read: got %zu, want %u\\n", got, IMAGE_LEN); return 2; }
"""

NEW_MAIN = """    /* Phase-5AJ: the card image is the ONLY input.  argv[1] is the .nds; the
     * guest image the dispatch loop boots, the decompressed ARM9 and the
     * overlay arena window are all derived from it in memory at start-up
     * (tools/nnk_rom.inc).  A raw guest image still loads for A/B work. */
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!path || !path[0]) {
        fprintf(stderr, "usage: %s <game.nds>\\n", argv[0]);
        fprintf(stderr, "  the card image is the only input; every guest image is\\n"
                        "  derived from it at start-up (Phase-5AJ).\\n");
        return 2;
    }
    if (nnk_rom_image_load(path, gmem, IMAGE_LEN) != 0) return 2;
    size_t got = nnk_rom_guest_len;
"""

OLD_IO = """static void io_load_rom(void) {
    const char *p = getenv("NNK_ROM_PATH");
"""

NEW_IO = """static void io_load_rom(void) {
    /* Phase-5AJ: the card image came in as argv[1] and is already mapped as
     * `rom`/`rom_len` (tools/nnk_rom.inc).  NNK_ROM_PATH stays as a debug
     * override for the raw-image path; nothing is required. */
    if (rom && rom_len) {
        rom_loaded = 1;
        fprintf(stderr, "io: card rom %zu bytes attached from argv (Phase-5AJ)\\n", rom_len);
        io_ovl_init();
        return;
    }
    {
    const char *p = getenv("NNK_ROM_PATH");
"""

OLD_IO_END = """    fprintf(stderr, "io: card rom %zu bytes from %s\\\\n", rom_len, p);
}
"""

NEW_IO_END = """    fprintf(stderr, "io: card rom %zu bytes from %s\\\\n", rom_len, p);
    }
}
"""


def splice(text):
    applied = []
    if "nnk_rom_image_load" in text:
        applied.append("main (already spliced)")
    else:
        if OLD_MAIN not in text:
            raise SystemExit("p5aj: main() load block not found (image changed?)")
        text = text.replace(OLD_MAIN, NEW_MAIN, 1)
        applied.append("main")
    if "io: card rom %zu bytes attached from argv" in text:
        applied.append("io_load_rom (already spliced)")
    else:
        if OLD_IO not in text:
            raise SystemExit("p5aj: io_load_rom() head not found (image changed?)")
        text = text.replace(OLD_IO, NEW_IO, 1)
        if OLD_IO_END not in text:
            raise SystemExit("p5aj: io_load_rom() tail not found (image changed?)")
        text = text.replace(OLD_IO_END, NEW_IO_END, 1)
        applied.append("io_load_rom")
    return text, applied


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: python3 tools/p5aj_rom_splice.py <in.c> <out.c>")
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()
    out, applied = splice(text)
    with open(dst, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(out)
    print("p5aj: %s -> %s (%s)" % (src, dst, ", ".join(applied)))


if __name__ == "__main__":
    main()
