# Phase-5W — the writer of the OSi_ThreadInfo corruption: the HLE plane copy

Follows Phase-5V (`11b4d3e`, merged `9b620ab`).  Phase-5V measured that the
whole OSi_ThreadInfo header at `0x0206C684` reads zero at the movie park while
a stale wait flag holds `0xE92D4000` (a `push {lr}` code word), and that the
narrow store watch (2.25e9..2.257e9) saw **no** writer.  This phase built a
probe that cannot be bypassed, named the writer, fixed it with measured
semantics, and locked it with cargo test class 21 (suite 35).

## 1. The probe: a per-instruction STATE DIFF

`tools/p5w_store_probe_splice.py` (the Phase-5Q/5V splice pattern, inert when
unset) injects three things into a frozen image:

| piece | what it measures |
|---|---|
| `p5w_store()` from `st8`/`st16`/`st32` | every store into `0x0206C680..0x0206C6B0` with PC/step/lr/r0-r3, a 512-entry ring buffer, and a health check (a plausible pointer slot turning 0 or garbage) |
| `p5w_diff()` from `trace_pc()` | **a 7-word snapshot of the block before every instruction**, compared with the previous one.  Because `trace_pc()` runs at dispatch *before* the instruction executes, the first differing snapshot names the **previous** instruction as the writer — whatever path it used.  This is the instrument that closed the case: the writer turned out to be a C `memcpy` that never touches `st32`. |
| `p5w_tick()` / `p5w_park()` | the block read through **both** `ld32` and a raw `gmem` pointer, plus `itcm_size`/`dtcm_base`/`dtcm_mask` — so a TCM read-path alias cannot masquerade as destroyed memory |

## 2. What it measured (run to 2.2573e9, then 2.30e9 with the fix)

**The block is healthy for 2.25e9 steps.**  Every region store in the whole run
came from the SDK scheduler itself: only two addresses are ever written
(`0x0206C6A0` = the 16-bit guard, `0x0206C6A4` = the current-thread slot) and
only with plausible thread pointers (`0x0206C770`, `0x0206EA48`, `0x0207981C`,
`0x02079200`, `0x02079750`, `0x0206F540`, `0x0206C6B0`).  The last legit store
is at step `2,256,159,918`.

**The state diff names the writer.**  First dead-state snapshot:

```
p5w: DIFF at step=2256256945 pc=0x0207B21C prev_pc=0x01FF9E00
     old=00000000 00000000 0206C6A4 0206C770 02079200 00000000 0206C770
     new=00000000 00000000 00000000 00000000 00000000 00000000 FFFFFFFF
     lr=0x0207B21C r0=0x000000DE r1=0x022E49C8 r2=0x022E48E8 sp=0x027E3A18
     pcs= ... 0x01FF9DF8 0x01FF9DFC 0x01FF9E00 0x01FF9DE4
```

`0x0207B21C` is the movie callback's **return** address, `prev_pc=0x01FF9E00`
the last instruction of the guest's ITCM decode callback: the block dies on the
step where `p5t_present()` runs.  And the HLE itself says which destination:

```
movie: plane REFUSED dst=0x02067FE8 n=49152 step=2256256944 pc=0x01FF9E00
```

`0x02067FE8 + 0xC000 = 0x02073FE8` **contains `0x0206C684`** at plane offset
`0x468C`: the player's source-plane field handed the HLE a destination 24 bytes
below the SDK block, and a full 256x192 plane was copied over it.  The field is
`[player+0x40]` — the same record whose `[player+0x3C]` reads `0x20` in every
present of the session (`movie: present ... src=0x00000020`), i.e. the player
has not populated its source-plane fields yet.

Classification: **not** a stray guest store, **not** a computed store pointer,
**not** a missing decoder form.  It is the Phase-5T HLE plane contract being
handed uninitialised guest pointers: `p5t_plane()` validated only "is main RAM"
`(dst & 0xFF000000) == GBASE` and "fits in 4 MiB", so a stale field could drop
48 KiB anywhere in the game's own image, and it did — on the SDK scheduler.

## 3. melonDS at the same point

`MELONDS_RAMDUMP_PATH` + `MELONDS_ARM9TRACE_TSLO/TSHI=2256000000/2256400000`
gives melonDS's main RAM at `ts=2,256,242,970` (the movie-start window, the
same master-cycle epoch Phase-5P used):

```
melonDS  0x0206C680=00000001  0x0206C684=0  0x0206C688=0  0x0206C68C=0206C6A4
         0x0206C6A4=0206C6B0  0x0206C6A8=02079200           [0x0206C6A4]=0206C6B0
```

intact — the reference engine never writes there, because its own player fills
`+0x3C/+0x40` before the decode step that uses them.

## 4. The fix (measured semantics, no new class)

`tools/nnk_smc.inc::p5t_plane()` may only write inside the guest's **movie
arena** `0x02100000..0x02400000`, the range every measured movie buffer lives
in (melonDS ring slot `0x022C0880`, native cursors `0x022E48E8`/`0x02355110`,
codecs `0x0227834C`/`0x022E8B74`, player `0x02278264`).  A destination outside
it is refused **and logged once per distinct value** — never silently dropped:

```
movie: plane REFUSED dst=0x02067FE8 n=49152 step=2256256944 pc=0x01FF9E00
       (outside movie arena 0x02100000..0x02400000)
```

This keeps the HLE's real job (write the six Y ring slots and the two source
planes once the player has filled them) and removes the only out-of-arena
writer the measurement found.

## 5. Result after the fix (run to 2.30e9)

* **the block never dies**: 0 dead-state diffs in the whole movie window; the
  last fine sample before the halt (`step=2,280,200,000`) reads
  `ld=00000001 00000000 00000000 0206C6A4 0206C770 02079200` — header and
  ready-queue head intact, 24 M steps past the old death point;
* **interrupts flow again**: 250 `irq enter` entries after the movie start
  (median gap 62,589 steps ≈ 8.9 entries per 558 k-step frame); before the fix
  the last delivery was step 2,256,303,812 and nothing followed;
* **the player iterates the decode callback**: `cb_entry=23 cb_ret=23`
  (Phase-5U measured 2);
* **the HLE planes reach VRAM and the screen**: 4 presents and the captured
  display shows the **Mobiclip Video Codec splash** — real movie content, not
  black:
  `shots/p5w_f04000_top.png` (= `shots/p5w.04000_top.png`, display frame 4000;
  `f.04000.top.ppm` mean RGB 251/211/211, min 0 max 251 — vs the black frames
  Phase-5T captured at 4120/4140).

## 6. The remaining gap (named, honest)

The intro **does not yet advance frame by frame**, for two measured reasons:

1. **the session restarts, so the HLE always presents stream frame 0**:
   `movie: session codec 0x0227834C -> 0x022E8B74` and back, seven times
   between steps 2,256,257,061 and 2,257,258,268; `p5t_present()` resets
   `p5t_index` on every session change and the cursor `cur = [codec]` jumps
   backwards, so all four presents show `frame=0`.  The captured screen is
   therefore static (display frames 3750/3900/3950/4000 byte-identical).
   `[player+0x3C]` still reads `0x20` in every present: the player is not
   populating its source-plane fields, so the *guest* side of the decode step
   is still not the one melonDS runs.
2. **a new halt one level further on**: `halt_pc=0x0207B218
   halt=dispatch to untranslated pc` at step `2,280,211,542` — exactly the
   `blx r1` call site of the decode callback, after the movie path ran for
   24 M steps beyond anything earlier phases reached.  The runtime has no case
   for that PC and the SMC re-decoder does not produce one.

Named follow-up (one bounded task): "Phase-5X — the callback call site
`0x0207B218` dispatches untranslated (extend the SMC band/re-decode coverage
for the runtime overlay slot) and the movie session restarts on every decode
step (find why `[codec]`/`[player+0x3C..0x40]` cycle, so `p5t_present()`'s
frame index advances instead of resetting to 0)".

## 7. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 35 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
```

Class 21 (`movie_plane_writes_stay_inside_the_movie_arena`) locks: the arena
bounds, the guard being the last check before the single `memcpy`, all three
refusal paths, the REFUSED log, and the probe's diff/ring/watch machinery at
`trace_pc` — so neither the guard nor the measurement can be lost silently.
