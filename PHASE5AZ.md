# Phase-5AZ — the "wait loop" was a MODE bug: an ARM `bl` ran its target as Thumb

Follows Phase-5AW (merge `9e0429e`).  Build:
`NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5az/nnk_runner_c`
(prelude audit green, `cargo test`: **60 passed**, `verify_phase2a.sh`: **1000/1000** on all
three prefix metrics).

## 1. Headline

The card's premise ("the boot parks in the 0x02007134..0x02009120 wait loop") is
**falsified twice over**:

1. **The decoded branch that "correctly" went backwards at 0x020033C2 was itself the
   bug.** `0x020033C2` is not an instruction boundary: `0x020033C0` is, and it is
   **ARM** (`ldr r1,[pc,#8] / ldr r12,[pc,#8] / mov r2,#0x10 / bx r12`, literals
   `0x04000330` = the 3D **EdgeTable** register block and `0x02009B24` = the halfword
   copy).  5AW's `b.n` sign-extension fix was correct *as a Thumb decode* — but the
   bytes were being decoded as Thumb at all because the state was unknown.
2. **The loop is the SDK idle body, not a hang**: the guest leaves it every time the
   ARM7 IPC reply arrives and runs a real frame of work; over a 2.4 G-step budget it
   reaches the game's actual screens (§3).

## 2. Root cause: no state carrier for a direct branch

Cold boot, step **39,238,544** (trace `NNK_TRACE_FROM=39238490`, the exact window the
card names):

```
0x020fbea0 .. 0x020fbeac        ov17 ARM routine (4-byte strides)
0x020fbeac  ebfc1d43  bl 0x020033C0
0x020033c0  0x1008    asrs r0,r1,#0     <- OLD: 2-byte step => decoded THUMB
0x020033c2  0xE59F    b.n 0x02002F04    <- lands 2 bytes into an ARM epilogue
0x02002f04  0xE3550000 ...              <- addeq sp,sp,#0x30 / popeq {r3,r4,r5,pc}
0x020d23e2                              <- garbage popped return address
```

* `0x020033C0` is a **hole** in the ROM-derived pcform map
  (`work/phase5ap/nnk_pcform_map.h`: no thumb run, no arm run covers it) and the
  compiled image carries no case for it, so the dispatch falls to the live decoder and
  `smc_pick_state()`.
* ov17's arena band is guest-loaded, so `smc_map_live_ok(prev_pc)` is false there, and
  an ARM `bl` sets `pc` directly — correctly no `nnk_smc_note_branch()` (a direct
  branch is not an interworking transfer).
* Every authority was therefore silent and `smc_infer_state()` fell through to
  `smc_t_sticky`, the state of some unrelated earlier live decode.  It said Thumb.

**Fix** (`tools/nnk_smc.inc`, `smc_state_from_branch()`, manifest row
`5az_branch_state_inherit`): on ARMv5T a direct `B`/`BL` **preserves** the processor
state; only interworking transfers (BX/BLX/pop{pc}/LDM{pc}) and exception returns
change it, and all of those already carry the successor's bit0/T state into the decoder
(`smc_branch_mode` / `nnk_smc_note_branch` / `nnk_smc_note_exc_return`).  So a pc
reached by a non-fall-through transfer inherits its predecessor's state, taken from the
ROM pcform map when it speaks and from the compiled image's own form map
(`nnk_pc_is_thumb`) otherwise.  Order of authority: branch-register → predecessor
stride/fresh live decode → **branch-predecessor** → sticky.

After the fix the same window runs the true ARM sequence
`0x020033C0 → 0x020033C4 → 0x020033C8 → 0x020033CC (bx r12) → 0x02009B24` and returns
to the ov17 caller.

Measured effect on the cold boot (600 M steps): `distinct_pcs` **16,486 → 19,460**,
card transfers **9,093 → 9,542** starts with the stream continuing to step
**42,034,593** (was 38,978,350), and the VRAM bank map becomes a real one
(`vrc: A=81->ABG[0] ... H=82->BBGEXT[0..3]`, was `E=81->ABG[0..3]`).

## 3. The screens are real now (PNG evidence)

2.4 G-step cold boot, frames every 100 (`NNK_GX_SHOT`), both screens:

| frame | top | bottom |
|---|---|---|
| 3600 | black | mean 34.2 (blank) |
| 3700 | **white 255** | **white 255** |
| 3800 | 247.6, 13 luma | **"Produced and directed by LEVEL5 / http://www.level5.co.jp"** |
| 3900 | **Mobiclip "VIDEO CODEC" logo** (48 luma) | mean 233 (text) |
| 4200 | **movie frame** (245 luma) | 251 luma |

`shots/p5az_f03900_top.png` (Mobiclip logo), `shots/p5az_f03800_bot.png` (LEVEL5
credit) and `shots/p5az_f04200_top.png` (movie frame).  So the boot no longer stops at
white→black: it walks the real boot chain into the intro.

## 4. The idle loop is exonerated as a "hang"

`NNK_WFI_PROBE`/`NNK_WFI_LO` at 550 M+: `ime=1 ie=0x00040019 if=0x000C0000
cpsr=1F` — enabled bits 0 (vblank), 3/4 (TM0/TM1), 18 (IPC recv).  The loop is woken
by IF bit 18 (the ARM7 reply): every wake runs the DISPSTAT scanline wait
(`0x02040A90..0x02040AA4`), the frame body (`0x02040AA8..0x02040D70`), the ITCM text
renderer (`0x01FF8058..`) and the IPC FIFO read (`0x0200BB44`), then returns to idle.
So the loop is a *frame-paced* wait, and over a long budget it terminates on its own.

`tm0=0x0000/00C1 tm1=0x0004/00C1` shows both timers armed with their overflow IRQ
enabled (CNT_H bit7|bit6|/64) while the prelude raised no timer IRQ.  A melonDS-faithful
TM0-3 overflow producer (`NDS::TimerStart/RunTimer/HandleTimerOverflow`) was implemented
and measured on this branch: it does raise IF bits 3/4, and the boot is **unchanged**
(`distinct_pcs` 19,457 vs 19,460, same 9,542 card starts, same 1,077 frames at 600 M)
while costing 13 % speed (56.6 s → 64.0 s).  It is therefore **not** shipped: the timer
is exonerated as this barrier's producer, and an unverified behavioural change that
slows the runner would be a regression by this card's own speed criterion.

## 5. Verification

* `nnktool && cargo test`: **60 passed** (new class test
  `phase_5az_a_direct_branch_inherits_the_predecessor_state`: the two readings of
  `0xE59F1008`, the -0x4C2 displacement to 0x02002F04, the fragment's rule, and the
  inference order).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh`: **1000/1000**, 1000/1000, 1000/1000.
* state hash at 30 M **unchanged** against the pre-fix build
  (`fnv=0xF2541500 crc=0x01022AED bytes=30695088`) — the verified prefix is untouched;
  the fix only fires at step 39,238,544.
* ROM-only runtime (only the `.nds` on the command line), 600 M steps in 56.6 s
  (~10.6 M steps/s, inside the 67 s / 700 M budget).
* `SKIP_MELONDS=1` prefix check and the 60-test suite were run on the committed tree
  (the timer experiment was reverted before the commit).
