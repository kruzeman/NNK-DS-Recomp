# Phase-5Z — scripted input, the recorder, and the frontier past 409 M steps

Follows Phase-5X (merge `7b20599`: class 35 PIC-base fix + class 36 VRAM read
shadow + the single-value `NNK_INPUT_FILE`).  This phase delivers the two things
the operator asked for — a **script** input format and a **recorder** — proves
the round trip, and then uses them to measure how far the guest actually runs
now that the guest can read back what it wrote to VRAM.

The input work already existed as `8486d7c` on the parent branch (committed
minutes after the merge point, so it never reached `7b20599`); it was
cherry-picked here as `72e9fc8` rather than re-derived.

## 1. `NNK_INPUT_FILE` accepts a script

```
0 0x3FF
13 0x3F7        # Start down (advance one prompt)
15 0x3FF        # Start up
917 0x3FE       # A down  (the operator's movie skip)
919 0x3FF       # A up
end
```

* one `<frame> <mask>` per line: `frame` decimal DISPLAY frame (`gxd_frames`),
  `mask` hex/octal/decimal (`strtoul` base 0), `#` comments, blank lines;
* `end` stops parsing; EOF (or `end`) **holds** the last mask;
* the file is re-parsed only when its **mtime/size** changes; a reload does not
  replay entries already in the past, it only arms the future ones;
* entries are applied **in order** as the frame counter passes them (out-of-order
  frames are reported on stderr and still applied in file order);
* every applied entry is traced under `NNK_INPUT_TRACE`:
  `script applied frame=<f> mask=<m> entry=<i>/<n> pressed=<p> released=<r>`;
* the legacy single-number form still works (DQ8-compatible) and becomes a
  constant mask applied at load;
* one-line exit summary (atexit):
  `nnk_input: summary script entries=<n> applied=<n> last_frame=<f> recorded=<n> entries`.

Mask bits (KEYINPUT active-low, idle `0x3FF`): A=bit0 `0x3FE`, B=bit1 `0x3FD`,
Select=bit2 `0x3FB`, Start=bit3 `0x3F7`, Right=bit4 `0x3EF`, Left=bit5 `0x3DF`,
Up=bit6 `0x3BF`, Down=bit7 `0x37F`, R=bit8 `0x2FF`, L=bit9 `0x1FF`; bits 10/11
are DS X/Y (EXTKEYIN 0/1).  A mask below `0x400` is published with X/Y released,
so a handwritten KEYINPUT script never presses the extra buttons by accident.

Why frame-timed at all: the guest polls `KEYINPUT` **about once per display
frame** (measured ~520–560 k steps/frame here), so a step-timed tap shorter than
a frame is usually invisible.  That is what made Phase-5X's register-for-register
reproduction need repeated presses.

## 2. `NNK_INPUT_RECORD` and the round-trip proof

`NNK_INPUT_RECORD=<path>` writes, **once per display frame**, one
`<frame> 0x%03X` line whenever the *effective* guest mask (SDL keys +
`NNK_INPUT_FILE` script + `NNK_INPUT_INJECT`, i.e. exactly what was pushed to
`gxd_set_keymask`) changed, `fflush`ed per line so a crash still leaves a usable
script.  A 3-line `#` header carries the bit legend, so the file is accepted
verbatim by `NNK_INPUT_FILE`.

Round trip (short fixture, `scratch/p5z/rt.sh`, binary = spliced p5u image):

```
script:  0 0x3FF 13 0x3FD 15 0x3FF 49 0x3FD 51 0x3FF
         85 0x3FE 87 0x3FF 120 0x3F7 122 0x3FF end
play   : NNK_INPUT_FILE=short.nnk NNK_INPUT_RECORD=rec.nnk   (70,000,001 steps, 131 frames)
replay : NNK_INPUT_FILE=rec.nnk                             (70,000,001 steps, 131 frames)

nnk_input: summary script entries=9 applied=9 last_frame=131 recorded=9 entries   (play)
nnk_input: summary script entries=9 applied=9 last_frame=131 recorded=0 entries   (replay)

--- script vs recorded:                 0 differences
--- applied masks, play vs replay:      0 differences
done steps=70000001 distinct_pcs=19478 halt_pc=0x02007134   (identical in both runs)
```

The recorder also captures the other input sources, i.e. the merge is real:
with no script at all and `NNK_INPUT_INJECT="30000000:X:down;31000000:X:up"`
(DS A), the recording is `0 0x3FF`, `59 0x3FE`, `61 0x3FF` — the injected edge
appears in the replayable file at the frames the guest was in.

## 3. Frontier with the VRAM read shadow (class 36)

The session itself is committed as `tools/nnk_operator_session.nnk` (113
entries: Start `0x3F7` held 2 frames every 36 frames from frame 13 → one prompt
per press, then the operator's A `0x3FE` at frame 917, `end`); the recorded
effective mask of the run below is `evidence/phase5z/operator_rec.nnk`, and the
verbatim log extracts are `evidence/phase5z/SUMMARY.txt`.

Image: frozen `work/phase3a/arm9_thumb_ovl_itcm_p5u.c` + the class-36 read
shadow spliced in by `tools/p5x_runtime_splice.py` (the transform that
`nnktool/src/recompile.rs` now carries), built with `NNK_GX=1`:

```
cd /tmp/nnk_movie
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
NNK_STEP_LIMIT=900000000 NNK_GX_SHOT=/home/hermes/NiNoKuni/shots/p5z/f \
NNK_GX_SHOT_EVERY=5 \
NNK_SMC_IMAGE="0x02000000:arm9.bin,0x020C0000:ovl_p4f4.bin" \
NNK_INPUT_FILE=operator.nnk NNK_INPUT_RECORD=rt/operator_rec.nnk \
  ./nnk_runner arm9_stored_padded.bin
```

```
done steps=900000001 traced=900000000 distinct_pcs=23174 hle_hits=7760
     halt_pc=0x0200911C halt=step budget exhausted
gx: 1613 frames presented, 205174 unhandled io writes
nnk_input: summary script entries=113 applied=91 last_frame=1613 recorded=91 entries
```

So the guest now runs **900 M steps** — past the operator's 409,074,823 halt and
past Phase-5X's 331,707,217 post-fix halt by almost 3x — with **23,174 distinct
PCs** and **1,613 presented frames**, and it does **not** stop on a translation
gap: the budget ends it at `0x0200911C` with a **healthy register file**
(`r13=027E3A00 r14=02009114 nzcv=0110`, no `untranslated pc` line at all).
Phase-5X's 520 M shadow run ended at `0x02009110` with 23,161 PCs, so +13 PCs
for +380 M steps: the guest is in a long-lived wait, not exploring new code.
Re-running the same script reproduces the numbers and the same 34 screen
transitions exactly.

Two input facts fall out of the trace and matter for the next phase:

* the script's A entry applied at **step 510,500,540 / frame 917** — exactly the
  operator's ~510 M press (20 M steps per 36 frames, 558 k steps/frame);
* but the guest's **last `KEYINPUT` read is step 328,139,850 (frame 591)**, i.e.
  the game stops polling input entirely once the Mobiclip splash is over.  The
  skip press is therefore *inert* in this build — the movie-era code path never
  asks for a key.  That is a measurement, not an input bug.

## 4. Screens (PNG captures)

`tools/p5z_shots.py` groups the `NNK_GX_SHOT_EVERY=5` dumps by content hash and
writes the first frame of every distinct screen as a stdlib-only PNG
(`zlib` + `struct`, no imaging library), so "every screen transition" is the
observed set, not a guess.

```
shots: 322 frames sampled (5..1610), 34 transitions
wrote 61 PNG(s) to /home/hermes/NiNoKuni/shots/p5z_png/
raw PPMs: /home/hermes/NiNoKuni/shots/p5z/f.<frame>.{top,bot}.ppm
```

The screens reached (frames = display frames, ~558 k steps each):

| frames | screen |
|--------|--------|
| 5–100 | boot flashes: black / grey / light-grey (DISPCNT still settling) |
| 205–235 (bottom) | dark screen fading in with a "Tap to …" prompt |
| 255–270 (top) | LEVEL-5 logo (stylised wordmark, near-white) |
| 405–420 | transition to the white "Licensed by Nintendo®" screen (bottom) |
| 435–580 (top) | **Mobiclip "VIDEO CODEC" splash** — red `mobi`/`clip` + badge |
| 585–600 | fade to black |
| 600–1610 | black on both screens (movie-era path; the HLE decode is still black) |

This is the first phase that shows the Mobiclip splash *and* keeps running past
it: the splash is drawn by the guest's own 2D path at frame 435 and stays until
frame 580, after which the screen goes black — i.e. the frontier is the movie
plane, not the front-end.

## 5. Verification

```
cd nnktool && cargo test        -> 37 passed; 0 failed   (class 37 = script+recorder)
SKIP_MELONDS=1 sh tools/verify_phase2a.sh
```

Class 37 locks the script parser (`<frame> <mask>`, `end`, backward-frame
report), the frame-gated application, the per-entry delta trace, the recorder's
edge-only flushed lines, the KEYINPUT-only X/Y normalisation and the
round-trip fixture arithmetic.

## 6. Open frontier after this phase

1. The guest is parked, not lost: 900 M steps end at `halt_pc=0x0200911C` in a
   wait loop (`0x020078F4..0x020078FC` in Phase-5X's pc trace) that keeps
   presenting frames (1,613) and raising vblank IRQs, while `distinct_pcs`
   grows by only +13 from 520 M to 900 M.  Next measurement: what that waiter
   is waiting for (a movie frame counter, a card stream, an IPC answer) —
   probe the loop's guard rather than pushing the step budget.
2. The movie era is black.  Phase-5T's HLE Mobiclip decode is wired but the
   planes stay black from frame 585 on, and the guest stops polling
   `KEYINPUT` after frame 591, so the operator's A skip cannot be observed
   until the movie path is real.
3. `NNK_INPUT_RECORD` + `NNK_INPUT_FILE` make any future live session
   replayable: hand the operator `NNK_INPUT_RECORD=session.nnk`, then re-run
   with `NNK_INPUT_FILE=session.nnk` (works identically headless).
