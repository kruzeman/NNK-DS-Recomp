# Phase-5BC — the "Skip the cutscene?" prompt: the skip key is START, not A

Follows Phase-5BB (master `5c46d6d`).  Branch `wt/t_5de4448b`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5bc/nnk_runner`
(**no runtime code changed** — this phase is measurement; the tree is unmodified
apart from this document and one new offline tool).

**Result: the prompt is reached on a healthy state and the trigger is named with
measurements: it is START (KEYINPUT bit 3), not A.**  With START held on frames
6170-6189 of the in-game cutscene, the guest uploads bank-B (AOBJ) tiles for the
first time in the whole session and draws the parchment box with the **"Yes"**
button — the missing text and "No" are the Phase-5BA AOBJ upload defect, as
predicted, and are not touched here.  A held at the same frame is *completely
inert*: identical pc set, identical decode count, **zero** AOBJ stores.

## 1. The card's premise is wrong, and it is measured, not argued

Same state (`work/repro_5bb/st3400.nnks`, step 3.4e9, display frame 6076, inside
the in-game cutscene), same window, one key:

| press (held 20 frames at 6170) | distinct_pcs | halt_pc | VRAM stores | AOBJ stores | OAM stores |
|---|---|---|---|---|---|
| none (control) | 25 426 | `0x0207CBDC` | 245 760 | **0** | 36 864 |
| **A** (`0x3FE`) | **25 426** | **`0x0207CBDC`** | **245 760** | **0** | **36 864** |
| **START** (`0x3F7`) | **26 151** | `0x02007134` | 296 274 | **1 362** | 82 432 |

A and the control are *indistinguishable* — same pcs, same hle_hits (13 075),
same store windows, same store counts; the `mem_A`/`mem_D` 4 MB RAM dumps differ
only inside the key state itself.  START changes control flow **and** produces
the first bank-B (sprite) upload of the run.

The A presses the 5BB card was built on (6100/6200/6300) are the same non-event;
this phase pressed A at 6170 as well to close the "maybe the timing was off"
hole.

## 2. Why A can never do it: the cutscene's key handler gets masks of zero

* The in-game cutscene is **not** played by the intro loop `0x0207CF18` (whose
  `and r1,r0,#9` A|Start exit test the card suspected): that code is never
  dispatched in this run (0 hits at `0x0207D230`), and the decode callback
  proves the real path.  `NNK_DEBUG_AT_PC=0x01FF8540` (the decode callback)
  gives `r14=0x0207B21C r5=0x022C9E50` — i.e. the callback is entered from the
  ov13 **cutscene playback loop `0x0207C678`**, via `0x0207C898`
  (`bl 0x207a744` -> `bl 0x207b1d0`) and the present at `0x0207C7DC`.
  The loop's per-frame decode/present/yield body is `0x0207C77C..0x0207C83C`.
* That loop's key handler is `0x0207C3A0(obj, mask1, mask2, mask3)`
  (`ldrh [0x04000130] | ldrh [0x02FFFFA8]`, active-low decode, edge against
  `[0x0208AFA0+2]`, `tst mask1, edge` -> `return 1` = leave the loop).
  The three masks are `ldrh` of **obj+0x40E8 / +0x40EA / +0x40EC**, written once
  by the "start movie" call `0x0207DC38` from the **stacked arguments** of the
  movie request (`ldrh r5,[sp,#36]`, `r3,[sp,#40]`, `r2,[sp,#44]`).
* Measured live (`NNK_DEBUG_AT_PC=0x0207C3A0`, 28 iterations over
  3.4032e9..3.4789e9 = the whole movie): **`r1=r2=r3=0` in every iteration**.
  With `mask1 = 0` the edge test at `0x0207C47C` can never return 1, so no key —
  A included — can leave the cutscene player.
* The melonDS prompt state holds the **same zeros** in the same object
  (`0x0228AE00+0x40E8..EC`, read out of `work/repro_5au/melonds_state.mln`), so
  this is not a divergence: the cutscene player's own key path is dead in both
  engines, and the reference's skip tap was consumed by a **different layer** —
  the scene/UI that draws the dialog (the movie keeps decoding: `hle_hits`
  stays 13 075 with START).

## 3. The trigger, measured

`work/repro_5bc/start.nnk`:

```
6100 0x3FF
6170 0x3F7      # START (KEYINPUT bit 3, active-low)
6190 0x3FF
```

`NNK_SAVESTATE=work/repro_5bb/st3400.nnks NNK_SAVESTATE_IGNORE_BUILD=1
NNK_INPUT_FILE=work/repro_5bc/start.nnk NNK_GX=headless NNK_VRAM_TRACE=1
NNK_STEP_LIMIT=3456000000` writes `work/repro_5bc/healthy_prompt.nnks`
(step 3.455e9, frame 6174, `VRAM window split: ABG=294912 AOBJ=1362`,
`stores: vram=296274 oam=51200`).

The frame at 6180 (`work/repro_5bc/s2/g.06180.bot.ppm.png`) and the frame
re-rendered from that savestate itself (`work/repro_5bc/s4/k.06178.bot.ppm.png`)
both show: parchment box, brown border, **"Yes"** button with the cyan selection
brackets — the reference's prompt.  The savestate was re-loaded in a second
process (`state: loaded ... identity OK ... restored step=3454999999
frame=6174`) and renders the same prompt, so it is a usable hand-off.

The trigger is **not** tied to a movie position: a second run with START at
display frame **6200** (a different point of the same film, `start6200.nnk`)
gives the identical signature — `AOBJ=1362`, `distinct_pcs=26151`,
`halt_pc=0x02007138`, `vrc: writes=378 vram=296274` — while A at the same frames
gives `AOBJ=0 / distinct_pcs=25426`.  So the only thing that ever mattered was
the key.  (The `hle_hits` count, 13 057 vs 13 075, is the movie clock's response
to the extra UI frames; the decode path is unchanged.)

## 4. What still differs from the melonDS reference (quantified, not fixed)

1. **Text and the "No" button** — the Phase-5BA AOBJ upload defect.  Our frame
   has the box and "Yes" only; melonDS's bank B holds 6 546 nonzero bytes vs our
   3 186, i.e. 3 371 bytes never written, in the documented period-8 / 4-tile
   (128-byte) blocks (glyph tiles 64-71, 96-99 and the "No" button).  The
   prompt now *exists*, so the next card can work on exactly this.
2. **The film itself is the wrong one.**  The guest is playing
   `data/movie/s01.mods`, but our HLE feeds it `op_u.mods`.  Proof from the
   guest's own player struct at `0x022C9E50` in our own RAM *and* in the melonDS
   state: `+0x04 "MODS"`, `+0x0C = 0x245 = 581` frames, `+0x10/0x14 = 256x192`,
   `+0x34 = codec 0x022C9F38`; `s01.mods` is the **only** container with 581
   frames (`op_u`/`op_d` 1317, `s02` 746, `s03` 1911, `s05` 162, `s07` 573,
   `s07B` 460, ...).  Our runtime logs
   `movie: stream 2 codec=0x022C9F38 source=rom:data/movie/op_u.mods frames=1317`
   because `tools/nnk_smc.inc:1844 p5t_stream()` binds the container by stream
   **ordinal** (`p5t_nstr == 1 ? op_d : op_u`), never by what the guest opened.
   So every in-game-cutscene frame is the wrong film; 5BA's "same scene's
   outline/residual ghost" comparison was made against op_u content.
   The fix is to bind the stream to the container the guest actually opened —
   the player struct's first five header words (581 / 256x192 here) identify it
   uniquely among `data/movie/*.mods`.  Not done here (renderer/decode territory,
   and it would invalidate this phase's frame evidence).
3. The background behind the box is that wrong film (banks C/D), Phase-5AX's
   stream-2 decode territory.

## 5. Verification

* No runtime/guest code, no renderer, no VRAM model touched — `git status` shows
  only this document and the new `tools/melds_ram.py` (offline melonDS RAM
  extractor; no prelude fragment, so the manifest audit is unaffected).
* Savestate hand-off verified by re-loading it in a fresh process and rendering
  the prompt (`work/repro_5bc/r_s4.log`, `state: loaded ... identity OK`).
* Determinism of the A/START difference: the control and A runs are
  *bit-identical* in RAM except the key word (`cmp` of the two 4 MB dumps), so
  the START effect cannot be run-to-run noise.
* Phase-5BB's 1000/1000 verify sweep, movie-band corr and `NNK_EXC_TRACE`
  behaviour are untouched by this phase (no code change).

## Evidence (under `work/repro_5bc/`, never committed)

`healthy_prompt.nnks` (healthy state AT the prompt) · `start.nnk`,
`start6200.nnk` (scripts) · `s2/g.06180.bot.ppm.png` (live frame after the
START press) · `s4/k.06178.bot.ppm.png` (same frame re-rendered from the
savestate) · `rA.log` (A press), `r_start.log`/`r_s3.log` (START press),
`rD2.log` (control) · `mel_ram.bin` (melonDS main RAM) · probe scripts
`mel_cpu.py`, `findkey.py`, `findptr.py`, `objfind.py`, `vtobj.py`,
`trace_last.py`.
