# Ni no Kuni: Shikkoku no Madoushi (NDS, B2KJ) — static recompilation

A Rust recompiler (`nnktool`) turns the game's ARM9 into one C function per
guest PC; the generated dispatch image plus a C runtime boots the game
natively.  The design documents are the `PHASE*.md` files and `ROM_MAP.md`
(no game bytes are ever committed: `Ni No Kuni.nds`, `work/` and every derived
image stay out of git).

## Run it — the card image is the ONLY input

```bash
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
    bash tools/build_split.sh out/p5aj/nnk_runner      # once
./run.sh "/path/Ni No Kuni.nds"
```

`./run.sh <rom>` takes the `.nds` and nothing else.  At start-up the runtime
derives, in memory, everything the older phases required as separate files
(Phase-5AJ, `PHASE5AJ.md`):

| derived image | from the ROM | was, before 5AJ |
|---|---|---|
| guest image 445,304 B @ `0x02000000` | ARM9 slice `0x4000+0x49328`, zero-padded | argv `arm9_stored_padded.bin` |
| decompressed ARM9 445,304 B | DS code compression of that slice | `NNK_SMC_IMAGE` `arm9.bin` |
| overlay arena window 0xC0000 B @ `0x020C0000` | overlay 17 (OVT + FAT), decompressed | `NNK_SMC_IMAGE` `ovl_p4f4.bin` |
| movie frames | `data/movie/*.mods` decoded in-process | `NNK_MOVIE_YUV*` dumps (removed in 5AH) |

A clean directory holding only the runner and the ROM boots, shows the notice
screen, plays the intro and runs to the step budget:

```
$ cd /tmp/clean_rom && ls
Ni No Kuni.nds  nnk_runner
$ ./nnk_runner "Ni No Kuni.nds"
rom: card image Ni No Kuni.nds (536870912 B) arm9 299816 B @0x4000 -> 0x02000000 ...
smc: generation-0 arm9 0x02000000..0x0206CB78 (445304 bytes) derived from the ROM (fnv=6087d11200ee29b2)
smc: generation-0 arena window 0x020C0000..0x02180000 (786432 bytes, overlay 17 at its OVT base) derived from the ROM
```

## Debug overrides (reviewer tools, never requirements)

| variable | what it does |
|---|---|
| `NNK_SMC_IMAGE="0xADDR:path[,...]"` | SMC generation-0 snapshot from files instead of the ROM (A/B) |
| `NNK_SMC_GEN0_OVL=<id>` | overlay placed in the derived arena window (default 17) |
| `NNK_ROM_PATH=<path>` | card image for the card model when argv[1] is a raw guest image |
| `NNK_MOVIE_YUV` / `NNK_MOVIE_YUV2` | A/B the movie HLE against raw yuv420p dumps |
| `NNK_MOVIE_MODS` / `NNK_MOVIE_MODS2` | name another container inside the card image |
| `NNK_GX=window` / `headless` | SDL2 window vs the headless renderer (default headless) |
| `NNK_GX_SHOT=<base>`, `NNK_GX_SHOT_EVERY=<n>` | PPM captures of both screens |
| `NNK_STEP_LIMIT=<n>` | stop after n guest instructions |

## Tests

```bash
cd nnktool && cargo test        # 47 decoder/build regression classes
SKIP_MELONDS=1 bash tools/verify_phase2a.sh   # boot-chain gate (ROM + a build)
```

`tools/prelude_audit.py` (called by `build_split.sh`) fails the build when the
compiled runtime TU is missing a prelude feature the tree carries — see
`PRELUDE_AUDIT.md`.
