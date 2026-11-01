# Phase-5AI — post-skip black screen: the guest HALTS, and the FIFO drop is only its shadow

Follows Phase-5AG (`8c43c16`).  The operator reported that after skipping the
movie (A|Start) the screen goes black and stays black while the log shows
`ipc: p4r FIFO-full drop ...` and VBlank IRQs still raising.  Two things were
measured, and they are not the same thing.

## 0. Headline

1. **The FIFO drop is NOT a deadlock source here — it is the arithmetic shadow
   of a dead guest.**  The guest drains the ARM9 recv FIFO for the last time at
   **step 334,040,918**, then enters the SDK terminate helper at
   **step 334,058,027** and parks forever in
   `0x02009108` (`bl 0x02008D20` = *disable IRQ*; `loop: bl 0x02009118` = WFI;
   `b loop`) with **CPSR.I = 1**, so no IRQ can ever deliver and nothing can
   drain.  16 recv + 32 host-queue words later — 8.29M cycles at the measured
   5.79 words/1e6 cycles — the first `FIFO-full drop` appears at cycle
   **345,486,197**, exactly where the old log said it would.  Nothing was lost
   *before* the guest stopped reading; nothing can be gained by re-ordering
   words *after* it.  (The operator's own log is the same class: first drop
   `0x00008087` at cycle 1,350,858,994 = step 1,347,648,768, i.e. their run
   halted ~1.34e9 in their own timeline.)
2. **The black screen has a named cause one level up: the guest terminates
   itself ~13 frames after the skip.**  The movie loop is left at
   step 326,353,316 (last presented movie frame, `f589` = the script's Start
   edge), and at step 334,058,027 an overlay-17 state wrapper
   (`0x020DA000`, `blx [vtable+0x50]` with vtable `0x0213B70C`) calls
   `0x02044868`, an NNS/library "set stream state" method, which reads
   `[obj+0x124]` and — because that word is the garbage `0x80808080` — takes
   `bl 0x020090D8` (`if (*0x0206CA14) { *0x0206CA14 = 0; blx fini }; halt`).
   The *sibling* object on the same list (`0x022CC714`) has `[+0x124] = 0`
   and takes the normal path, so the difference is exactly that one field.
3. **Fixed (class 45): the model can no longer lose a produced word.**  The
   Phase-4R producer now back-pressures instead of dropping (the ARM7 "blocks"
   with `next` frozen; 4,096-word backlog), the one-shot reply waits for a
   free recv-FIFO slot instead of writing past `arm9_recv_fifo[16]`, and the
   FIFO-full transition is logged once as `ARM7 send BLOCKED ... 0 words
   dropped`.  Guest behaviour is bit-identical to before (same
   `distinct_pcs=23201`, `hle_hits=7760`, `halt_pc=0x02009114`).
4. **New barrier (named, not fixed): why is `[0x022CC8CC+0x124]` garbage?**
   That field is the only difference between the object that aborts and its
   sibling; the next measurement is a write watch on `0x022CC8CC+0x124` (and
   on `0x022CC714+0x124`) to see which class method (or which *skipped*
   constructor) is supposed to fill it.

## 1. Reproduction — the guest's exact stop

Canonical build/run (see §6), operator session
(`tools/nnk_operator_session.nnk`, whose Start taps at f13.. kill the movie at
f589), `NNK_IPC_PROBE=1` for the drain probe:

```
movie: stream 0 codec=0x0227834C source=op_u.yuv frames=1317 step=323391425
movie: present stream=1 ... frame=6 step=326421022      <- last movie frame
...
irq return  step=334041028 pc=0x02009CF0 cpsr_ctl=0x0000001F   <- LAST IRQ EVER TAKEN
p4r: NO DRAIN for 560190 cyc depth=4  pc=0x0200911C cpsr=0000009F lr=0x02009114
     last_read_step=334040918 step=334601104
p4r: NO DRAIN for ... depth=16 qn=13 pc=0x0200911C cpsr=0000009F ...
dbg1 step=334058027 pc=020448A0 r0=00000000 ... r4=00000080 r5=022CC8CC
     r13=027E3A10 r14=0202B050
halt state: r0=00000000 r1=6000009F r14=02009114 nzcv=0110
done steps=340000001 distinct_pcs=23201 halt_pc=0x02009114
```

* Last recv-FIFO drain: **step 334,040,918** (cycle 337,043,406).
* Last IRQ: step 334,041,028.  `ie=0x00040009`, `ime=1` from then on — the IE
  is irrelevant once CPSR.I is set.
* Guest PC while black (every probe from cycle 422.8M to the 1.4e9 budget):
  **`0x0200911C`** with `lr=0x02009114`, `cpsr=0x0000009F` — the body of
  `0x02009118`/`0x0200911C` (`mcr p15,0,r0,c7,c0,{4}`) inside the
  `0x02009110` loop of `0x02009108`.  That is the SDK terminate/idle helper,
  not a game wait: `0x020090D8` first runs the registered fini callback
  (`*0x0206CA14`, here 0) and then never returns.
* Phase-5F already measured the same helper from 90 s of melonDS emulation
  (ARM9 trace window `0x020090D8..0x020090E0`, ts ≥ 2.16B): **0 hits** — the
  reference never enters it.  Combined with Phase-4R's measured rhythm (the
  ARM9's mailbox loop reads `0x04100000` continuously; the largest ARM9 IO gap
  in the whole card phase is 141,230 ts), melonDS never has to drop a request
  because its ARM9 never stops draining.

## 2. Why the model dropped, and what the drop cost

`nnk_p4r_enqueue()` (tools/nnk_dma.inc, Phase-4R) pushed the three measured
periodic ARM7 words (`0xC0240006` / 560,190 cyc, `0x00008007` / 335,104 cyc,
`0x00008087` / 983,040 cyc = 5.79 words/1e6 cyc) into a 32-entry host queue and
**dropped** on overflow.  Delivery into `arm9_recv_fifo` (16 words) is what
fails first once the guest stops reading, so the queue fills 8.29M cycles
after the last drain — the measured first drop at cycle 345,486,197 is that
number to within the stream phases (48 words x 172.7k cyc).

Nothing the guest consumed was lost: the drops all happen *after* the halt, and
the unfixed and fixed builds produce identical guest state at 1.4e9 steps.
The drop is still wrong in principle (a word the guest may later wait on is
silently gone), so it is fixed rather than tolerated.

## 3. The abort, from the RAM dump at the halt

`NNK_HALT_AT_PC=0x020448A0` + `NNK_DUMP_MEM` gives the exact chain:

```
0x020DA000  (overlay 17)  push {r3,r4,r5,lr}; r5=r0; r3=ldrh[r5+0x130]; r4=r1
0x020DA038                r1=(int16)(r4 + ldrh[r5+0x9C] - r3); blx [ [r5]+0x50 ]
                          vtable 0x0213B70C, slot +0x50 = 0x02044868
0x02044868  (arm9.bin)    if (ldrh[r5+0x9C] == r4) return;      ; state setter
0x02044888                r0 = [r5+0x124]
0x0204488C                beq 0x020448A4            ; NULL -> normal path
0x02044894                r0 = ldrh[r0]             ; *** [r5+0x124] = 0x80808080
0x0204489C                cmp r0, r4 ; bhi 0x020448A4
0x020448A0                bl 0x020090D8             ; terminate
```

measured: `r5 = 0x022CC8CC`, `r4 = 0x80`, `lr = 0x020DA03C`; the object is
`{ [+0]=0x0213B70C vtable, [+4]=0, [+8]=0x022CC714, [+0xC]=0x022781BC
(player A 0x02278264 - 0xA8), [+0x124]=0x80808080 }`.  The sibling
`0x022CC714` (same vtable, same list, `[+8]=0x022CC55C`, `[+0xC]=0x022781BC`)
has `[+0x124] = 0` and is called first with `0x7E` — it takes the `beq` path.
`0x80808080` is not a valid pointer (outside the 24 MiB guest window), so the
runtime's `ld16()` yields 0 and the `<= 0x80` test fires.

The surrounding library is the NNS/stream family in arm9.bin
(`0x0202B040` initialises a 4,096-byte ring, `0x0202B080` flushes,
`0x0202A868`, `0x0202C308`) — the class whose objects are the two movie audio
streams (both carry the player back-pointer `0x022781BC`).

**Difference from melonDS at this point: unmeasurable as a word comparison** —
melonDS's ARM9 never reaches `0x020090D8` at all, and this project has no
input-scripted melonDS run, so the *skip path* itself has no reference.  What
is measurable and measured is that the unscripted path does not abort: the
canonical unscripted run reaches **2.42e9 steps** with
`distinct_pcs=22884`, `hle_hits=11202`, `halt=step budget exhausted` and the
movie band still matching (§5).

## 4. The fix (class 45, `tools/nnk_dma.inc`)

1. `NNK_P4R_QMAX` 32 -> **4096** and the queue is a *backlog*, not a drop ring.
2. `nnk_p4r_schedule()` checks for a full queue **before** enqueueing: on full
   it returns without advancing `next` (the ARM7's send blocks — no word is
   produced and none is lost; the catch-up burst on drain is what hardware
   does), logging `ARM7 send BLOCKED ... 0 words dropped` once on entry and
   `ARM7 send RESUMED` once on exit.
3. `nnk_p4r_enqueue()`'s overflow branch is now the loud
   `INTERNAL queue overflow ... (schedule() must back-pressure first)` — the
   old `FIFO-full drop` wording is gone and the class test forbids it.
4. The one-shot Phase-4Q reply (`0xC0204006`) is gated on
   `arm9_recv_n < 16u`; it used to index `arm9_recv_fifo[16]` with no bound at
   all when the FIFO happened to be full (a real out-of-bounds write).
5. `NNK_IPC_PROBE=1` adds the drain instrument used above: it watches the
   recv-FIFO depth every dispatch, remembers the last observed drain
   (step + cycle), and reports `NO DRAIN for <cycles>` with the guest's
   `pc/cpsr/lr` every 4 frames, plus `drain RESUMED`.

Measured effect of the fix on the reported scenario (1.4e9 steps):
`ARM7 send BLOCKED (FIFO full: qn=4096) ... 0 words dropped step=1044822584
cyc=1047865076` — one line instead of eight drops, **no drop line at all**,
and the guest outcome is unchanged (`distinct_pcs=23201 hle_hits=7760
halt_pc=0x02009114`).

## 5. No regressions (measured this branch)

* `cargo test` in `nnktool`: **46 passed** (45 + the new class-45 test
  `phase_5ai_arm7_producer_backpressures_instead_of_dropping`).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh`: **green, rc=0** — section A
  `RESULT: PASS`, section D `done steps=20000001 distinct_pcs=125`, section F
  strict common prefix **1,000/1,000** melonDS PCs (log `verify2a.log`).
* Unscripted canonical run to **2.42e9 steps**: no `untranslated pc`, no halt
  (`distinct_pcs=22884 hle_hits=11202`), 173 frames captured.
* Movie band (`tools/p5ad_band.py`, shots `shots/p5ai`): top screen
  corr **0.9986..0.9990** against `op_u.yuv`, bottom **0.9991** against
  `op_d.yuv`, MAE ~2.9 luma, at f4175..f4300 — the movie is untouched by this
  phase.
* Post-skip screen state, captured for the record
  (`shots/p5ai_skip`, `NNK_GX_SHOT_EVERY=5`, scripted run to 360M steps):
  the top and bottom screens are **FLAT 0x00 from display frame 585** on
  (single-valued pixel data, checked frame by frame through f645:
  `s.00585..s.00645` top and bottom all `distinct=1 min=0 max=0`) after the
  movie's last frames fade at f570 (mean 210) and f575 (mean 112).  The guest
  never draws again: the last movie frame is presented at step 326,421,022
  (= f583) and the terminate call follows ~11 frames later at f596.
  PNGs: `shots/p5ai_png/p5ai_f560_top_movie.png` (the last movie band),
  `p5ai_f590_top_black.png` and `p5ai_f590_bot_black.png` (the black screen —
  223-byte PNGs, i.e. a single colour).

## 6. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/post-skip-ipc
ln -sfn /home/hermes/NiNoKuni/work work        # gitignored build state
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
  SPLIT_DIR=work/split/c bash tools/build_split.sh out/split/psipc_fix

S=/home/hermes/.hermes/profiles/coder/cache/scratch/psipc
# (a) the reported scenario: drops -> BLOCKED, plus the halt itself
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' \
NNK_MOVIE_YUV=/home/hermes/NiNoKuni/work/p5t/op_u.yuv \
NNK_MOVIE_YUV2=$S/../../p5t/op_d.yuv NNK_GX=headless NNK_IPC_PROBE=1 \
NNK_STEP_LIMIT=1400000000 NNK_INPUT_FILE=tools/nnk_operator_session.nnk \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/psipc_fix work/phase3a/arm9_stored_padded.bin

# (b) the abort chain (registers + object memory + full RAM at the halt)
add NNK_DEBUG_AT_PC=0x020448a0 NNK_DEBUG_N=2 \
    NNK_DEBUG_MEM="0x022CC8CC:0x140" NNK_DEBUG_MEM_PATH=$S/haltmem2.txt
add NNK_HALT_AT_PC=0x020448a0 NNK_DUMP_MEM=$S/mem_halt.bin
(disassemble: arm-none-eabi-objdump -D -b binary -m arm \
   --adjust-vma=0x02000000 --start-address=0x020D9F40 --stop-address=0x020DA080 mem_halt.bin)

# (c) the unscripted 2.42e9 run + movie band
NNK_STEP_LIMIT=2420000000 NNK_GX_SHOT=/home/hermes/NiNoKuni/shots/p5ai/f \
  NNK_GX_SHOT_EVERY=25  (same env, no NNK_INPUT_FILE)
python3 tools/p5ad_band.py <shots> <op_u.yuv> <pngdir> 4000 4300 p5ai 600 top
python3 tools/p5ad_band.py <shots> <op_d.yuv> <pngdir> 4000 4300 p5aid 600 bot
```

## 7. Next bounded step (the new barrier)

Watch writes to `0x022CC8CC+0x124` and `0x022CC714+0x124` (a memwatch over
`0x022CC714..0x022CCA80`) from the movie start (step 323M) to the abort
(334M), and name the method that fills the field on the sibling but not on the
object that aborts.  If it turns out that the field is only ever written by
the *codec setup* that the Phase-5U HLE bypasses, the answer is not "fix the
object" but "the skip path needs the guest's own stream teardown to run" —
which also bounds what an input-scripted melonDS capture would have to show.
