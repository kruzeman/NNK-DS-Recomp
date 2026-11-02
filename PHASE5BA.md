# Phase-5BA — the prompt's text/No button and the garbled background: the RENDERER is exonerated, the bytes never reached VRAM

Follows Phase-5AZ (merge `976dbef`).  Branch `wt/t_aaa3e095`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5ba/nnk_runner`
(**no runtime code changed** — see §7; nothing was rebuilt beyond that unchanged tree).

**Result: stop-report (the card's second acceptance branch).**  No renderer/VRAM
change is warranted: the defect is *not* in `gxrender`, and the card's "work from a
HEALTHY state" premise is now measured rather than assumed.

## 1. The melonDS savestate IS readable — the "v13 wall" of Phase-5AV is gone

Phase-5AV blocked on `melonds_state.mln` being savestate v13 while the local
melonDS tree expects v14.  It does not need any melonDS build at all: the
container is a documented walk (`src/Savestate.cpp`: `MELN` + 16-byte header, then
`<magic:4><len:u32><8 reserved>` sections) and the GPU section layout is
`GPU::DoSavestate` (`src/GPU.cpp`, 1.1).  New tools (committed, pure Python):

* `tools/melds_walk.py <state.mln>` — list the top-level sections.
* `tools/melds_gpu.py <state.mln> <outdir>` — extract **palette, OAM, the nine raw
  VRAM banks, VRAMCNT/VRAMSTAT and every `VRAMMap_*`** (what melonDS itself holds).
* `tools/melds_2dregs.py <state.mln>` — engine A/B `DISPCNT`, `BGCnt`, window,
  blend and master-brightness registers.
* `tools/melds_gpu_diff.py <our GX-state> <melodir>` — byte-diff the two.
* `tools/melds_synth_state.py` — splice melonDS's GPU bytes into a GX2D state
  image so **our** renderer can draw **melonDS's** machine state offline.

On the operator's `work/repro_5au/melonds_state.mln` this yields a coherent GPU
state (VCount 262, `VRAMCNT = 81 82 89 91 00 85 00 00 00`, ABG map banks A/C/D in
three 128 K slots, AOBJ = bank B, AOBJEXT = bank F).  Extracted bytes are kept in
`work/phase5ba/melds_gpu/` (not committed).

## 2. Headline: fed melonDS's own GPU bytes, OUR renderer draws the correct screen

`probe2d` (= `gx2d_render_screen`, the shipped compositor) on the synthesised
state — melonDS palette + OAM + raw VRAM banks + bank maps + 2D registers, our
rasteriser — renders **"Skip the cutscene?" in the parchment box, BOTH buttons
("Yes" with the cyan selection brackets, "No"), over the sky/village/road
background of the melonDS reference**:
`work/phase5ba/synth_melonds_gpu_prompt.png` (next to
`work/phase5ba/melonds_reference_botscreen.png`).

The same synthesised state renders `onlyOBJ` = box + both buttons + text, and
`onlyBG2` = the cutscene background.  So, called out plainly:

* `gxrender`'s tile/bitmap/OBJ paths, the OAM interpretation, the palette
  indices, the extended-palette bank, the window/blend/mosaic handling and the
  bank-map arithmetic are **all correct for this exact frame** — every previous
  "the compositor draws every tile that exists" claim is now proven against the
  emulator's own state instead of inferred.
* Therefore the *only* remaining difference between our screen and the reference
  is **what our build put into VRAM**.  The defect is in the guest/runtime
  upload path, exactly as Phase-5AV's stop-report suspected — but now quantified
  byte for byte (§3, §4).

## 3. Byte-exact diff of the prompt frame (our state vs melonDS state)

| region | ours (nonzero) | melonDS (nonzero) | verdict |
|---|---|---|---|
| palette (2048 B) | 96 | 96 | **EQUAL** |
| OAM (2048 B) | 491 | 661 | 170 bytes differ (5 sprites' attributes) |
| VRAM bank A (ABG slot 0 = BG1 map) | 128 | 128 | **EQUAL** |
| VRAM bank B (**AOBJ** = sprite tiles) | 3 186 | 6 546 | **3 371 bytes we never wrote** |
| VRAM bank C (movie frame / BG2 bitmap) | 89 994 | 85 009 | 84 423 bytes differ (different picture) |
| VRAM bank D (movie frame / BG3 bitmap) | 90 295 | 85 011 | 84 745 bytes differ (different picture) |
| banks E, F, G, H, I | 0 | 0 | **EQUAL** |
| VRAMCNT, bank maps, `BGxCNT`, `DISPCNT` mode/base bits | — | — | **EQUAL** (`DISPCNT A` differs only in *which* movie buffer is shown: ours `0x80211A15` = BG3, melonDS `0x80211615` = BG2 — the double-buffered movie swap, a one-frame offset) |

## 4. The two upload defects, characterised

**(a) AOBJ (the text and the "No" button).**  Comparing bank B tile by tile
(32-byte 16-colour tiles) shows the failure is not "some tiles are wrong" but a
**period-8 pattern of 4-tile (128-byte) blocks that were never written**:

```
tile   0-3  EQUAL      tile  4-7   ours ZERO, melon full  -> lost
tile   8-11 EQUAL      tile 12-15  ours ZERO, melon full  -> lost
tile  56,57,60 EQUAL   tile 64-71  ours 0-3 bytes, melon 7-25 -> partially lost
tile  96-99 partial    ...
```
So the box (tiles 0-3, 8-11 …) survives, while the glyph tiles and the "No"
button tiles (64-71, 96-99) are the ones that never landed — matching the visible
defect exactly (text and "No" missing, box and "Yes" present), and *this* is why
feeding melonDS's bank B makes them appear.

**(b) The background (banks C/D) is a different picture, not noise.**  melonDS's
bank C decoded as a 256×192 16-bpp bitmap is the reference's sky/village/road/
fields scene (`work/phase5ba/melonds_bankC.png`); ours is the same scene's
**outline/residual ghost** — thin white/cyan contours on flat horizontal colour
bands (`work/phase5ba/ours_bankC.png`).  That is what a frame built from motion
residuals (rather than the reconstructed frame) looks like, which is the
decode-side signature Phase-5AX named for the in-game cutscene stream
(`stream=2 codec=0x022C9F38`); the boot's `stream=0/1` intro renders correctly
(card's band corr 0.99), so this is a *per-stream* decode/upload defect, not the
renderer.

**(c) The source data is present in RAM — the loss is in the copy/render.**  The
AOBJ atlas lives at guest **0x020679D8** (found by searching RAM for a surviving
16-byte tile run).  Our RAM and melonDS's RAM agree there to **2 bytes out of
16 384**, and **10 996 of the 10 998 bytes** melonDS has in AOBJ and we lack
**do exist in our own RAM**.  So the guest's assets were read and decompressed
correctly; the tiles were lost on the way into the VRAM window (a copy/blit that
did not run, or a partially-skipped upload loop — the 128-byte block period is
the fingerprint to chase).

## 5. What could NOT be produced in this run: a healthy state AT the prompt

The cold boot chain from Phase-5AZ is healthy and reproducible (state saved at
step 2.35e9 / frame 4201, `work/repro_5ba` staging in scratch only):

* movie plays (frames 4033-4360, `movie: present` stream 0/1), A press on the
  movie → title/New-Game menu at frame ~4400 (BG3 tile mural + the "New Game" OBJ
  button, **legibly rendered**, i.e. the OBJ text engine works),
* A press on "New Game" (frames 4550-4570) → fade, then the **in-game cutscene**
  (`stream=2 codec=0x022C9F38`, frames ~5180+, bottom screen),
* an A press **during that cutscene** — the only known trigger of the "Skip the
  cutscene?" box — takes the guest to a **wild branch**: ARM trace
  `…0x02137a9e → 0x02267068` then a 6 000-instruction linear 4-byte walk through
  `0x0226f574..0x02270CDC`, which ends on `SWI #0x65` (`0xEF656C00`, an invalid
  BIOS number) and halts.  0x02267068 is **not** overlay code (overlays load at
  0x02079F80 / 0x02159FE0) but heap/scratch space, i.e. a computed branch target
  is wrong (the 5AW/5AT mode-carrier family — a Thumb caller at 0x02137A92
  entering ARM code in un-mapped RAM).
* Without pressing A the cutscene simply runs to its end and parks on a static
  screen (frame ~7200, then stable to 7500) — **the prompt never appears on its
  own**, so it cannot be reached today without fixing that branch.

## 6. Exact remaining gap (for the next card)

1. Fix the wild branch on the A press during the in-game cutscene
   (`0x02137A9E -> 0x02267068` executed as ARM; 5AZ's rule says a direct branch
   inherits the predecessor state, so either the target is reached by an
   interworking transfer whose bit0 is lost, or the caller reads a bad pointer).
2. With the prompt reachable in a healthy run, trace the VRAM writes for the
   dialog draw (`NNK_VRAM_TRACE`) and find why the **128-byte upload blocks** of
   the AOBJ atlas at 0x020679D8 are skipped — the exact 4-tile period and the RAM
   source address are in §4.
3. The background needs the `stream=2` decode (residual-vs-reconstructed frame)
   — Phase-5AX's territory, not the renderer.

## 7. Verification performed (no runtime code changed, so no regression sweep)

* `out/p5ba/nnk_runner` built from the unchanged tree; boot verified alive
  (`vrc: stores: vram=2966304 oam=2101760`, window split
  ABG=44032 BBG=458976 AOBJ=12896 BOBJ=442720 LCDC=2007680, 4470 frames).
* Determinism: a run restored from `st2350.nnks` reproduces the cold boot's
  frame-4300 PPM **byte-identically** (`cmp`), so the savestate path is faithful.
* Renderer check: `probe2d` on our operator-state dump differs from the real
  captured frame only inside the composed region it is asked to draw, and on the
  synthesised melonDS state it reproduces the reference screen (§2).
* No `verify_phase2a.sh` / `cargo test` / speed run: the tree is unmodified
  (only new tools + this document), so they would re-measure the Phase-5AZ
  baseline.  The 5AZ numbers stand.

## 8. Evidence (all under `work/phase5ba/`, never committed)

`melonds_reference_botscreen.png` (reference crop) ·
`synth_melonds_gpu_prompt.png` (our renderer on melonDS's GPU bytes = correct
screen) · `synth_melonds_obj.png` (its OBJ-only layer) ·
`our_state2000_full.png` / `our_state2000_obj.png` (our build at the same prompt)
· `melonds_bankC.png` / `ours_bankC.png` (the two movie frames) ·
`ours_bankC.bin`, `melds_gpu/` (the extracted melonDS palette/OAM/banks/maps).
