# Phase-5AW — the prompt press, fix 1: an ARM exception return must carry SPSR's T bit

Follows Phase-5AT (merged `064ab93`).  Build:
`NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh work/split/arm9_5aw`
(prelude audit green, `cargo test`: **58 passed**).

## 1. Result

The `0x022CA20C` halt — the one this card measured from the scripted
reproducer — is **gone**: it was not a missing code range but a lost
instruction state.  The press now runs the guest's real save path and stops at
a **new, named barrier**, the operator's own halt frame:

```
done steps=1140846949 traced=1140846949 distinct_pcs=28476 hle_hits=8838
     halt_pc=0x0212CCF6 halt=dispatch to untranslated pc
untranslated pc=0x00000000
halt state: r0=027E3A9D r1=00000000 r2=00000000 r3=00000000 r4=00000000
            r5=00000000 r6=00000000 r7=00000000 r13=027E3C28 r14=0212CCDD nzcv=0010
```

These are the operator's window numbers to the register (`halt_pc`,
`r0=027E3A9D`, `r13=027E3C28`, `r14=0212CCDD`, `nzcv=0010`) — one run earlier
this same script stopped at `0x022CA20C` with a different frame entirely.  The
press is consumed: frame 2000 shows the (separately-known mis-rendered) prompt
overlay, frame 2005 shows the same artwork with the overlay gone and the screen
fading into the save path (§4).  The remaining halt is a **second, different
defect**: the save worker's frame does not exist while its body runs.

## 2. What the 0x022CA20C halt actually was

Trace (`NNK_TRACE_FROM` + `NNK_SMC_TRACE_FROM` around the last 3,000 steps):

```
0x0200729c  subs pc, lr, #4            <- ARM exception return, SPSR=0x2000001F (T=0)
0x01ffc684  (resumed pc = lr-4)
```

* The thread context restored at `0x0200729C` has SPSR **`0x2000001F`**: mode
  SYS, **T = 0**, so the resumed instruction at `lr-4 = 0x01FFC684` is **ARM**.
* `0x01FFC684` is ITCM, live-decoded, and the previous live decode had been a
  Thumb stream.  The exception-return case restored the mode byte and NZCV but
  **dropped the T bit**, so `smc_pick_state()` fell through to the stale sticky
  guess (Thumb):

  ```
  0x01FFC684  ARM   e089314a  add r3, r9, sl, asr #2
              Thumb 314a     adds r1, #0x4A       <- what ran
                    e089     b.n 0x01FFC79C
  0x01FFC79C  Thumb 7087     strb r7, [r0, #2]
  0x01FFC79E  Thumb e040     b.n 0x01FFC822  -> 0x01FFC4EC
  ```

* The ARM epilogue at `0x01FFC4EC` (`pop {pc}` at `0x01FFC548`) then ran
  **without its `push {lr}` prologue** and popped the codec/data pointer
  `0x022C9F38` from the stack.  The run executed the movie codec's data table
  as ARM (`0x02338408`, `0x00000100`, ...) until the data word `0xFFFFFFD3`
  at `0x022CA20C` did not decode.

So the pc was not covered because it was **an instruction state, not a code
range**: `lr-4` is a dynamic target that no compiled case, no ROM-derived
pcform run and no register/stride guess can classify — the restored SPSR is
the only authority (the same one-phase carrier `bx` and `ldmia {pc}` already
use via `nnk_smc_note_branch`).

## 3. The fix (runtime + recompiler, no address special-case)

* `nnktool/src/recompile.rs`: every emitted ARM exception-return block
  (`subs`/`movs pc, lr` with S=1, rd=15, privileged mode) now calls
  `nnk_smc_note_exc_return(pc, _spsr)` after restoring the CPSR byte and NZCV.
* `tools/nnk_smc.inc`: the hook records the carrier
  `smc_branch_mode((target & ~1u) | ((spsr >> 5) & 1u))` and marks it live
  (the SPSR bit is hardware state, so it may overrule a compiled case).  The
  live decoder's own exception return (`smc_exec_arm`) calls
  `smc_branch_mode(*npc | ((spsr >> 5) & 1u))`, so compiled and live code
  cannot disagree.
* `tools/split_dispatch.py` declares the hook in `nnk_rt.h`.
* `tools/p5aw_exc_return_splice.py` applies the same call to the **frozen
  image's** 13 exception-return case bodies (they predate the emitter change);
  `tools/build_split.sh` runs it marker-gated
  (`NNK_PRELUDE_DISABLE_SPLICE=5aw_exc_return_state` is the negative control)
  and the manifest row quotes the splice's own marker string.
* Class test 58 (`phase_5aw_exception_return_carries_the_spsr_instruction_state`)
  pins the emitter call, both runtime call sites, the splice's rewrite and the
  toolchain wiring.  Suite 57 → **58**.

## 4. The new frontier (named): the save worker's body runs with no frame

`0x0212CCF6` is the **shared return thunk** of the Thumb function entered at
`0x0212B93C`, in ov17 mapped at `0x020D0A00` (`ov017.bin` + `0x5AF3C`; verified
by disassembly, not by symbol):

```
212b93c  b5f8  push {r3,r4,r5,r6,r7,lr}
212b93e  b0ff  sub sp, #0x1FC
212b940  b0a7  sub sp, #0x9C          ; frame 0x298 + 6 saved words = 0x2B0
212b95e  f001 f9c8  bl 0x212ccf2       ; <- "return via thunk": Thumb-1 `b` is
...                                   ;    only +-2 KB, so the epilogue is CALLED
212bc2e  f001 f83c  bl 0x212ccaa       ;    (same thunk reached from 0x212cc90's
...                                   ;    body and from 0x212ccaa's tail)
212ccf2  b07f  add sp, #0x1FC
212ccf4  b027  add sp, #0x9C
212ccf6  bdf8  pop {r3,r4,r5,r6,r7,pc}
```

Measured frame arithmetic (DC probes at the three points + the halt state):
`sp = 0x027E3978` at `0x0212BC24` and `0x0212CCAA`, `0x027E3978 + 0x298 =
0x027E3C10` before the pop, and the pop's six words live at
`0x027E3C10..0x027E3C27` — so the prologue's save area is `0x027E3C28 - 24`.
**All six popped zero, so `pc = 0` and the dispatcher reports an untranslated
pc.**  What is *not* a working theory any more:

* **The store never happened.**  `NNK_STOREWATCH=0x027E3C10:0x027E3C28` with
  `NNK_STOREWATCH_N=200000` logs **no store in the whole 33.9 M-step run**,
  while the same watch on `0x027E3800:0x027E3C40` fills 2,000 entries within
  17 k steps — so the watch is live, and it fires *before* the TCM/bus routing
  in `st32()`, i.e. a DTCM- or ITCM-banked store would have been logged too.
  The 24 bytes are also zero in the operator's savestate at the raw address and
  at its 4 MiB fold (`0x023E3C10`), and the whole `0x023E0000..0x023EFFFF`
  block is zero in the halt memory dump (`NNK_DUMP_MEM`).
* **This function is not compiled.**  `0x0212B93C` is not a case in the shipped
  image: pages `0x0212A000..0x0212DFFF` have no case at all (only 55 of ov17's
  pages have any), so the whole body runs in the live SMC decoder
  (`on_demand=334,116,355`).  Its prologue would have gone through the same
  `st32()` the watch sees.
* **`0x020EA750` is not a scheduler resume** (correcting an earlier reading of
  the same trace).  It is `bl 0x201f250` inside the live-decoded ARM helper
  `0x020EA6D4`; the dc hit at `0x020EA770` is that helper's conditional
  `popcc {r3,r4,r5,r6,r7,pc}`, and the token it returns — traced as
  `0x0212BC24` — is the `blx 0x20ea6d4` at `0x212bc20`.  So the body was
  already executing at `0x212bc20` before the traced window.
* **It is not the SPSR carrier either.**  This epilogue is a plain
  multi-register `pop {...,pc}` whose target is the *frame word itself*; no
  exception return is involved.

**So the named frontier is: control is inside the `0x298`-frame body
(`0x212bc20` today) while that frame has never existed — the body was entered
mid-function.**  The only legal entry into this function is
`bl 0x212b93c` at `0x212d250` (the sole external reference in the whole
overlay), and the `push` at its prologue never wrote.  The next task should
start from "who set pc into the middle of `0x212B93C` after the A press"
(the resumed save-worker context: mode/`sp`/`pc` of the thread the game
schedules to write the save), not from the pop.  Evidence to start with:
`mode 3->0 step=1140783986 sp=0x027E3958 lr=0x020EA754` (USR bank restore),
`sp=0x027E3978` inside the body, and the empty `0x023E0000..0x023EFFFF`.

## 5. Measurements

| check | result |
|---|---|
| scripted reproducer (`operator_dialog.nnks` + `press_a_at_frame1990.nnk`), fresh run | old: halt `0x022CA20C` @1,112,592,480 · new: passes it, halts at `0x0212CCF6` @1,140,846,949 with the operator's exact registers (§1) |
| screen after the press | frame 2000: prompt overlay present (mis-rendered — separate card); frame 2005: overlay gone, artwork fading into the save path (`work/phase5aw/after_press.02005.bot.ppm.png`) |
| `cargo test` | **58 passed** |
| `tools/verify_phase2a.sh` | `rc=0`; decoder cross-check `RESULT: PASS`; melonDS-vs-native **1000/1000** strict prefix, 1000/1000 window-filtered, 1000/1000 in-order (`work/phase5aw/verify_phase2a.log`) |
| prelude audit / splice marker | green, `5aw_exc_return_state` row present; the splice is marker-gated with `NNK_PRELUDE_DISABLE_SPLICE=5aw_exc_return_state` as the negative control |
| savestate round trip 5M→6M | `fnv=0xA258DF1B crc=0x4341C3DE` — identical on save and on reload |
| savestate round trip 30M→32M | `fnv=0xAC8F90B0 crc=0x6FFECEF1` — identical |
| ROM-only runtime (clean dir: binary + card image, no `work/`) | 20,000,001 steps, 13,328 distinct PCs, `halt=step budget exhausted` |
| speed | 700 M steps in **66.9 s** (19.1 s / 200 M; 5AT baseline 30.8 s / 200 M) |
| notice screen renders | fresh `notice.01100.bot.png` — the fan-translation text on the parchment background |
| title screen via skip script | see §6 (unchanged pre-existing blocker) |
| movie band corr | see §6 |

## 6. Honest gaps of this task

* **melonDS ground truth could not be loaded here.**  `work/repro_5au/melonds_state.mln`
  is savestate format major **13** (`MELN`, version word `0x0d`); every melonDS
  build in this environment is `SAVESTATE_MAJOR 14` (bumped at `ba317e2e`), and
  the loader refuses a major mismatch.  Fetching an older melonDS is not
  possible offline, so "what does melonDS do after the press" is still
  unanswered — which is exactly the open half of §4 (guest path vs. our path).
* **Title screen skip script was not run.**  The cold-boot path still parks in
  the `0x020078F4` halfword spin (`ldrh/cmp/bne`) long before frame 4,300 — my
  700 M-step speed run ends at `0x020078F8/0x020078FC`, the same loop Phase-5AT
  reported, so the title is unreachable from a cold boot for the same
  pre-existing reason.  `work/phase5at/skip_title.nnk` remains the script to
  use once that wakeup is fixed.
* **The movie band audit was not re-run.**  It needs a fresh intro capture
  (~2.9 e9 steps plus per-frame shots) against `work/p5t/op_u.yuv`; it does not
  fit this task's budget.  The change is one exception-return state note and its
  splice — no movie, codec or YUV code moved.
