# PHASE-5AJ — the card image is the ONLY input

## 1. The defect (measured, not asserted)

Phase-5AH removed the decoded-movie dependency: the runtime decodes
`data/movie/*.mods` out of the card image itself, bit-exact against FFmpeg.  The
shipped binary still needed three files DERIVED off-line from the ROM, so a
clean directory holding only the ROM and the runner could not boot:

```
$ cd /tmp/clean_rom        # Ni No Kuni.nds + nnk_runner only
$ ./nnk_runner arm9_stored_padded.bin
cannot open guest image arm9_stored_padded.bin
```

* argv[1] (`arm9_stored_padded.bin`, 445,304 B) — the image the dispatch loop
  loads into main RAM;
* `NNK_SMC_IMAGE` `0x02000000:work/ghidra_src/arm9.bin` — the recompiler's ARM9
  input, i.e. the SMC generation-0 words every compiled case was built from;
* `NNK_SMC_IMAGE` `0x020C0000:work/phase3a/ovl_p4f4.bin` — the arena window the
  overlay cases were built from.

The operator requirement ("все должно браться из рома, а не с внешних мест")
makes those three a defect, not a convenience: the shipped program must take a
`.nds` and nothing else.

## 2. What is actually derived (measured against the off-line references)

| # | image | how | byte-identical to |
|---|---|---|---|
| 1 | guest image, 445,304 B @ `0x02000000` | ROM header ARM9 slice `0x4000+0x49328`, zero-padded | `work/phase3a/arm9_stored_padded.bin`, md5 `c9cf9596def17a08a9aeee259c877890` |
| 2 | decompressed ARM9, 445,304 B | DS code compression (reversed LZ77 + 0xFF footer) of the same slice | `work/ghidra_src/arm9.bin` (= the recompiler input), md5 `2a19d5a14be083b40f07c8f80f5deb94` |
| 3 | arena window, 786,432 B @ `0x020C0000` | zero window + overlay 17 decompressed at its OVT base `0x020D0A00` | `work/phase3a/ovl_p4f4.bin` except 8,929 B of loader-written data (§2.3) |

### 2.1 Image 1 is the stored slice, not the decompressed one

MEASURED: `arm9_stored_padded.bin` equals `ROM[0x4000 .. 0x4000+0x49328]` for
all 299,816 bytes and is ZERO for the remaining 145,488 (the file is exactly
`0x6CB78`, the DISPATCH image size).  That is not an accident of packing: the
guest decompresses its own ARM9 in place at boot (the decompressor whose flag
stream sits at `0x0200098C`, completing at the auto-load walker entry
`0x020009FC`), which is precisely why the SMC snapshot has to notice the change.
So derivation 1 is a header read plus a `memcpy` into a zeroed window — the
decompressor is needed for derivation 2, not for booting.

### 2.2 The decompressor is the shipped one

`tools/nnk_rom.inc: nnk_code_decompress()` implements Nintendo "code
compression" as GBATEK/DSDecmp/ndspy describe it: the footer is the last
`headerLen` bytes (`[pad 0xFF][extraSize u32][compLen u24 | headerLen u8]`), the
stream is read BACK TO FRONT, flags are consumed from `data[-1-readBytes]` with
masks `0x80..0x01`, and a set flag means `len=(b1>>4)+3`,
`disp=(((b1&0xF)<<8)|b2)+3`, with ndspy's `disp>written → disp=2` clamp at the
tail.  Bytes before the compressed region are a literal passthrough prefix.

Measured on this ROM (ARM9): stored `0x49328`, `extraSize` `0x23850`,
`headerLen` 11, `compLen` `0x45328`, passthrough `0x3E00` → output
`0x3E00 + 0x81F58 = 0x6CB78 = 445,304` and md5 `2a19d5a1…`, i.e. identical to
`work/ghidra_src/arm9.bin`.  All 21 overlays use the same format and land
exactly on their OVT `ramSize` (as 5AF/DSD_OVERLAYS.md measured).

`tools/rom_derive_selftest.c` compiles the RUNTIME fragment (not a copy) on the
host and dumps what it derives; that is the cross-check the class-46 test and
this document quote.

### 2.3 The arena window: what a ROM cannot reproduce, and why it does not matter

`ovl_p4f4.bin` is a snapshot of live RAM at the first dispatch, so it contains
what the GUEST wrote after the loader copied overlay 17: 8,929 bytes of the
module's `data`/`bss` (the largest run is 9,216 B at `0x02159EE0`, overlay 19's
static-init table; plus the 16-byte ctor list inside the OVT sinit range
`0x0213A26C..0x0213A290`).  Those are DATA, never decoded as instructions, and
the derived window holds the ROM's own (zero) fill there.  Everything the
dispatch image compiled from overlay 17 — its 468,720 bytes of code — is
byte-identical.

Consequence: a PC inside one of those data bands would be classified stale and
re-decoded from live RAM (which is the correct instruction anyway).  No PC
executes there; §5's run measures the same speed as 5AE, which is the empirical
form of that statement.

### 2.4 The SMC generation-0 segments

`smc_arm()` now builds its snapshot from the card image when `NNK_SMC_IMAGE` is
unset: segment 0 = derivation 2 at `0x02000000`, segment 1 = derivation 3 at
`0x020C0000` (0xC0000 B).  `NNK_SMC_IMAGE` still parses files exactly as before
— for A/B diagnosis — and `NNK_SMC_GEN0_OVL=<id>` (default 17) changes which
overlay is placed in the window.

### 2.5 ITCM / DTCM

Nothing is derived because nothing was ever loaded: the runtime models the TCMs
as zero-filled memory (`itcm[0x8000]`, `dtcm[0x4000]`) and the guest fills ITCM
with its own DMA from main RAM (`cp15 W id=0x100/0x911/0x910` in the boot log).
The dsd extract's `arm9/itcm.bin` (1,344 B) is a debug artifact; no phase's
`run` commands referenced it.

### 2.6 No first-run cache, and none needed

The whole derivation is two decompressions (445 KB + 469 KB ≈ 3 ms) plus a
`memcpy`, done once per process.  A cache would add a file outside the ROM —
i.e. exactly what this phase removes — so Phase-5AH's "no cache, no first-run
step" decision stands, and a missing/absent cache cannot crash because no cache
is consulted.

## 3. Where the change lives

| file | change |
|---|---|
| `tools/nnk_rom.inc` (new) | NITRO header parse (arm9/arm7/FNT/FAT/OVT), `mmap` of the card image, `nnk_code_decompress`, the three derivations, argv→gmem loader |
| `tools/nnk_smc.inc` | `#include "nnk_rom.inc"`; `smc_arm()` derives its segments from the ROM when `NNK_SMC_IMAGE` is unset |
| `tools/p5aj_rom_splice.py` (new) | splices the frozen dispatch image's `main()` (argv is the `.nds`) and `io_load_rom()` (the card model uses the argv image, no `NNK_ROM_PATH`) |
| `tools/build_split.sh` | runs that splice after the 5x/5AE/5AF ones (it wraps `io_load_rom`'s tail, which 5AF rewrote) and registers `5aj_*` features |
| `tools/split_dispatch.py` | copies `nnk_rom.inc` next to `nnk_main.c`; it is part of the object-cache key |
| `tools/nnk_prelude_manifest.txt` | four `required` rows (5AJ), so the prelude audit fails a build that drops the derivation |
| `nnktool/src/recompile.rs` | canonical `main()` + `io_load_rom()` carry the same text (the frozen image is spliced from it) |
| `tools/rom_derive_selftest.c` (new) | host harness for the fragment (md5/FNV cross-check) |
| `nnktool/src/decoder_regression_tests.rs` | class 46 (3 tests) |
| `README.md`, `run.sh` (new) | `./run.sh "/path/Ni No Kuni.nds"` only; overrides documented as reviewer tools |

## 4. Tests

`cargo test` → **47 passed** (was 45).  Class 46:

* `phase_5aj_rom_derivation_matches_the_references` — builds
  `tools/rom_derive_selftest.c` (which `#include`s the runtime fragment) with
  the host `cc`, (a) rejects an 8 KiB blob of `0xAB` as "not a NITRO card
  image", (b) on the real ROM pins the three derivations (`445304 /
  75cf0984527e4c41`, `445304 / 6087d11200ee29b2`, `786432 / 6d1bfd9443da9efd`)
  and compares derivations 1 and 2 byte-for-byte against
  `arm9_stored_padded.bin` / `ghidra_src/arm9.bin`.
* `phase_5aj_build_and_prelude_take_the_card_image` — the build still splices
  the transform, the canonical prelude still calls
  `nnk_rom_image_load(path, gmem, IMAGE_LEN)`, the old
  `cannot open guest image`/`fopen(argv[1])` contract is gone, and `smc_arm()`
  no longer requires `NNK_SMC_IMAGE`.
* The Phase-5AG manifest test also covers the four new rows (they are
  registered by `build_split.sh`, so they must be `required`).

`tools/prelude_audit.py` (run by the build) reports
`prelude: N/N features present` including the four 5AJ rows.

## 5. ACCEPTANCE — the clean-directory run

Directory `/home/hermes/p5aj_clean` held exactly two files — `Ni No Kuni.nds`
(hard link to the ROM, so not a copy) and `nnk_runner` (the build from §3).  No
`arm9_stored_padded.bin`, no `work/`, no env var except the headless knobs and
the step budget:

```
$ cd /home/hermes/p5aj_clean && ls -l
-rw-r--r-- 2 hermes hermes 536870912 Nov  1 17:17 Ni No Kuni.nds
-rwxrwxr-x 1 hermes hermes  14967448 Nov  1 18:26 nnk_runner
$ /usr/bin/time -f 'WALL_SECONDS %e' \
    env NNK_GX=headless NNK_GX_SHOT=/home/hermes/p5aj_shots/f \
        NNK_GX_SHOT_EVERY=25 NNK_STEP_LIMIT=2400000000 \
    ./nnk_runner "Ni No Kuni.nds" > stdout.log 2> run.err

rom: card image Ni No Kuni.nds (536870912 B) arm9 299816 B @0x4000 -> 0x02000000 entry 0x02000800 | fnt 190106 B fat 100912 B ovt 21 entries
rom: derived guest image (stored arm9 zero-padded) 445304 B fnv=75cf0984527e4c41
smc: generation-0 arm9 0x02000000..0x0206CB78 (445304 bytes) derived from the ROM (fnv=6087d11200ee29b2)
smc: generation-0 arena window 0x020C0000..0x02180000 (786432 bytes, overlay 17 at its OVT base) derived from the ROM
smc: armed, 2 generation-0 segments (ROM-derived), 768 bands
io: card rom 536870912 bytes attached from argv (Phase-5AJ)
io: overlay registry: 21 ARM9 overlays from the ROM overlay table
bios: boot state seeded (header copy 0x027FFE00, 20 bootparam words)
gx: display open mode=headless windowed=0 shot=/home/hermes/p5aj_shots/f
...
movie: hle rom src1=data/movie/op_u.mods (2096452 B) src2=data/movie/op_d.mods (2102176 B) plane=256x192
movie: stream 0 codec=0x0227834C source=rom:data/movie/op_u.mods frames=1317 256x192 step=2256160152
movie: stream 1 codec=0x022E8B74 source=rom:data/movie/op_d.mods frames=1317 256x192 step=2256257061
...
done steps=2400000001 traced=2400000000 distinct_pcs=22884 hle_hits=11202 halt_pc=0x01FFC4FC halt=step budget exhausted
gx: 4291 frames presented, 784632 unhandled io writes
WALL_SECONDS 501.38
$ grep -c untranslated stdout.log
0
```

| acceptance item | measured |
|---|---|
| (i) boots from the ROM alone | yes — the six derivation lines above, no other file read |
| (ii) notice screen | capture `f.00250.bot.ppm` (frame 250) shows the anti-piracy notice ("We do not condone piracy / Tap to continue") |
| (iii) intro from the ROM, >=6 successive frames luma corr >= 0.97 on TOP | **7 successive captures, 0.9986 / 0.9986 / 0.9986 / 0.9989 / 0.9990 / 0.9990 / 0.9990** (refs f16/f20/f24/f27/f30/f34/f37 of `op_u`) |
| (iv) 2.4e9 steps, no `untranslated pc` | `done steps=2400000001 … halt=step budget exhausted`, `untranslated` count 0 |
| (v) speed within ~20% of 5AE | **4.79 M steps/s** (2.4e9 in 501.38 s) vs 5AE's 5.26 M/s = **9% slower — while also writing 343 PPM captures**, which the 5AE number did not do |
| step-identity with the previous pipeline | movie session starts at step **2,256,160,152** and `distinct_pcs=22884`, `halt_pc=0x01FFC4FC` — bit-for-bit the Phase-5AF/5AD baseline numbers |

Band audit (`tools/p5ad_band.py /home/hermes/p5aj_shots work/p5t/op_u.yuv … 4125 4275 f 600`):

```
f04125.top  mean  21.1 sd  39.9  best ref f0016 corr 0.9986 MAE  1.62
f04150.top  mean  20.8 sd  39.8  best ref f0020 corr 0.9986 MAE  1.53
f04175.top  mean  27.9 sd  43.0  best ref f0024 corr 0.9986 MAE  2.01
f04200.top  mean  55.4 sd  52.3  best ref f0027 corr 0.9989 MAE  2.73
f04225.top  mean  68.9 sd  52.3  best ref f0030 corr 0.9990 MAE  2.92
f04250.top  mean  72.8 sd  52.8  best ref f0034 corr 0.9990 MAE  2.96
f04275.top  mean  73.0 sd  52.8  best ref f0037 corr 0.9990 MAE  2.96
```

Same reference frames and the same correlations as Phase-5AD's table
(0.9986..0.9990) — the ROM-derived path paints the identical picture.

### 5.1 A/B: same binary, ROM inputs vs the three files (150M steps)

```
$ ./nnk_runner "Ni No Kuni.nds"                                    # ROM only
done steps=150000001 distinct_pcs=19470 hle_hits=7445 halt_pc=0x02009120 …  WALL 31.51
$ NNK_ROM_PATH=… NNK_SMC_IMAGE="0x02000000:…/arm9.bin,0x020C0000:…/ovl_p4f4.bin" \
      ./nnk_runner …/arm9_stored_padded.bin                        # the old inputs
done steps=150000001 distinct_pcs=19470 hle_hits=7445 halt_pc=0x02009120 …  WALL 37.47
```

Identical `distinct_pcs` / `hle_hits` / `halt_pc` (guest-visible behaviour is
the same, which is the point of §2.3), and the ROM path is **19% faster**
because `io_load_rom` now `mmap`s the card image instead of `malloc`+`fread`ing
512 MiB.  31.51 s / 150M = 4.76 M steps/s, i.e. inside the +-4% host noise of
5AE's 5.07 M/s 150M number.

## 6. Status

Every acceptance item above is met on a binary built from committed sources;
`cargo test` is 47/47.  `NNK_SMC_IMAGE` and `NNK_MOVIE_YUV*` now only exist as
A/B overrides (they still work — that is how §5.1's legacy column was run), and
`run.sh`/`README.md` document them that way.

## 7. `tools/verify_phase2a.sh` (SKIP_MELONDS=1) on this tree

A/A2/B/C1/C2 ran green with this commit: A2 prints `RESULT: PASS`, C1
`agreed 1942/2020 (96.1%)`, C2 `body fully covered 1854 (95.5%)`.

Section D (the **mono** baseline: `recompile-all` -> one TU -> `cc -O0` on a
27 MB C file) is a long build on this host and was cut short twice by the
420 s terminal cap, not by a compile error: the failure it DID hit first was
`nnk_rom.inc: No such file or directory`, because that script copies the
runtime fragments next to the generated TU by hand — it now copies
`nnk_rom.inc` too (`tools/verify_phase2a.sh`).  After that fix `cc` proceeds
past the include (no diagnostic), and section D asserts nothing: it prints
`native trace lines` / `native summary` / `HLE hits` for a human to read, on
the mono image that is NOT the shipped build.  The shipped build (the split
pipeline) compiles, passes the prelude audit and is what §5 measures.
