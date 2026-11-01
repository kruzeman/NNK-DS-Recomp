# INPUT.md — SDL keyboard controls for the Ni no Kuni DS runtime (Phase-4W)

The windowed runtime (`NNK_GX=window`) maps the host keyboard to the real DS
input registers.  The guest reads them exactly as on hardware: `KEYINPUT`
(0x04000130, active-low, bits 0-9) and `EXTKEYIN` (0x04000136, DS X/Y on
bits 0/1).  Everything below is proven end to end — see "Proof".

## 1. Key map

| SDL key              | DS key   | register bit                    |
|----------------------|----------|---------------------------------|
| Arrow Up/Down/Left/Right | D-pad Up/Down/Left/Right | KEYINPUT 6/7/5/4 |
| X                    | A        | KEYINPUT 0                      |
| Z                    | B        | KEYINPUT 1                      |
| S                    | DS X     | EXTKEYIN 0 (KEYINPUT mask bit 10) |
| A                    | DS Y     | EXTKEYIN 1 (KEYINPUT mask bit 11) |
| Q                    | L        | KEYINPUT 9                      |
| W                    | R        | KEYINPUT 8                      |
| Return               | Start    | KEYINPUT 3                      |
| Right Shift          | Select   | KEYINPUT 2                      |
| Backspace            | Select   | KEYINPUT 2 (alternate)          |
| mouse button 1 on the sub screen | touch | —                        |
| window close / SDL quit | run stops | —                           |

Semantics (all melonDS-aligned, `src/NDS.cpp`):

* KEYINPUT is **active-low**: 0 = pressed; idle word is `0x03FF`.  The DS X/Y
  buttons are not in KEYINPUT: they live in EXTKEYIN bits 0/1 (also
  active-low; `NDS::SetKeyMask` maps its mask bits 10-11 onto `KeyInput`
  bits 16-17 = EXTKEYIN bits 0/1).  EXTKEYIN idles at `0x7F`
  (`KeyInput` reset `0x007F03FF`), the lid stays open.
* Key **repeat** events are ignored (a held key stays pressed from its
  original KEYDOWN; auto-repeat adds nothing).
* **Focus loss / minimize releases every key** (no stuck keys).  This also
  fires on the deterministic window-focus event at the end of a run under
  xvfb.
* Key down/up is edge-driven per scancode; there is no latched state outside
  the register mask.

## 2. Running the windowed build with input

```sh
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c BUILD_JOBS=16 bash tools/build_split.sh out/split/arm9_gx

NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_GX=window \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  xvfb-run -a out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

(On a machine with a real display drop `xvfb-run`.)  `NNK_GX=headless` runs
without SDL entirely; keyboard state is then only reachable through the
injection path below.

## 3. Input injection + trace (the proof harness)

* `NNK_INPUT_INJECT="<step>:<scancode-name>:<down|up>[;...]"` — at the given
  runtime step, push a real SDL key event (`SDL_PushEvent`, windowed) through
  the same filter real keyboard events take, e.g.
  `NNK_INPUT_INJECT="17580000:X:down;17590000:X:up"`.  Headless runs apply
  the identical state change directly through `gxd_set_keymask`.
* `NNK_INPUT_TRACE=<path>` — append-only trace: filter install, every key
  transition (scancode, DS key, resulting KEYINPUT mask + EXTKEYIN), every
  guest KEYINPUT/EXTKEYIN read with step + value + frame, injections,
  release-all events.

## 4. How it is wired

* `tools/nnk_input.inc` (new) is the single owner of SDL keyboard state.  An
  SDL event filter (`SDL_SetEventFilter`) consumes KEYDOWN/KEYUP **before**
  `gx_display`'s own `pump_events` can see them, so the built-in gxrender
  keymap is bypassed and there is exactly one mapping in the system.  The
  resulting state is written through the merged API's key_set path
  (`gxd_set_keymask`), which is what `gxd_io_read(0x04000130)` serves to the
  guest.
* Guest `KEYINPUT` reads flow `io_read16 -> gxd_io_read` (Phase-4E bridge,
  unchanged).  `EXTKEYIN` (0x04000136) is served by the runtime from its own
  key state (melonDS `NDS.cpp:3892`: read16(0x04000136) = `KeyInput >> 16`).
* The fragment is spliced into `nnk_main.c` by `tools/split_dispatch.py`
  (same pattern as `nnk_dma.inc`/`nnk_vramcnt.inc`, Phase-4L/4M): one
  early-out line at the top of `io_read16()` (KEYINPUT read observation +
  EXTKEYIN service) and one per-dispatch tick (lazy init + injection
  schedule).  It compiles to no-op stubs in non-`NNK_GX` builds, so nothing
  changes for the default build.  `gxrender/` itself is untouched — only its
  merged API is called.

## 5. Proof (end to end)

Run of 2026-10-31 (`out/split/arm9_sdlinput`, windowed under xvfb, native
boot of `work/phase3a/arm9_stored_padded.bin`), evidence committed under
`evidence/phase4w/`:

* `input_trace.log` — synthetic SDL events injected at known steps, the
  resulting KEYINPUT/EXTKEYIN word transitions, and the **guest's own reads**:
  * `inject SDL_KEYDOWN scancode=X` (step 17,580,000) -> mask `0xFFE`;
    guest KEYINPUT reads #1-#3 (steps 17,586,697/751/815) return **0x03FE**
    (DS A pressed).
  * `inject SDL_KEYDOWN scancode=W` (step 18,556,000) -> mask `0xEFF`;
    guest KEYINPUT reads #4-#5 (steps 18,557,557/18,560,701) return
    **0x02FF** (DS R pressed).
  * `inject SDL_KEYDOWN scancode=S` (step 20,000,000) -> EXTKEYIN **0x7E**
    (DS X pressed, bit 0 clear), back to `0x7F` on release.
  * All releases return the idle word `0xFFF`/`0x03FF`/`0x7F`; the real
    focus-loss event at step 27,371,209 exercises the release-all path.
* `pressed_f038_{top,bot}.ppm` / `released_f041_{top,bot}.ppm` — screenshot
  pair (frame 38: DS R held while the guest read `0x02FF`; frame 41: all
  released).  The frames are still uniform white at this boot frontier (the
  DISPCNT switch is a later barrier, see PHASE4U_READ_QUEUE.md §4) — the
  register-level trace above is the input proof.

No boot/IRQ/card code was touched; `verify_phase2a.sh` stays green on the
branch.

## 6. Frame-timed input: `NNK_INPUT_FILE` (DQ8 `DQ8_INPUT_FILE` analogue)

A plain text file whose content is the desired active-low key mask, re-read
once per **display frame** (gated on the presented-frame counter) and
published through the same `gxd_set_keymask` path a real key edge takes.
Two forms:

```
# legacy (DQ8-compatible): the whole file is ONE number = constant mask
0x3FD
```

```
# SCRIPT: <frame> <mask> per line; frame = decimal DISPLAY frame, mask = hex
# '#' comments and blank lines are ignored; 'end' (or EOF) holds the last mask
0 0x3FF
13 0x3FD      # B down (advance the notice screen)
15 0x3FF      # B up
917 0x3FE     # A down (skip the movie)
919 0x3FF     # A up
end
```

Bits (KEYINPUT active-low; idle `0x3FF`, cleared bit = pressed):
A=bit0 (`0x3FE`), B=bit1 (`0x3FD`), Select=bit2 (`0x3FB`),
Start=bit3 (`0x3F7`), Right=bit4, Left=bit5, Up=bit6, Down=bit7,
R=bit8, L=bit9; bits 10/11 = DS X/Y (optional, released by default).

Semantics:

* an entry's mask is applied when the display-frame counter reaches its
  frame and **held** until the next entry; entries are applied in file order;
* the file is parsed **once per change** (mtime/size); a reload during a run
  does not replay entries already in the past;
* a missing file **holds** the last mask (the host may write it only on
  transitions);
* every applied entry is traced under `NNK_INPUT_TRACE` as
  `script applied frame=<f> mask=<m> entry=<i>/<n> pressed=<p> released=<r>`;
* one-line exit summary: `nnk_input: summary script entries=<n> applied=<n>
  last_frame=<f> ...`.

Why frame-timed: the guest polls `KEYINPUT` about **once per display frame**
(~556 k steps in the movie era), so a 20 k-step step-timed tap is usually
invisible — which is exactly why fixed-step `NNK_INPUT_INJECT` never
reproduced the operator's real session while a frame-timed (or repeated)
press does.  See PHASE5X.md §1.

## 7. Input recorder: `NNK_INPUT_RECORD`

```
NNK_INPUT_RECORD=/tmp/session.nnk ./run.sh     # play: the file IS the script
NNK_INPUT_FILE=/tmp/session.nnk  ./run.sh      # replay it verbatim
```

At every display frame, if the **effective** guest mask (SDL keys +
`NNK_INPUT_FILE` script + `NNK_INPUT_INJECT`, i.e. exactly what was pushed to
`gxd_set_keymask`) changed, one `<frame> 0x%03X` line is appended and
flushed (a crash still leaves a usable script).  A 3-line header records the
title, that the build id is unavailable, and the bit legend.  Recording works
in window mode (the operator plays) and headless.  The recorded frame samples
are what the guest could have seen, so replaying the file reproduces the same
mask sequence — proven in PHASE5X.md §6.
