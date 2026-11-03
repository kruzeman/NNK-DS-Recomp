# Phase-5BM — after "Yes" the guest is **not** parked: it loads the post-skip scene from the card and the next screen is on display

Branch `wt/t_f6a38194` (master `ce08b84`).  **Measurement phase — no runtime
change was needed and none was made**: the "WFI park" the card asked me to wake
is the ARM9 OS idle thread doing its job, and the post-skip picture was already
being drawn in Phase-5BL's own last captured frame.  This file corrects the
5BL conclusion, names what the guest actually does in the black window, and
delivers the frame.

## 1. The claim "the guest parks in the WFI idle loop with IRQs disabled" is refuted

`0x02009118` is not a shutdown loop, it is the SDK's **shared `wfi()`
helper** — `mov r0,#0; mcr p15,0,r0,cr7,cr0,{4}; bx lr` — with **three
callers** in the image (`work/phase3a/arm9_thumb_ovl_itcm_p5u.c`):

```
0x0200712C  push {r3,lr} ; bl 0x02008D0C ; loop: bl 0x02009118 ; b loop   <- IDLE thread
0x02009108  push {r3,lr} ; bl 0x02008D20 ; loop: bl 0x02009118 ; b loop   <- shutdown
0x02010564  ...          ; bl 0x020096C4 ; loop: bl 0x02009118 ; b loop
0x020090D8  the shutdown helper: calls [0x0206CA14] then bl 0x02009108
```

`0x02008D0C` = `mrs r0,CPSR; bic r1,r0,#0x80; msr CPSR_c,r1` — **enables**
IRQ.  `0x02008D20` = the same with `orr` — disables it.  The idle-thread loop
therefore runs with `CPSR.I = 0`, the shutdown loop with `CPSR.I = 1`.

Measured on a cold boot of the "Yes" script (`work/p5bm/yes.nnk`, 4.2e9 steps,
`NNK_GX=headless`, this build `0x11CC991D`):

```
irq enter step=4199642801 resume=0x02009118 ... spsr=0x0000001F ime=1 ie=0x00040019 if=0x002C0000
irq enter step=4199977889 resume=0x02009120 ... spsr=0x0000001F ime=1 ie=0x00040019 if=0x002C0000
halt state: ... r13=0206C8A8 r14=02007138                      (r14 = return from `bl 0x02009118`
                                                                in the 0x0200712C idle loop)
```

* `spsr=0x0000001F` at the WFI instruction: the CPU is in **System mode with
  IRQs enabled** — not the "IRQ-off spin" 5BL described.
* `ime=1`, `ie=0x00040019` (VBlank=0, Timer0/1=3/4, DMA0=18), `if=0x002C0000`:
  the interrupt is **taken and handled** (handler at `0x02007258`, 55,643 IRQ
  entries in the run) and execution returns to the WFI.
* `NNK_DEBUG_AT_PC=0x020090D8 NNK_DEBUG_N=16` over all 4.2e9 steps: **0 hits**.
  The terminate/shutdown entry is never reached on the "Yes" path; the earlier
  `0x02044868 -> 0x020090D8 -> IRQ-off WFI` abort of 5AI/5AK/5AL does not occur
  in this build.

So there is **no missing wake event**: the guest sleeps at WFI, wakes on every
VBlank/DMA interrupt, runs the handler and sleeps again — exactly the ARM9 OS
idle thread.  A pc sample of 4 M steps at step 3.615e9 puts **17 %** of the
steps in that idle loop, which is normal between-frames behaviour, not a park.

## 2. What the guest is doing in the black window: loading the next area from the card

4 M-step pc sample at step 3.615e9 (`work/p5bm/D.out`, `NNK_TRACE_FROM=3615000000`)
— the first 4 M steps after the answer at 3.59e9:

| pc | hits in 4 M steps | meaning |
|---|---|---|
| `0x02016B30..0x02016B50` | 344,677 each (43 % of all steps) | ROMCNT poll loop of the card read routine |
| `0x02007134/0x02009118..20` | 172,493 (17 %) | idle-thread WFI (above) |
| `0x0200A638..0x0200A648` | 49,530 (6 %) | `swpb` copy loop (decompressor output) |
| `0x02016B3C` (word stored) | **39,552** | card words delivered |

The loop is the SDK card transfer routine (`0x02016AAC`), whose literals are
`0x040001A4` (**ROMCTRL**) and buffer `0x0206F120`:

```
0x02016B30  ldr  r2,[r4]        @ ROMCTRL
0x02016B34  tst  r2,#0x00800000 @ bit23 = data ready
0x02016B38  beq  0x02016B4C
0x02016B3C  ldr  r1,[fp] ; cmp r3,#128 ; strcc r1,[r5,r3,lsl #2] ; addcc r3,#1
0x02016B4C  tst  r2,#0x80000000 @ bit31 = busy
0x02016B50  bne  0x02016B30
```

39,552 words per 4 M steps = 158 KB per 4 M steps, i.e. the modelled cart
stream runs at ~2.6 MB/s (one word per ~8.7 polls) — a plausible DS ROM rate,
and it is *progress*, not a stall.  The answer to "which event is missing" is
therefore **none**: the guest is mid-load, and the load completes.

## 3. Timeline re-derived in this run (cold boot, frame numbers are mine)

Script `work/p5bm/yes.nnk` = `work/repro_5bi/start_cutscene.nnk` + Right
6400..6406 + A 6420..6426 (`Right+A` = "Yes").  `NNK_GX_SHOT_EVERY=25`.

| display frame | bottom screen | top screen |
|---|---|---|
| 4200 / 4300 | intro film | intro film — **corr 0.9978 / 0.9983** vs `work/p5t/op_u.yuv` f27 / f40 |
| 6400 | skip prompt, "No" highlighted — **dark-band 219 px** | black |
| 6420 | Right+A answered ("Yes") | black |
| 6450 .. 7450 | black (mean 0.0) | **map fades in from 6525** (mean 26.1 → 138.9 at 6575, static) |
| 7475 | first content (mean 29.5) | map |
| **7500** | **dialogue box "Hey, Oliver!"** (mean 72.7) + advance arrow | **town map, label "Norton Street"** |
| 7500 .. 11600 (run end, 6.5e9 steps) | static dialogue, waiting for A | static map |

Measured with the committed `tools/p5bm_postskip_shots.py` (`first-nonblack
work/p5bm/c bot 6450` → `7475 mean=29.5`; `... top 6420` → `6525 mean=26.1`;
`darkband` → 219; `corr` → 0.9978/0.9983).

In steps: the savestate anchor of the same run is `step=4200000000 frame=7508`
and the 6.5e9-step run ends at `frame=11618`, i.e. **559,611 steps per display
frame**; the first post-skip bottom frame 7475 is therefore **step ≈ 4.182e9**
(the answer is at frame 6420 ≈ step 3.59e9, as Phase-5BK measured for the
same input script).

Two PNGs delivered (cold boot, this build):
`work/p5bm/c.07600.bot.ppm.png` — bottom: the parchment dialogue box
"Hey, Oliver!" with the red advance arrow;
`work/p5bm/c.07600.top.ppm.png` — top: the town map ("Norton Street").
Frames 07450 / 07475 / 07500 pin the transition itself.  The map frame 6575 was
viewed as well: a top-down town map with streets, trees and the yellow street
label — not a movie frame and not black.

## 4. Why 5BL reported black: its own last capture already showed the screen

`worktrees/gxstat/work/p5bl/shots3/f.07500.bot.ppm` has **mean 72.7** — the same
dialogue box.  My run reproduces 5BL's binary byte for byte at the same frames:

```
cmp work/p5bm/b.06400.bot.ppm  .../p5bl/shots3/f.06400.bot.ppm  -> identical
cmp work/p5bm/b.07500.bot.ppm  .../p5bl/shots3/f.07500.bot.ppm  -> identical
cmp work/p5bm/b.07500.top.ppm  .../p5bl/shots3/f.07500.top.ppm  -> identical
```

The run budget (4.2e9) stops 9 frames after the transition, and 5BL's
mean-of-frames table evidently stopped at 7450; the picture was on disk.

## 5. Gates (all on this branch, this build)

| gate | result |
|---|---|
| `cd nnktool && cargo test --release` | **65 passed, 0 failed** (64 + the new 5BM test) |
| `SKIP_MELONDS=0 sh tools/verify_phase2a.sh` | **1000 / 1000** on all three melonDS prefix checks, `rc=0` |
| intro corr (top screen vs `work/p5t/op_u.yuv`) | f4200 → f27 **0.9978**, f4300 → f40 **0.9983** (both ≥ 0.99) |
| prompt text, cold boot | frame 6400 dark band (x70..200, y88..104) **219 px** — the 5BI/5BJ number |
| "No" path | `work/p5bm/no.nnk` (A at 6420): the film runs on (bottom 55.9 at 7600 … 57.6 at 8100 — "That must mean he finally finished it…" in the shop) and the next cutscene is on screen at frame **8200** (mean 75.7); run to 5.3e9 steps |
| savestate 5 M → 6 M, `NNK_SAVESTATE_STRICT=1` | fnv `0x5F9DD1AD` crc `0x3ACA5B0A` **both** runs, `identity OK` |
| savestate 30 M → 32 M, STRICT | fnv `0x2AFC0F98` crc `0x3E0F0619` **both** runs |
| speed, 600 M steps headless | **59.43 s** (reference ≈ 61 s) — within 10 % |
| ROM-only | yes — the card image is the only input; `work/` is symlinked, nothing game-derived is committed |

## 6. Honest gaps

* The **bottom screen is blank from 6450 to 7475** while the top screen already
  shows the map from 6575.  Whether real hardware shows a loading indicator on
  the bottom screen during that window is *unverified* — no melonDS capture
  exists at that point of the session (`work/repro_5au/melonds_state.mln`
  predates the prompt).  It is the next question, not a proven defect: the
  guest renders the map and the dialogue itself with the same pipeline.
* `0x0200712C` is modelled as "WFI returns immediately", so the idle thread
  spins instead of sleeping; it costs speed, never correctness, and it is the
  reason a pc sample can make a healthy idle look like a park.
* The `Right` key edge is consumed by the guest (KEYINPUT trace) and the film
  stops 4 display frames after the A press; the answer itself is not modelled
  by us in any way.
