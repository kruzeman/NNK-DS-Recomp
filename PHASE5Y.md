# Phase-5Y — the first screen (gambling notice) renders: VRAMCNT_H/VRAMCNT_I are at 0x04000248/0x04000249, 0x04000247 is WRAMCNT

Follows Phase-5V (merged `9b620ab`).  Scope: the very first screen — the
bordered notice panel with the white pixel-art pirate, the "We do not condone
piracy" caption and the "Tap to continue" prompt — rendered black except a
partial "Tap to" in native output.

**Headline (measured, not inferred):**

1. **The fade was never the bug** (operator, mid-run): boot goes white, then
   master-brightness fades to dark, then the notice lands.  Native reproduces
   white -> fade -> dark exactly (`shots/p5y/a.*.bot.ppm`: luma 255.00 at
   frame 30..80, 207/125/44 at 85/90/95, 0 at 100; content appears at frame
   **205**, i.e. after the fade).
2. **The content layer was drawing black because engine B's BG *extended
   palette* bank was never mapped.**  DISPCNT_B = `0xC0211210` enables BG
   extended palettes (bit 30) and BG1 (`0x0087`, 256-colour text BG, char base
   0x4000, map base 0x0000) is the layer that carries the panel, the figure and
   the caption: its 32x32 map holds 767 non-zero tile entries and its tile block
   holds 11008 non-zero bytes in *both* engines' state, and the B-BG palette RAM
   (0x05000400) is legitimately all zero (BG ext pal is in use, not palette RAM).
   With the ext-palette bank un-mapped, every BG1 pixel resolves to colour 0 =
   black.  The prompt survived because it is **OBJ** (12 sprites, y=168/184,
   B-OBJ palette has 4 colours).
3. **The first differing word (native vs melonDS, same frame) is VRAMCNT_H and
   VRAMCNT_I.**  melonDS `f00240.2d` at the notice: `H=0x82, I=0x83`; native
   `state240.bin`: `H=0x03, I=0x82`.  Class: **the VRAMCNT register file
   layout is not a flat `0x04000240 + bank` table.**  melonDS
   `NDS.cpp ARM9Write8/16/32`:
   `0x240..0x246 = VRAMCNT_A..G`, **`0x247 = WRAMCNT`** (not a VRAM bank),
   **`0x248 = VRAMCNT_H`**, **`0x249 = VRAMCNT_I`**.  Our runtime model
   (`tools/nnk_vramcnt.inc`, carried by the frozen dispatch image, and the same
   C emitted by `nnktool/src/recompile.rs`) plus `gxrender`'s decode all used
   `bank = addr - 0x04000240`, so the guest's WRAMCNT store (0x03) overwrote
   VRAMCNT_H, the real H store (0x82) landed on **I**, and the real I store
   (0x83 = enabled, mst 3 = *engine-B BG extended palette*) was dropped on the
   floor.  Measured guest sequence (`NNK_VRC_TRACE=1`, 140 M steps):

    ```
    vrc: W1 0x04000243=0x00000084 A..I=810084840000000382 step=40768356   ; D = BBG
    vrc: W1 0x04000249=0x00000083 A..I=810084840000000382 step=40768411   ; I = BG ext pal (LOST)
    vrc: W1 0x04000248=0x00000080 A..I=810084840000000380 step=41917963
    vrc: W1 0x04000248=0x00000082 A..I=810084840000000382 step=41918152   ; H -> our bank I
    ```
   After the fix the same run reports
   `H=82->BBGEXT[0..3] I=83->BOBJEXT` (bank-map print) and the frame renders.

## 1. Evidence chain

* Native capture, post-fix (`shots/p5y/c.00245.bot.ppm`, 140 M steps = 256
  presented frames): the notice, faded, 5 colours, 7.39 % non-dominant pixels,
  dominant 0x181818 — melonDS `f00240.scr` buffer 1 is 4 colours, 7.68 %,
  dominant 0x181818.  Reference content confirmed against melonDS's own screen
  dump decoded to `shots/p5y/mel_f240_bot.png` (panel + pirate + caption +
  prompt), which is the same content as the operator's `shots/ref_tap_notice.jpg`.
* Pre-fix the same frame was 3 colours / 5.86 % (prompt only).
* Full state diff (`state240.bin` vs reconstructed melonDS `f00240.2d`) after
  the fix: **BBG flat, bgextpal, abgextpal, objextpal, palette RAM, DISPCNT/BGxCNT
  and BG scroll all byte-identical**; the residue is listed in §3.

## 2. Fix

* `gxrender/src/gx_display.c` — `global_write` decodes the VRAMCNT block by
  *slot* (`{A..G, WRAMCNT, H, I}`), width-correct for 8/16/32-bit stores
  (melonDS ARM9Write8/16/32 pairings), keeps WRAMCNT in its own field, and no
  longer aliases it onto a VRAM bank.
* `tools/nnk_vramcnt.inc` — `nnk_vrc_slot()` + width-correct write and readback
  (the frozen p5u image carries this file, so no image regeneration is needed);
  `nnk_vrc_read` now serves the same slots, so a 16-bit read of 0x04000248
  returns `H | I<<8` like melonDS.
* `nnktool/src/recompile.rs` — the generator emits the same `vrc_slot()` model,
  so a regenerated image keeps the fix.
* New instrumentation kept behind env vars (no cost when unset):
  `NNK_GX_STATE_DUMP=<path> NNK_GX_STATE_DUMP_FRAME=<n>` writes the exact GX2D
  state image the compositor consumes (plus the nine raw VRAM banks) so a frame
  can be diffed against melonDS's `f%05u.2d` offline; `NNK_VRAM_TRACE=1` logs
  VRAM window stores and counts dropped ones; `NNK_VRC_TRACE` logs the bank file.
* `tools/side_by_side.py` — stdlib 2-up PPM composer for native-vs-reference
  images.

## 3. Measured residual (out of this task's scope — belongs to Phase-5X)

State diff at the notice frame after the fix (native `state240b.bin` vs
melonDS `f00240.2d`):

```
-- BOBJ flat: 1 range, 704 bytes  first offsets: ['0x002c0..0x00580']
-- VRAM_A..D raw: 1 range, 704 bytes  first offsets: ['0x602c0..0x60580']   (bank D)
-- OAM: 232 single-byte differences (melonDS hides 116 sprites with y=192/tile=192, native leaves 0)
-- engB BLDALPHA: nat EVA=0x0e EVB=0x02, mel EVA=0x0d EVB=0x03
```

The visible consequence is exactly the operator's "Tap to" truncation: native's
prompt sprites stop after tile 10 while melonDS has glyph tiles up to ~21.  The
upload is a guest-side copy whose length/content comes from data native serves
differently — the prime suspect is Phase-5X's measured gap that **guest reads of
VRAM/OAM return 0** (our `ld*`/`gxd_io_read` never consult the VRAM windows), so
a blitter that reads back what it staged, or copies VRAM->VRAM/OAM, produces a
truncated/zeroed result.  This is 5X's file (`nnktool/src/recompile.rs` read
shadow) and was deliberately **not** duplicated here.

Also unchanged and unrelated: `AOBJ=0` in the operator's run is *correct* — the
top screen (engine A) is black in the reference too (engine A has no BG and no
OBJ enabled at this point).

## 4. Tests

`nnktool` suite **35** (was 34).  New class 21
(`vramcnt_h_and_i_are_at_0x248_and_0x249_with_wramcnt_at_0x247`) locks: the
measured guest stores reproduce `H=0x82, I=0x83, WRAMCNT=0x03`; `0x246` is
VRAMCNT_G and `0x247` is never a VRAM bank; all three implementations carry the
slot table and the flat `a - 0x04000240` model is gone; `gx_display.c` routes
0x248/0x249.

## 5. Acceptance

* `sh tools/verify_phase2a.sh` on this branch: every section (A, A2, B, C, C2,
  D, E, F) ran to completion — recovered/Ghidra agreement 1942/2020 (96.1 %),
  body coverage 96.5 % byte-weighted, mono recompile+`cc -O2`+run OK, and the
  melonDS PC ground truth reproduced **1000 / 1000** (strict prefix, windowed
  and post-resync).
* `cargo test --release` in `nnktool`: **35 passed** (34 + class 21).
* Native notice frame post-fix:
  `shots/p5y_firstscreen_native_bot.png` (256x192) and the 2-up
  `shots/p5y_firstscreen_nat_vs_mel.png` (native | melonDS `f00240.scr`).
  Notice window in display frames: **205..255+** (fade ends at frame ~100;
  the splash is far later, frame ~4000).
