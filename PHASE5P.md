# Phase-5P — the corrupted movie frames: the DECODE output is wrong, not the presentation

Follows Phase-5O (merged `ec8303f`): the run paints frames 4,200..4,648, but the
reviewer's vision check called them blocky noise.  This phase **measures where
in the pipeline the corruption first exists** and falsifies the two presentation
hypotheses the card named (VRAM-bank routing, palette/format mismatch).

**Headline**

1. **The corrupted content IS the movie path.**  Both engines hold the same
   Mobiclip container in guest RAM: `MODS` header at `0x02278268`
   (`w=0x100`, `h=0xC0`, `frames=0x525` = 1317), player object at
   `0x02278264`, and the same field layout at `0x0227829C`:

   ```
   +0x00 0x01FF8540   decode callback (ITCM)
   +0x04 <src>        current 8-bit frame buffer
   +0x08 <src2>       next frame buffer   (<src2>-<src> = 0xC010 = 256*192+16)
   +0x0C <dst>        VRAM destination    (native 0x06820000 / melDS 0x06800000)
   +0x10 0x00000200   stride (bytes) = 256 px * 2 bytes
   +0x14 0x00000100   width 256
   +0x18 0x000000C0   height 192
   ```

   So the movie decodes 256x192 **8-bit** frames (0xC010 apart, i.e. a plane,
   not YUV420) and a converter expands them into 16bpp VRAM at stride 0x200.
   The two engines agree on the whole player state (verified at native step
   2.6e9 vs melonDS `ts=2.6179e9`): the *wiring* is identical, which falsifies
   "the frame buffer is wired to the wrong VRAM bank" and "the palette/format is
   wrong in the presentation path" as the *first* divergence.

2. **The first divergence is the decoded frame itself.**  The player's current
   source buffer is `0x022AE860` in native and `0x022C0880` in melonDS (the
   6-slot ring rotates, so the slots are not frame-aligned; the ring is at
   `0x02284810, 0x0228A820, 0x0229C840, 0x022AE860, 0x022C0880, 0x022D28A0`,
   0x12010 apart).  The strict first differing word is therefore the first word
   of the plane:

   ```
   native  0x022AE860 word0 = 0x93929191   (Y=145,145,146,147)
   melDS   0x022C0880 word0 = 0x02020202   (black border, Y=2)
   identical words in the 256x192 plane: 0 / 12288 (0.0 %)
   ```

3. **What native's plane looks like.**  Rendered as a raw 8-bit plane it is a
   low-contrast, over-bright plateau field with 128x96 **quadrant seams**;
   melonDS's plane at the same structural slot is a crisp black/white image
   (the LEVEL-5 credit card).  Measured on the five live ring slots:

   | metric | native | melonDS (ts 2.6179e9) |
   |---|---|---|
   | plane mean | 183/210/166/210 per quadrant, 192-196 per slot | 34..82 per quadrant, 53-59 per slot |
   | per-quadrant variance | 650..1550 | 1900..4200 |
   | frame-to-frame MAE inside the ring | 17..27 | 39..52 |
   | seam |d| at x=128 / y=96 | 23..37 / 12..18 | 12..18 / 21..32 |

   The native planes are **flat and nearly static across six consecutive ring
   slots** while melonDS's six frames move a lot: the decoder is producing a
   plateau pattern, not successive video frames.

   It is not a "DC-only"/missing-IDCT decode: at 8x8 granularity the native
   plane has only 1.2 % constant blocks (melonDS 10..17 %, its logo card) and
   all 192 rows are distinct, i.e. the plane is *fine-grained noise*, not flat
   blocks - the corruption is in what the decoder computes inside each block
   (prediction/residual dataflow), not in a missing inverse transform.

4. **The composed screens confirm the same thing end-to-end.**  Pearson luma
   correlation of the native composed top screen (frames 4,200/4,300/4,400/
   4,500/4,600) against **all 21** melonDS composed captures in 4,100..4,700
   (`.scr` dumps, 10/50-frame granularity) is **<= 0.29**; native 4,600 is
   best-matched by melonDS 4,610 at 0.27.  Native's own composed screen
   correlates 0.44..0.47 with its own ring buffers, so the compositor faithfully
   shows the (broken) decoded plane - the corruption is upstream of the display.

5. **Correction to the Phase-5O reading.**  The buffers are *not* YUV420
   (0x12000 apart): the player's two source pointers are 0xC010 apart, i.e. one
   8-bit 256x192 plane each.  Reading them as YUV420 (as this phase first did)
   makes the U/V planes come from the *next* buffer's header, which is exactly
   the "magenta banding" artefact seen in that first render - a useful negative
   control, and the reason the "format" hypothesis looked plausible for a while.

## 1. Measurement setup (reproducible)

Reference (melonDS, `build-ptrace`):
* composed screens: `MELONDS_GXLOG_DIR` + `MELONDS_GXLOG_2DEVERY` (2 x 256x192
  u32 BGRA, post master brightness) - `f04100.scr`..`f04650.scr`, plus a
  10-frame-granular set over 4,550..4,700.
* RAM snapshots: `MELONDS_ARM9TRACE_TSLO/_TSHI` + `MELONDS_ARM9TRACE_LO=0
  HI=0xFFFFFFFF N=1` + `MELONDS_RAMDUMP_PATH` gives a one-shot 4 MiB main-RAM
  dump at a chosen master-cycle `ts` (2,600,000,000 and 2,617,900,000 here).
* the movie player's first execution: `LO=0x0207B218` -> `ts=2,272,165,619`
  (`0x0207B218` is the `blx r1` that calls the decode callback).
* `NNK_RAMDump` was extended to append the 32 KiB ITCM (local build change,
  see §4) so the ITCM blob can be inspected at a chosen `ts`.

Native: the Phase-5O image `out/split/p5o1` with
`NNK_STEP_LIMIT=2600000000 NNK_DUMP_MEM=... NNK_GX_SHOT_EVERY=100`:
`done steps=2600000001 distinct_pcs=22888 hle_hits=11202 halt=step budget exhausted`.

## 2. What is still open

The decode output is wrong, but *where* in the decoder is not yet named:

* The callback is `0x01FF8540`, guest-written ITCM code, so it is executed by
  the runtime's hand-written ITCM live re-decoder (`tools/nnk_smc.inc`), which
  is the prime suspect for a silent semantic error (each unsupported form halts
  honestly; a *mis-implemented* form decodes garbage without a halt).
* The ITCM snapshot at `ts=2.6179e9` has **data** at `0x01FF8540` (a table of
  `0x0000xxxx` halfwords), while the dump PC itself was in ITCM at
  `0x01FFDB80` - i.e. the ITCM blob is **swapped during playback** (audio vs
  video phases), so a first-execution ITCM dump cannot be used.  The next phase
  needs a PC-triggered ITCM dump (dump when the PC is the callback entry).

## 3. Result and the next barrier (named)

**Barrier I5 (named): the Mobiclip decode loop's dataflow.**  Established by
measurement: the presentation path is faithful and the player wiring matches
melonDS word for word; the 8-bit decode plane itself is a flat quadrant/plateau
pattern (`mean 192`, per-quadrant var 650..1550, 0/12288 Y words equal to
melonDS), so the corruption is produced inside the decoder callback
`0x01FF8540` (ITCM, live-re-decoded).

Next measurement, in this order:
1. PC-triggered ITCM dump at the callback entry (patch melonDS's
   `ITCMDump::Maybe` to fire on `pc == 0x01FF8540`, and add the equivalent
   native hook), then disassemble the *actual* decoder blob.
2. Enumerate the Thumb/ARM forms in that blob and audit each against
   `tools/nnk_smc.inc`'s hand-written semantics (the class that already produced
   Phase-5A/5M/5O bugs); every new class locked with a `cargo test` (suite 32).
3. Only then re-run to 2.6e9 and frame-diff the plane against melonDS's ring.

## 4. Evidence in `/home/hermes/NiNoKuni/shots/`

```
p5p_nat_f04300_top.png / p5p_mel_f04300_top.png   same frame index, both engines
p5p_nat_f04600_top.png / p5p_mel_f04600_top.png
p5p_quad_nat_vs_mel.png                          2x2 native/melonDS at 4,200 and 4,600
p5p_nat_dec8_022AE860.png / p5p_mel_dec8_022C0880.png / p5p_dec8_pair.png
                                                 the decoded 8-bit planes, side by side
```

Analysis scripts (scratch, not committed):
`p5p/{cmp,corr,yuvcmp,cross,diff,ysearch,quads,intra,firstword}.py`.

## 5. Local melonDS instrumentation change (not committed to this repo)

`/home/hermes/tools/melonDS/src/NDS.cpp` `NNK_RAMDump` now also appends the
32 KiB ARM9 ITCM (`fwrite(n->ARM9.ITCM, 1, ITCMPhysicalSize, f)`) so a `ts`-gated
snapshot carries the ITCM blob; rebuilt with
`make -j12 melonDS` in `build-ptrace`.

## 6. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 32 passed; 0 failed
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh
RESULT: PASS (rc=0) - strict prefix 1000/1000
```
