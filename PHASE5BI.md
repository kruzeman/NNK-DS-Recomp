# Phase-5BI — the "Skip the cutscene?" text: the carrier is named (OBJ sprites 5/6 from the AOBJ atlas) and the missing pixels are the GUEST's own glyph bitmap, not the upload

Follows Phase-5BF/5BF-follow-up (master `e395fb4`).  Branch `wt/t_89eef77e`.

Build (isolated split dir — see §7): `SPLIT_DIR=work/p5bi/split
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh
work/p5bi/nnk_clean`

**Result: stop-report (the card's second branch).**  One defect, named with
layer evidence and to the byte: the prompt text is drawn by **OBJ sprites 5 and
6** of engine A out of the **AOBJ atlas in VRAM bank B**, and everything on our
side — OAM, palette, VRAMCNT, bank map, tile addressing, the store path — is
faithful.  What is missing is the *bitmap itself*: the guest's glyph blitter
(`0x020303C4`) stores 60 non-zero words where melonDS's atlas has ~194 non-zero
bytes, and **we write exactly the sparse values the guest computes**.  The
defect is therefore upstream of VRAM: the guest-side glyph rasteriser.

## 1. The carrier: OBJ sprites 5 and 6, tiles 64-79 and 96-111 of AOBJ

Measured on melonDS's own GPU bytes (`tools/melds_synth_state.py` + the shipped
compositor, `tools/gx2d_probe.c`), presentation frame 6400 / engine A
(bottom screen; engine B has 0 on-screen sprites and 0 non-black pixels):

* `onlyOBJ` = 20 229 non-black pixels and contains the box, both buttons **and**
  the text; `onlyBG1` = 0; the BG1 screen map (bank A) is 128 bytes and is
  **byte-identical** to ours.  So the text is not a BG/parchment text layer.
* Leave-one-out attribution inside the text band (x 70..200, y 88..104, 219
  glyph pixels): disabling sprite **5** loses **147** of them, disabling sprite
  **6** loses **72**, every other sprite loses **0**
  (`work/p5bi/spritetext2.py`).  Sprites 5/6: `y=76 x=80` / `y=76 x=144`,
  `64x32`, `attr2=0x200C` / `0x2014` (tile 12 / 20, palette 2, prio 0).
* Tile arithmetic (DISPCNT A = `0x80211615`: bit4 = 1D mapping, bits 20-21 = 2):
  `pixelsaddr = tile << 2`, row stride 8 tiles.  Sprite 5 -> AOBJ tiles
  48-55, 56-63, **64-71, 72-79**; sprite 6 -> 80-87, 88-95, **96-103,
  104-111**.  The glyph pixels live exactly in 64-79 / 96-107 (thin 1-nibble
  strokes), which is what §2 finds missing.
* The buttons ("Yes"/"No") come from the same atlas (sprites 39-42, tiles
  264-295 via the same `<<2`) — which is why the labels render while the
  sentence does not.

## 2. What actually differs at frame 6400 (ours vs melonDS)

`tools/melds_gpu_diff.py` on our `NNK_GX_STATE_DUMP` image:

```
palette EQUAL   oam DIFF (235 vs 661 nz bytes)   vramA EQUAL   vramE..I EQUAL
vramB  ours nz=6379  melon nz=6546  differing=195  first=+0x803 +0x80A +0x80F ...
vramC/D  different picture (the known stream=2 background card)
VRAMCNT A..I = 81 82 89 91 00 85 00 00 00   (EQUAL to melonDS)
```

* The **195-byte vramB deficit is the whole text defect**: it lies entirely in
  AOBJ tiles 64-79 and 96-107 (`work/p5bi/bankdiff.py`), i.e. in the two text
  strips of §1 — every other bank-B tile, including box and button art, is
  byte-identical.
* OAM: the text sprites 5/6 (and the box/button sprites 4, 7-42) are
  **byte-identical** to melonDS's: same y/x/tile/palette/priority.  The 434
  differing OAM bytes are sprites 0-3 (an 8x8 ornament set) and sprites 43-127
  (melonDS parks them at `y=192 tile=192`, we leave zeros) — none of them
  contributes a pixel to the text.

So the carrier, the OAM entry, the palette, the bank map and the addressing are
all correct; only the tile *contents* are wrong.

## 3. The store trace proves the copy path is faithful

Cold boot to 3.6e9 with `NNK_AOBJ_TRACE=1` (`work/p5bi/run2.log`, 2 916 AOBJ
stores).  In the text window `0x06400800..0x06400DFF` there are **exactly two
writers**:

| pc | stores | non-zero | what it is |
|---|---|---|---|
| `0x02009BB4` | 768 | **0** | the `stmia` of the 32-byte fill loop at `0x02009B84` (clear the strip) |
| `0x020303C4` | 456 | **60** | the blitter that copies the rendered glyph bitmap into AOBJ |

No DMA (`0x01FF84D0`, the ITCM helper that fills the rest of the atlas with
15 040 stores) touches the text window at all — so Phase-5BF's DMA-length fix is
not involved here.

The 60 non-zero words are what our bank B holds, and they are **not** melonDS's
bytes (`work/p5bi/aobjcmp.py`):

```
step=3551219996 pc=0x020303C4 a=0x0640083C v=0x00100001  melon=01010110  differ
step=3551226791 pc=0x020303C4 a=0x0640089C v=0x00001000  melon=01101000  differ
step=3551230272 pc=0x020303C4 a=0x06400984 v=0x00000010  melon=00000000  differ
step=3551255462 pc=0x020303C4 a=0x06400B5C v=0x10000000  melon=00000010  MATCH
step=3551258502 pc=0x020303C4 a=0x06400D60 v=0x00000001  melon=01000000  MATCH
...   60 non-zero stores: 3 MATCH, 57 differ
```

The guest stores a **sparse** bitmap (1-2 nibbles per word, concentrated on the
rightmost byte column of each 4-byte row) where melonDS's atlas has the full
glyph.  Replaying those stores reproduces our bank B exactly, so nothing is
dropped between the CPU and VRAM.

## 4. Who owns the remaining defect

The guest's glyph rasteriser: it produces roughly 1/7 of the ink melonDS's
does, and the survivors sit at shifted positions, i.e. the per-pixel inner loop
of the ITCM font renderer (Phase-5AW located it at `0x01FFC684`) computes a
different bitmap than the hardware does.  That is a guest-code-emulation defect,
not a 2D/VRAM/upload one, and it is the next card's file.

Diagnostic recipe for that card (all cheap now):

```
NNK_GX_STATE_DUMP=<f> NNK_GX_STATE_DUMP_FRAME=6400 ...   # frame state (done)
NNK_AOBJ_TRACE=1                                        # per-store pc/addr/value
work/p5bi/aobjcmp.py <log> work/phase5ba/melds_gpu/vramB.bin 0x6400800 0x6400E00
DEBUG_AT_PC=0x01FFC684 ...                              # register file at the
                                                       # rasteriser entry
```
The first question for that card is why a *word* store carries only one nibble
of ink per 4 pixels (e.g. `v=0x00001000` where melonDS has `01 10 10 00`):
either the source font bitmap the renderer reads is nearly empty, or its
scale/stride arithmetic truncates.

## 5. Tests and repository state

* `sh tools/verify_phase2a.sh`: **EXIT 0** — sections A/A2/B/C/D/E/F all ran;
  the melonDS PC ground truth reproduced **1000 / 1000** (strict prefix,
  window-filtered and resync variants), 96.1 % Ghidra agreement, 96.5 %
  byte-weighted body coverage (`work/p5bi/verify.log`).
* `nnktool` class test suite: **62 passed, 0 failed** (`cargo test`,
  `work/p5bi/cargo_test.log`).
* No source change: this card is diagnostic only (`git diff` is empty apart from
  this document).  All additions are analysis scripts under `work/p5bi/`
  (never committed) plus this markdown.
* Regressions therefore untouched by construction: the intro band
  (0.9979/0.9981/0.9982), frame 6300 title art, the `NNK_EXC_TRACE` signature
  and the savestate round trips are not exercised by a diagnostic-only diff; the
  cold boot of §2/§3 was run twice (two builds, §7) with an identical frame-6400
  signature (`vramB differing=195`), no halt and the same prompt frames.

## 6. Evidence (under `work/p5bi/`, never committed)

`state6400.bin` / `state6400c.bin` (GX state dumps at frame 6400, first and
clean build — identical bank-B signature) · `run2.log` (cold boot with
`NNK_AOBJ_TRACE`, 2 916 AOBJ stores) · `run1.log` · `synth_mel.bin` +
`synth_mel_A_onlyOBJ.ppm.png` / `synth_mel_A_full.ppm.png` (melonDS's GPU bytes
through our compositor) · `grp/g_textonly*`, `grp/g_buttons*` (per-sprite
isolation) · scripts `spritetext2.py`, `oamdiff.py`, `bankdiff.py`,
`bytemod.py`, `aobjwin.py`, `aobjcmp.py`, `tilecov.py`, `tileatlas.py`,
`ppmtext.py`, `objgroup.py`, `nnks_ram.py`, `ramfind.py`, `slicein.py`.

## 7. Toolchain note (worth fixing before the next card)

`work/split/c` is **shared between worktrees** (the `work/` tree is one
directory), so a build started while another worker has different fragments in
`work/split/c` links *that worker's* runtime fragments (this card's first
runner came out as Phase-5BH's v2 state container, built from another worker's
tree).  The measurements above were reproduced with a private
`SPLIT_DIR=work/p5bi/split` build from this worktree's own `tools/*.inc`, and
both builds agree on the frame-6400 signature.  A concurrent worker's `cargo
test`/build therefore cannot corrupt results only if `SPLIT_DIR` is set.
