# Ni no Kuni: Shikkoku no Madoushi (NDS, B2KJ) — static recompilation

A native recompilation of the Nintendo DS game *Ni no Kuni: Shikkoku no Madoushi*
(LEVEL-5, 2010, `B2KJ`) into portable C.

A Rust recompiler (`nnktool`) decodes the game's ARM9 and every overlay, and emits
one C function per guest program counter; the generated dispatch image plus a C
runtime boots the game natively, with the DS GPU modelled in our own 2D/3D
renderer (`gxrender`). Nothing is interpreted at run time: the guest's own code is
the program.

**The card image (ROM) is the only input.** No extracted or pre-decoded assets are
needed or shipped — see below.

> No game bytes are committed to this repository: the ROM, BIOS, extracted images,
> decoded movie streams, emulator states and game screenshots all stay out of git
> (`.gitignore`).

## Current status

| milestone | state |
|---|---|
| boot → hardware init → first frame | working |
| fan-translation notice screen, Mobiclip splash | working |
| Ghibli intro movie (`data/movie/op_u.mods` + `op_d`), 1317 frames | working, bit-exact decode |
| title screen (`New Game`) | working |
| in-game cutscene (`data/movie/s01.mods`, 581 frames) | working, decoded from the ROM |
| in-game prompt *"Skip the cutscene?"* — box, text, **Yes** and **No** | working |
| answering the prompt → town map, dialogue that advances with input | working |
| sound | not implemented |
| 3D: geometry pipeline, textures, extended palette | in progress |

The run is deterministic: the same build, ROM, step budget and input script
produce identical counters and byte-identical frames. Savestates (`F5`/`F9`) are
part of the runtime and are version-tolerant across builds.

## Build

```bash
cd nnktool && cargo build --release && cd ..          # the recompiler (do this first)
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20 \
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
    bash tools/build_split.sh out/p5aj/nnk_runner
```

`nnktool` must be built **in the tree you build the runtime from**: the build
chain shells out to `target/release/nnktool` for the dispatch image, the pc→form
map, the savestate field table and the image splices, and a stale binary fails the
splices with `expected '…', recompiler emitted '…'`.

The build is guarded by `tools/prelude_audit.py`: every runtime feature the tree
carries must have a marker row in `tools/nnk_prelude_manifest.txt`, and a missing
feature fails the build instead of silently producing a binary without it (see
`PRELUDE_AUDIT.md`).

## Run

```bash
./run.sh "/path/Ni No Kuni.nds"                    # SDL2 window, F5 save / F9 load
NNK_GX=headless ./nnk_runner "/path/Ni No Kuni.nds" # no window (reproducible)
```

At start-up the runtime derives everything in memory from the card image:

| derived | from the ROM |
|---|---|
| guest image 445,304 B @ `0x02000000` | ARM9 slice `0x4000+0x49328`, zero-padded |
| decompressed ARM9 | the DS compression of that slice |
| overlay arena window @ `0x020C0000` | overlay 17 (OVT + FAT), decompressed |
| movie frames | `data/movie/*.mods`, decoded in-process (Mobiclip HLE) |
| files the guest opens | the ROM's Nitro filesystem (FNT/FAT) |

### Controls (window)

Arrows = D-pad · `X` = A · `Z` = B · `S`/`A` = X/Y · `Enter` = Start · `Shift` = Select · `Q`/`W` = L/R.

### Scripted runs

```bash
NNK_INPUT_FILE=script.nnk NNK_STEP_LIMIT=4300000000 ./nnk_runner "Ni No Kuni.nds"
```

`script.nnk` is `<display frame> <KEYINPUT mask>` per line (`0x3FF` = nothing held,
active-low), terminated by `end`; a press must be held for **≥ 6 frames** (the guest
polls `KEYINPUT` once per frame). `NNK_INPUT_RECORD=out.nnk` records a live session.

## Performance

Headless, `/usr/bin/time`, two runs each, 24-core host:

| steps | time | rate |
|---|---|---|
| 200 M | 9.1 s | ~22 M steps/s |
| 600 M | 24.5 s | ~24 M steps/s |

The per-step DMA/vblank/timer/IPC tick is **event-driven** (Phase-5BN): the runtime
computes the exact next cycle at which anything can change and runs the tick only
then — 200 M steps execute ~450 k ticks instead of 200 M, which is what the 2× over
the previous build comes from. Debug probes are compiled in but skipped unless
`NNK_PROBES=1` (another ~9%).

## Tests

```bash
cd nnktool && cargo test                      # 65 decoder/build regression classes
SKIP_MELONDS=1 bash tools/verify_phase2a.sh   # boot-chain trace gate: 1000/1000 PCs vs melonDS
```

## Debug overrides (never requirements)

| variable | what it does |
|---|---|
| `NNK_STEP_LIMIT=<n>` | stop after n guest instructions |
| `NNK_GX=window` / `headless` | SDL2 window vs headless renderer (default headless) |
| `NNK_GX_SHOT=<base>`, `NNK_GX_SHOT_EVERY=<n>` | PPM captures of both screens |
| `NNK_GX_STATE_DUMP=1` | dump DISPCNT/BGxCNT/OAM/VRAMCNT at exit |
| `NNK_INPUT_FILE`, `NNK_INPUT_RECORD`, `NNK_INPUT_TRACE` | scripted input / record / trace |
| `NNK_SAVESTATE=<file>`, `NNK_SAVESTATE_SAVE[_AT]`, `NNK_SAVESTATE_STRICT`, `NNK_STATE_HASH_AT` | savestates and determinism hashes |
| `NNK_TRACE_FROM`, `NNK_SMC_TRACE`, `NNK_EXC_TRACE`, `NNK_DEBUG_AT_PC`, `NNK_VRAM_TRACE`, `NNK_AOBJ_TRACE`, `NNK_DMA_TRACE`, `NNK_MOVIE_TRACE`, `NNK_PROBES` | tracing and probes |
| `NNK_DMA_LEGACY_SIZE=1` | A/B the pre-5BF DMA transfer-size bit |
| `NNK_SMC_IMAGE`, `NNK_ROM_PATH`, `NNK_MOVIE_YUV*`, `NNK_MOVIE_MODS*` | A/B overrides for review only |

## Layout

```
nnktool/     the Rust recompiler: decode, emit, state map, pc->form map, tests
tools/       build chain, runtime prelude (*.inc), splices, verifiers, savestate tools
gxrender/    the DS GPU model: 2D layers, GX FIFO, 3D, SDL2 presentation
PHASE*.md    the engineering log, one file per investigation (frontier, evidence, fixes)
ROM_MAP.md   memory map and the game's own module layout
```

## Notes on method

Each `PHASE*.md` is a self-contained record: what was measured, how, what the fix
was, and which gates passed. The rules that came out of them (instruction-state
witnesses for exception returns, `blx` and taken branches; the ARM9 vs ARM7 DMA
field layouts; movie containers bound by identity through the Nitro filesystem;
savestate regions identified by name and version) are enforced by regression classes
in `nnktool/src/decoder_regression_tests.rs`, so a fix cannot be quietly lost.
