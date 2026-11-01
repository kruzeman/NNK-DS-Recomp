# Phase-5V — the CPSR.I stall: the writer is measured, the scheduler block is not

Follows Phase-5U (`5438263`, merged `ae109b3`).  Phase-5U left one measurement
away two candidates: (1) a critical section whose restore is never reached, or
(2) an `msr CPSR_c` that the live re-decoder never applies.  This phase built a
probe that sees **every** CPSR-control write in the run and answers it, and in
doing so moved the problem one level: the I bit is held by the SDK's own
sleep/yield critical section, and the *thread switch* that is supposed to end
it never happens because the SDK's thread bookkeeping has been destroyed.

## 1. The probe (one funnel, three injections)

| piece | what it measures |
|---|---|
| `tools/p5v_i_probe_splice.py` | splices three probes into a **frozen** image's runtime (the Phase-5Q/5R/5U splice pattern; the image is not regenerated). |
| `p5v_i_edge()` inside `cpsr_set_ctl()` | every architectural CPSR-control write, with the writing PC in `last_pc`.  `cpsr_set_ctl` is the single funnel: the compiled `Kind::Msr` emitter, the live decoder's `msr` CPSR_c / `msr` SPSR_c cases, the exception entry (`cpsr_set_ctl(0xD2u)`) and the exception return (the SPSR restore).  Logs **only I-bit transitions**, so the line count is the number of critical-section edges.  Env: `NNK_P5V=1 NNK_P5V_FROM=<step> NNK_P5V_N=<lines>`. |
| `p5v_sched()` from `trace_pc()` | at the yield loop `0x020070B4` and at `OS_RescheduleThread` `0x020067E0`: the OSi_ThreadInfo header (`0x0206C684` +0/+4/+8/+0x20/+0x24/+0x28), the current-thread chain, the ready-queue walk (node state +0x64, next +0x68) and the caller's registers.  Env: `NNK_P5V_SCHED=1 NNK_P5V_SCHED_FROM=<step>`. |
| `p5v_watch()` from `st32()` | every 32-bit store into the OSi_ThreadInfo block `0x0206C680..0x0206C6B0` with PC/step/registers (header words unconditionally, the current-thread slot only when the stored value is implausible, so normal scheduler churn cannot eat the line budget).  Env: `NNK_P5V_WATCH=1 NNK_P5V_WATCH_FROM=<step>`. |

Unset, all three are one predictable branch; the runtime is otherwise unchanged.

Reproduce:

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5v
python3 tools/p5v_i_probe_splice.py work/phase3a/arm9_thumb_ovl_itcm_p5u.c $S/p5v_img.c
NNK_GX=1 NNK_IMAGE_C=$S/p5v_img.c bash tools/build_split.sh $S/p5v
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2257200000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_MOVIE_YUV=$PWD/work/p5t/op_u.yuv \
    NNK_P5V=1 NNK_P5V_FROM=2256303800 \
    NNK_P5V_SCHED=1 NNK_P5V_SCHED_FROM=2256300000 \
    $S/p5v work/phase3a/arm9_stored_padded.bin
```

## 2. The exact I-bit history (measured)

Window `2,256,200,000 .. 2,258,000,000` (1.8 M steps, the movie start plus the
park).  Seven transitions, in order:

```
1->0 step=2256303812 pc=0x01FF80B4 ctl=D2->1F lr=0x01FF9F54   (IRQ return; last interrupt ever delivered)
0->1 step=2256355776 pc=0x02008D28 ctl=1F->9F lr=0x020075A8   (OS_DisableInterrupts)
1->0 step=2256355838 pc=0x02008D40 ctl=9F->1F lr=0x020075E4   (OS_RestoreInterrupts)  balanced
0->1 step=2256356058 pc=0x02008D28 ctl=1F->9F lr=0x020076E8   (OS_DisableInterrupts)
1->0 step=2256356133 pc=0x02008D40 ctl=9F->1F lr=0x020077A8   (OS_RestoreInterrupts)  balanced
0->1 step=2256356359 pc=0x02008D28 ctl=1F->9F lr=0x02007070   (OS_DisableInterrupts)  **last CPSR write of the run**
```

and then **nothing** for the remaining 1.6 M steps: no `msr`, no exception
entry, no exception return.  So the `CPSR.I` that blocks delivery is a single
unreleased `OS_DisableInterrupts` taken by the function at `0x02007044`
(`bl 0x02008d20` at `0x0200706C`); the matching `OS_RestoreInterrupts` at
`0x020070CC` is only reached through the `beq 0x020070C8` at `0x020070AC`,
i.e. when the wait flag is already zero.

Candidate (2) of Phase-5U is therefore **refuted**: the live decoder's `msr`
path is not involved (no `msr` is executed at all after `2,256,356,359`, and
the three transitions that do appear go through the *compiled* `Kind::Msr` at
`0x02008D28` / `0x02008D40`).

## 3. The scheduler is the broken part — and it is not "no ready thread"

The yield loop (`0x02007044`: `while (flag) { OS_SleepThread(0); }`, the yield
body `0x02006DA0` = disable / set state / `bl 0x020067E0` / restore) parks with
`CPSR.I = 1` as the SDK intends: the thread switch is what re-enables
interrupts, not a restore in this frame.  So the question is why the switch
never happens.  Dumped at step `2,256,356,590`:

```
p5v: park#1 step=2256356590 cpsr=9F ime=1 ie=00040018 if=00080000
     G0=00000000 G4=00000000 G8=00000000 G20=00000000 G24=00000000 G28=00000000
     r0=E92D4000 r1=8000009F r2=8000009F lr=0200871C sp=027E3928 [sp+4]=E92D4000
```

versus the same dump at step `18,846,719` (boot, healthy):

```
p5v: park#1 step=18846719 G8=0206C6A4 G20=0207981C G24=02079200
     p8=0x0206C6A4 cur=0x0207981C
     ready[0]=0x02079200 st=0 next=0x02079750 ... ready[7]=0x0206F540 st=1 next=0x0206C6B0
```

Two measurements, both new:

* the **entire OSi_ThreadInfo header** (`0x0206C684` +0, +4, +8, +0x20, +0x24,
  +0x28) reads **zero** at the park: the current-thread pointer slot
  (`0x0206C6A4`, the value `[0x0206C68C]` points at) and the ready-queue head
  (`+0x24`) are gone.  So the scheduler has no current thread and no runnable
  thread — `OS_RescheduleThread` correctly does nothing, the yield returns
  immediately, and the caller's stale wait flag keeps the loop spinning.
* the **stale wait flag** the loop tests holds `0xE92D4000` — the ARM encoding
  of `push {lr}`, a code/stack word, where a thread pointer belongs.  The
  current-thread slot was therefore overwritten with a code word *before* the
  block was zeroed, so this is memory corruption, not a legitimate "all threads
  blocked" state.

## 4. Where it stops (the honest gap)

The I-bit history, the funnel argument, the park state and the corruption are
measured; **the writer of `0x0206C68C` / `0x0206C6A4` is not yet named**, so no
fix is claimed and the acceptance ("the intro plays") is **not** met.

One bounded follow-up run has already been spent narrowing it: the targeted
store watch (`NNK_P5V_WATCH=1 NNK_P5V_WATCH_FROM=2250000000` — the block header
unconditionally, implausible writes to the current-thread slot with
PC/lr/r0-r3) ran to `2,257,200,000` and logged **zero** hits in
`2.25e9..2.257e9`.  So the thread block was corrupted and zeroed **earlier**
than 2.25e9 — the park merely inherits it (which is also why the stale wait
flag holds a code word: it was loaded from the already-corrupt current-thread
pointer).

Named follow-up (one run, bounded): the same binary and probe with
`NNK_P5V_WATCH_FROM=19000000` (right after the boot park, where the block is
measured healthy) and `NNK_STEP_LIMIT=2257200000` names the writer instruction
with its PC/lr/r0-r3; the probe's line budget already filters normal scheduler
churn, so the writer appears in the first lines after the corruption.  Then:

* if the PC is a guest store with a bogus *address*, the divergence is the
  instruction that computed it (decoder class, fix in `nnk_smc.inc`/emitter);
* if the PC is a guest store with a legitimate address and a bogus *value*, the
  divergence is upstream in the value (the register chain the probe prints);
* if it is a DMA (`st32` from `nnk_dma.inc`), the card/DMA destination computed
  by the runtime model is wrong.

Until that is fixed, `CPSR.I` stays set and no interrupt can be delivered, so
the intro cannot play.

## 5. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 34 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
```

Class 20 (`cpsr_i_transitions_are_observable_through_the_single_ctl_funnel`)
locks the funnel invariant (one `cpsr_set_ctl` per runtime text, the compiled
and live `msr` paths, both exception CPSR paths) and the three probe injections,
so a future fix cannot lose the measurement.
