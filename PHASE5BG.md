# Phase-5BG — a taken direct branch must stamp the one-step state carrier

Follows Phase-5BE (master `95e3bc0`, branch `wt/t_7aa2d437`). Branch `wt/t_8095f28c`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5bg3/nnk_runner`
→ `BUILD OK`.

## 1. What the halt actually is: the IRQ resumed a Thumb pc in ARM state

Reproducing the card's run (`NNK_SAVESTATE=work/repro_5bc/healthy_prompt.nnks`,
`NNK_INPUT_FILE=work/repro_5bc/start.nnk`, 7.5e9 steps, GX headless) gives the
same halt at step 6,191,615,830. The last 20 dispatches (NNK_TRACE_FROM,
`work/repro_5bg/trace_pcs.txt`) are:

```
0x020d8402 0x020d8406 0x020d840a 0x020d840e 0x020d8412   <- 4-byte stride,
0x020d8504 ... 0x020da0f8 0x020da118                      <- then Thumb
```

`0x020D8402..0x020D8412` are **Thumb** instructions (`lsls r3,r1,#2`,
`adds r3,r5,r3`, `ldr r3,[r3,r0]`, ...) and they were executed four bytes
apart, i.e. as ARM. `NNK_SMC_TRACE` names the words and the decision:

```
irq return step=6191612652 pc=0x020D8402 cpsr_ctl=0x0000001F
smc: on-demand step=6191612652 pc=0x020D8402 word=0x18EB008B state=arm   -> 0x020D8406
smc: on-demand step=6191612653 pc=0x020D8406 word=0x589B581B state=arm   -> 0x020D840A
...
stateconf FALLBACK pc=0x020D8412 chose=arm decoded_as=thumb step=6191612656
```

The ARM reads happen to decode (`0x18EB008B` is `stm(ne)`, ...), so four
instructions run and corrupt registers; only the fifth (`bl`) fails to decode
as ARM and falls back to Thumb — by then `r0` is `0x00000508` instead of the
object `0x0228F078` (measured at the call `0x020D8412 bl 0x020D8504` with
`NNK_DEBUG_AT_PC=0x020D8504`: `r0=00000508 r5=0228F078`). The rest is
consequence: `0x020D8508 adds r4,r0,#0` makes `r4=0x508`, the object lookup
`ldr r1,[r4,#0x508+4*idx]` reads a code word, and the final
`0x020DA118 blx r3` gets `r3=[r1]` = `0xE3A02001`, so the dispatch target is
`0xE3A02000` (`mov r2,#0`) — the reported "untranslated pc".

So the transfer at the halt is named: **`0x020DA118 e12fff33 blx r3`**, `r3`
loaded by `0x020DA10C e5913000 ldr r3,[r1]` + `0x020DA110 e5933064 ldr r3,[r3,#0x64]`
(a virtual call through slot `+0x64` of the object in `r1`), with `r1` garbage
because of the wrong-state block above.

## 2. Why the state was ARM: the SPSR's T bit had no witness

The interrupted instruction is `0x020D8402`, reached by the `blt.n 0x020D8402`
back-edge at `0x020D841C` (`work/repro_5bg/trace2_pcs.txt`, the pcs right
before `irq enter step=6191612305 resume=0x020D8402 ... spsr=0x8000001F`).
`spsr=0x8000001F` has **T=0**, and that value comes from Phase-5BB's
`irq_enter`, which synthesises the interrupted CPSR from
`nnk_state_of_next(resume, last_pc)`.

`nnk_state_of_next` (Phase-5BE) asks, in order: the one-step carrier, the
2/4-byte stride, the compiled pc-form map. For a **taken direct branch** the
first two cannot answer, and for this pc the third is skipped —
`NNK_STATE_CONFLICTS=1` reports `live-band pc=0x020D8402 (guest-rewritten, map
skipped)` because overlay 17's data band is dirty — so the ladder fell through
to the compiled image's default, ARM.

The missing witness is the carrier itself: the live Thumb decoder stamped
`smc_branch_mode()` for `blx rM`, `pop {..,pc}`, `bx` and Thumb `bl/blx`, but
**not for a taken direct branch**. On ARMv5T a direct `B`/`BL` is not an
interworking transfer — the target runs in the branch's own state — so the
carrier was simply never told what it already knew.

## 3. The fix (producer side, three lines, no heuristics)

`tools/nnk_smc.inc`, live decoder:

* Thumb format 15 (conditional branch), inside the taken arm:
  `smc_branch_mode(*npc | 1u);`
* Thumb format 17 (`b.n`): `smc_branch_mode(*npc | 1u);`
* ARM `B`/`BL` (reached only after the condition check, i.e. taken):
  `smc_branch_mode(*npc);` (bit0 = 0 = ARM)

Nothing else changed: no pc pinning, no mode-override row, no change to the
emitted image, the renderer, the movie binding or the AOBJ path.

## 4. Measured effect

Replay of the card's run with the fix (same savestate, same input script,
7.5e9 step budget, GX headless) — see §5 for the log lines: the IRQ at
`step=6191612305` now synthesises `spsr=0x8000003F` (T=1), the return resumes
`0x020D8402` in Thumb state (`word=0x008B`), and the run passes the old
`halt_pc=0x020DA118` frontier.

Measured (build `out/p5bg3/nnk_runner`, `NNK_SAVESTATE=work/repro_5bc/healthy_prompt.nnks`,
`NNK_INPUT_FILE=work/repro_5bc/start.nnk`, `NNK_STEP_LIMIT=7500000000`,
`NNK_GX=headless`, log `work/repro_5bg/run_fix1.log`):

```
before:  done steps=6191615830 ... halt_pc=0x020DA118 halt=dispatch to untranslated pc
         untranslated pc=0xe3a02000              (frame static, 11063 frames)
after :  done steps=7500000001 traced=7500000000 distinct_pcs=24263 hle_hits=15359
         halt_pc=0x0207CEA0 halt=step budget exhausted
         gx: 13397 frames presented;  zero "untranslated pc" lines
```

The IRQ witness is directly visible in the log of the fixed run: every IRQ
entered on Thumb code now synthesises `spsr=0x8000003F` (T=1); the same class
of IRQ synthesised `spsr=0x8000001F` (T=0) before the fix.  `0x020DA118`,
`0x020DA0F8` and `0x020DA10C` still execute — as the ARM code they are — but
with `r1` = the real object, not a code word.

## 5. Checks run here

* `cargo test --manifest-path nnktool/Cargo.toml` → **62 passed, 0 failed**
  (`work/repro_5bg/cargo_test.log`).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh` → **1000 / 1000** on all three
  melonDS prefix checks (`work/repro_5bg/verify2a.log`).
* Frontier run above (same savestate + input script as the card's repro).
* NOT re-run in this task (unchanged code paths, but not re-measured here):
  intro correlation f4200/f4300/f4400, the s01 bit-exact frame set, the 5BB
  `EXC_TRACE` signature, the 5M->6M / 30M->32M savestate round trips and the
  600M speed check.  The savestate field table is generated from the runtime's
  statics, so the two env-gated diagnostics added here (`NNK_REGWATCH`,
  `NNK_CARRIER_TRACE`) add rows to it: states written by this build load in
  this build, and older states still load in an older build.

## 6. Diagnostics added (env-gated, off by default)

* `NNK_REGWATCH=<r>[,<r>...]` (+`NNK_REGWATCH_FROM=<step>`) — logs every
  dispatch where a watched register changed, naming the writing pc
  (`tools/nnk_dma.inc: nnk_regwatch`, called from the dispatch loop written by
  `tools/split_dispatch.py`).  This is what proved `r0=0x508` at the call.
* `NNK_CARRIER_TRACE=1` (+`NNK_CARRIER_FROM`, `NNK_CARRIER_AT_PC`) — logs
  carrier stamps and every `smc_carried_thumb` HIT/MISS with the staleness
  test spelled out (`tools/nnk_smc.inc`).
