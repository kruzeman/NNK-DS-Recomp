# Phase-5AD — the movie session: who starts it, who stops it (measured)

Follows Phase-5AC (`2011fa7`).  The card asked three questions: why the session
starts at display frame 578 instead of 4033, why exactly 7 frames are presented,
and how to make the band at `f4000..f4300` match the reference again.

Both questions have the same answer, and it is **not** the VRAM read shadow,
**not** the ring, and **not** the movie clock:

> The early session and the 7-frame stop are **the operator input script**.
> `tools/nnk_operator_session.nnk` presses **Start every 36 display frames,
> from frame 13 to frame 1189**, and the movie player's own frame loop
> (`0x0207D124..0x0207D2E8`, overlay 13) treats an A/Start edge as *skip the
> movie* and leaves the loop.  The session that Phase-5AC measured starting at
> f578 was killed 11 frames later by the very same script — the press at
> f589 — so the band never got host frames.
>
> With the canonical YUV pair but **no input script the decode driver is never
> called through 340M steps** (0 hits; the HLE is never even engaged), exactly
> like melonDS, where the first decode call is at ts 2.27e9 / f4033.  The
> configuration was never the problem; the *measurement harness* was.

## 1. Measurement A — unscripted: no session until the movie window

Same binary (build path with the mandatory Phase-5X read shadow), same SMC
images, canonical `NNK_MOVIE_YUV=op_u.yuv NNK_MOVIE_YUV2=op_d.yuv`:

```
NNK_STEP_LIMIT=340000000, no NNK_INPUT_FILE, NNK_DEBUG_AT_PC=0x0207B1D0
  -> 0 dbg hits at the decode driver 0x0207B1D0
  -> no "movie: hle"/"movie: stream" line  (the decode callback 0x01FF8540
     was never entered: p5t_note_codec() is the only thing that initialises it)
  -> done steps=340000001 halt=step budget exhausted
```

The same binary, same budget, with `NNK_INPUT_FILE=nk_operator_session.nnk`,
enters the movie player at **step 323,391,401** (= display frame 578) with
`r0=0x02278264` (player A), `r14=0x0207D190` (the per-frame caller).

So the first differing input of the two runs is not a guest word at all: it is
the **keypad stream**.  There is no melonDS counterpart to compare a word
against, because melonDS has no scripted-input run in this project — which is
exactly why Phase-5AC's "shadow-fixed guest starts early" reading could not be
falsified there.  It is falsified here: unscripted native == melonDS (first
decode at the movie, not at boot).

## 2. The guest's movie loop, from the disassembly

`0x0207CF18` is "play a movie": it polls `KEYINPUT` (0x04000130) OR'd with the
ARM7 mirror (0x02FFFFA8), inverts and masks it (`(v ^ 0x2FFF) & 0x2FFF`), and
keeps `A|Start` (`& 9`) as the *skip* mask.  It then creates two players
(`0x0207A570`, 6 ring slots each) and runs:

```
0x0207D188  bl 0x207a744        ; decode step, player A
0x0207D194  bl 0x207a744        ; decode step, player B
0x0207D198  add r7, r7, #1      ; six-slot burst counter
0x0207D19C  cmp r7, #6
0x0207D1A0  bcc 0x0207D1E8      ; (burst) -> poll keys, continue
0x0207D1A4  ...                 ; (once per 6) present gate:
0x0207D1AC  cmp r0, r8 / ble    ; [0x0208AFAC+0xC] (movie clock) vs r8
0x0207D1B4  cmp r9, #0 / bne    ; present once per clock tick
0x0207D1BC  bl 0x207a78c        ; present (converter) for A and B
0x0207D1E8  ldrh r1,[0x04000130] ; poll KEYINPUT
0x0207D1F0  ldrh r0,[0x02FFFFA8] ; poll ARM7 mirror
0x0207D230  and r1, r0, #9      ; *** A|Start (active-low) ***
0x0207D234  ldr r0, [sp, #0x14]
0x0207D238  cmp r1, r0          ; same as at entry?
0x0207D248  bne 0x0207D2EC      ; *** A/Start edge -> LEAVE THE LOOP ***
0x0207D2E0  bl 0x207a6fc        ; decode step A (0x0207AFB4)
0x0207D2E8  bne 0x0207D124      ; while it returns 1
```

Measured directly (`NNK_DEBUG_AT_PC=0x0207D248`, scripted run):

```
dbg2 step=327550216 pc=0207D248 r0=00000000 r1=00000008 r3=00002C08
     r4=00002FFF r5=02278264 r6=022E8A8C r7=00000007 r14=06618200
```

`r1 = 8` is Start in the active-low mask (`0x3F7`), `r0 = 0` is the value the
loop captured at entry, `r5`/`r6` are the two players, `r7 = 7` the burst
counter.  The branch at `0x0207D248` is the exit, and the word that differs
from an unscripted run is `KEYINPUT` (`0x04000130`): `0x03F7` (Start held,
the script's frame-589 entry) vs `0x03FF` idle.  (The hit at step 125,404,876
with `r1=0x10` is another overlay's code at the same address — the
`ovl_13_02079F80.bin` image is an arena UNION, so overlay addresses alias.)

`0x0207AFB4` (the decode step) is the function that returns 0 when the movie is
finished: `if ([player+0x98] == [player+0x0C]) return 0`.

**Both of those "ring full / movie finished" suspects are exonerated by
measurement.**  Phase-5AD extended the `NNK_P5U` witness line to dump the
player's header and stream counters (`tools/nnk_smc.inc`), and at the moment
the session stops (`step 326,421,022`, player `0x02278264`):

| word | value | meaning | verdict |
|---|---|---|---|
| `player+0x04` | `0x53444F4D` | "MODS" | the intro movie |
| `player+0x0C` | `0x00000525` = **1317** | total frames | not finished |
| `player+0x10/0x14` | `0x100` / `0xC0` | 256x192 | plane geometry |
| `player+0x98` | **7** | frames decoded | `!= 1317` → `0x0207AFC4` does **not** take the "finished" return |
| `player+0xA0` | **7** | frames presented | one per decode |
| `player+0x9C` | **1** | the other side of `0x0207B1DC`'s `cmp` | `1 > 7` is false → the ring-full early return of `0x0207B1D0` does **not** fire |

So the loop did not stall, starve or dead-lock: it **left**, through the only
remaining exit, the A/Start edge at `0x0207D248` — measured taken at step
327,550,216 with `r1 = 8` (Start).  Between the last present (326,421,022) and
that exit the loop still ran about one iteration per display frame (62 of the
1.4M instructions traced in [326.6e6, 328.1e6] are in `0x0207Dxxx`, i.e. ~3
iterations of a body that spends its time in callees), so it was idle-paced,
not spinning.

## 3. Measurement B — remove the presses, the session does not stop

Control experiment: the same script truncated after frame 555 (the last press
that precedes the session; nothing after it), everything else identical:

```
tools/nnk_operator_session.nnk   -> present n=7 at step 326,353,316 / 326,421,022
                                    and then NO further present to 340M
truncated at frame 555            -> present n=7 at 326,353,316 / 326,421,022
                                    present n=8 at 327,673,343 / 327,821,244  (continues)
```

The truncation changes nothing before the press and nothing else in the guest:
the 8th frame appears at exactly the step where the old run went idle.  The
"7 frames" is the operator's skip, delivered 11 frames after the movie starts.

## 4. What this means for the band

* the 7 frames the HLE wrote into the ring (`op_u` 0..6, `op_d` 0..6) are
  correct and are exactly what a real skip at f589 would leave behind;
* the session is gone afterwards, so nothing feeds the ring at `f4000+` and the
  canonical configuration shows black there — **with this script**;
* `NNK_MOVIE_YUV=1` (guest decoder) shows macroblock garbage there for the same
  reason: the guest decoder is decoding a stream that nothing advances any
  more, from the wrong reference.

The acceptance band is a property of an **unscripted** session (Phase-5AA's
`corr 0.9987/0.9985` was measured unscripted), so it is measured unscripted in
§5.

## 5. The band, canonical configuration, no input script — ACCEPTANCE

Command (reproduces the numbers below exactly; runner built by
`NNK_GX=1 NNK_IMAGE_C=<image>.c bash tools/build_split.sh <runner>`, the
"prelude splice" is applied by the build, i.e. the VRAM read shadow is in):

```
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' \
NNK_MOVIE_YUV=/home/hermes/NiNoKuni/work/p5t/op_u.yuv \
NNK_MOVIE_YUV2=/home/hermes/.hermes/profiles/coder/cache/scratch/p5t/op_d.yuv \
NNK_GX=headless NNK_STEP_LIMIT=2400000000 \
NNK_GX_SHOT=/tmp/p5ad/shots/f NNK_GX_SHOT_EVERY=25 \
NNK_SMC_IMAGE="0x02000000:/tmp/p5ad/arm9.bin,0x020C0000:/tmp/p5ad/ovl_p4f4.bin" \
/tmp/p5ad/nnk_p5ad arm9_stored_padded.bin
# then:
python3 tools/p5ad_band.py /tmp/p5ad/shots <op_u.yuv> /home/hermes/NiNoKuni/shots 3990 4300 p5ad 600
python3 tools/p5ad_band.py /tmp/p5ad/shots <op_d.yuv> /home/hermes/NiNoKuni/shots 4120 4290 p5ad_bot 600 bot
```

The session starts exactly where melonDS and Phase-5AA put it:
`movie: stream 0 codec=0x0227834C ... step=2256256944` (= display frame 4033).

| capture | TOP best ref / corr | BOTTOM best ref / corr |
|---|---|---|
| f4000 | splash (mean 211) | splash |
| f4025 | splash (mean 130) | splash |
| f4050..f4110 | black (movie not started) | black |
| f4125 | `op_u` f16 **0.9986** | `op_d` f16 **0.9990** |
| f4150 | `op_u` f20 **0.9986** | `op_d` f20 **0.9990** |
| f4175 | `op_u` f24 **0.9986** | `op_d` f24 **0.9992** |
| f4200 | `op_u` f27 **0.9989** | `op_d` f27 **0.9992** |
| f4225 | `op_u` f30 **0.9990** | `op_d` f30 **0.9991** |
| f4250 | `op_u` f34 **0.9990** | `op_d` f34 **0.9991** |
| f4275 | `op_u` f37 **0.9990** | `op_d` f37 **0.9991** |

7 successive captures, 7 **different and increasing** movie frames (16 → 37, so
not a frozen plane), luma corr 0.9986..0.9990 on the top screen and
0.9990..0.9992 on the bottom, MAE 1.4..3.0 of 255.  Both streams show the SAME
movie frame at the same display frame, which cross-checks the two independent
sources.  f4200 (0.9989 / 0.9992, `op_u`/`op_d` f27) reproduces Phase-5AA's
unscripted `0.9987 / 0.9985` (`op_u`/`op_d` f27) to within 2e-4.

Side-by-sides (native RGB left, reference luma right, 516x192, stdlib PNG) in
`/home/hermes/NiNoKuni/shots/`:

```
p5ad_top_f4125_vs_ref0016_c9986.png .. p5ad_top_f4275_vs_ref0037_c9990.png   (7)
p5ad_bot_top_f4125_vs_ref0016_c9989.png .. p5ad_bot_top_f4275_vs_ref0037_c9991.png (7)
```

No crash regression: `done steps=2400000001 ... halt=step budget exhausted`,
**0** `untranslated pc` lines through 2.4e9 (FACT 1 / class 39 intact).

### 5.1 The same band WITH the operator script is black — measured

Same binary, same sources, `NNK_INPUT_FILE=tools/nnk_operator_session.nnk`,
2.42e9 steps, shots every 25:

```
f3700 .. f4275   top: mean 0.0 sd 0.0     (24 consecutive captures)
                 bot: mean 0.0 sd 0.0
```

and the present log stops at `n=7` (`step=326,421,022`).  This is Phase-5AC's
cfg4 "22/22 flat black" reproduced — with its cause now named (§2/§3): the
script skipped the movie player at display frame 589, so neither the Mobiclip
splash nor the intro movie is ever shown in a scripted run.  The unscripted run
above shows the splash at f3725..f4025 and the movie from f4110 — i.e. the two
runs differ in the **input**, not in the configuration.

## 6. Remaining gap (honest)

1. **Nothing in the session code needs a fix for the canonical configuration.**
   The early start and the 7-frame stop are the operator's skip; unscripted, the
   session starts at f4033 and presents per decode step, and the band matches at
   corr >= 0.9986 on both screens.  What WAS wrong is the measurement harness:
   every Phase-5AC band row was taken with a script that skips the intro.
2. **The film frame rate is the guest's, not one per display frame**: 7 movie
   frames per 167 display frames in the measured band (burst of 6 at the start,
   then ~1 per 2-8 display frames).  Phase-5T's melonDS ring at ts 2.6179e9
   (f4677) held `op_u` f125..f130, i.e. the same order of magnitude, so this is
   the emulated card/audio pacing, not a new defect — but a frame-per-display-
   frame comparison against melonDS in the *same* window is still unmeasured
   (melonDS has no scripted-input run here).
3. `NNK_MOVIE_YUV=1` (guest decoder) still paints macroblock garbage in the band
   (Phase-5AC cfg1); it is not the acceptance path and was not re-measured here.

## 7. Canonical configuration and how to reproduce

```
build: NNK_GX=1 NNK_IMAGE_C=<image>.c bash tools/build_split.sh <runner>
run:   NNK_ROM_PATH='Ni No Kuni.nds' \
       NNK_MOVIE_YUV=<op_u.yuv> NNK_MOVIE_YUV2=<op_d.yuv> \
       NNK_GX=headless NNK_STEP_LIMIT=2420000000 \
       NNK_GX_SHOT=<base> NNK_GX_SHOT_EVERY=25 \
       NNK_SMC_IMAGE="0x02000000:<arm9.bin>,0x020C0000:<ovl_p4f4.bin>" \
       <runner> <arm9_stored_padded.bin>
       (add NNK_INPUT_FILE=tools/nnk_operator_session.nnk only if the scripted
        session is what you want to study — it SKIPS the intro at f589)
```

## 8. Instruments and verification (this branch)

`tools/nnk_smc.inc` `NNK_P5U` sample line now also dumps, for the movie player
at `0x02278264`: the header words `+0x00..+0x30` (`0x0C` = frame total,
`0x10/0x14` = 256x192), the stream counters `+0x88..+0xB0` (`0x90/0x98/0x9C/
0xA0`, the ring-full and finished witnesses), and the movie clock
`0x0208AFAC+0x0C/+0x10/+0x14`.  No guest state is touched.

`tools/p5ad_band.py` (new, stdlib only): per-capture luma correlation of a shot
directory against a YUV stream (stride-8 coarse pass + stride-2 refine) plus
side-by-side PNGs (native RGB | reference luma) — the tool that produced §5.

Locks: cargo class 41
`movie_session_ends_on_the_skip_key_edge_not_a_starved_ring` (suite 42) pins
the operator script's press cadence (Start every 36 frames, one 72-frame gap at
the frame-917 A press, frame 589 present), the exonerated guards
(`[0x98]=7 != [0x0C]=1317`, `[0x9C]=1 <= [0xA0]=7`), the measured ordering
(last present 326,421,022 < skip exit 327,550,216 < control's 8th present
327,673,343) and the Phase-5AD witness fields.

```
cd nnktool && cargo test --release   -> 42 passed; 0 failed   (class 41 new)
SKIP_MELONDS=1 sh tools/verify_phase2a.sh -> green:
   A 192/192 + 64/64 + 64/64 + 125/125, A2 Thumb battery 51/51 vs binutils,
   B 2020 functions / 72959 cases recovered, C1 1942/2020 (96.1%) agree with
   Ghidra, C2 body coverage 100.0% (>=1 insn) / 96.5% byte-weighted,
   D recompile-all -> cc -> run (20M steps, no HLE halt), E/F melonDS vs
   native PC trace 1000/1000 = 100.0% (strict prefix and after resync)
```

No regression to the acceptance checks of earlier phases: A/A2/B/C/D/E/F all
report their previous values; this phase's only runtime change is a diagnostic
(`NNK_P5U`) whose fields are reads (`st32` count in the block is 0, locked by
class 41).

