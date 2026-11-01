# Phase-5U — B1 refuted, B2 root-caused: the guest parks with interrupts masked

Follows Phase-5T (merged `a8ea125`).  The card named two blockers: **B1** the
display clock "stalls" at the movie start, **B2** the player does not iterate
the decode callback per frame.  This phase measured both with a per-instruction
probe and one melonDS A/B, and the answer is asymmetric: **B1 does not exist**
(it was the step-budget boundary), and **B2 is a guest interrupt stall the
runtime cannot break** — the player's thread is parked inside the SDK
scheduler with `CPSR.I = 1` while enabled IRQs stay pending forever.

## 1. The probe (tools, all off by default)

| piece | what it measures |
|---|---|
| `tools/nnk_smc.inc` — `p5u_hook/p5u_track/p5u_report` | per-sample `step/cycles/vcount/gxd_frames/pc`, the player+codec witness words, `r0..r3/lr/sp`, and `dispstat/ie/iflags/ime/cpsr`; a PC histogram of the window and a callback entry/return count.  Env: `NNK_P5U=1 NNK_P5U_FROM=N NNK_P5U_EVERY=N NNK_P5U_TOP=N NNK_P5U_MEM=prefix`. |
| `tools/p5u_probe_splice.py` | splices the `p5u_hook(pc)` call into a **frozen** image's `trace_pc()` (the one function both the compiled parts and the live re-decoder call), the Phase-5R/5Q splice pattern.  Without it the histogram only saw live-decoded PCs — the first probe run reported 0 samples, which is itself the measurement that the movie window runs mostly *translated* code. |
| melonDS `MELONDS_ARM9TRACE_*` ts window | the callback cadence and the IRQ cadence in the same program window. |

Runs (all `NNK_GX=headless`, `NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin"`):
`p5u4` (2.30e9, probe), `p5u5/p5u6` (2.32e9, probe + IE/IF), melonDS
`ts 2.25e9..2.40e9` at `0x01FF8540` and `ts 2.27e9..2.29e9` at `0x01FF8000`.

## 2. B1 is refuted: the display clock never stalls

```
p5u4 stderr, end of run:  gx: 4112 frames presented, 753487 unhandled io writes
p5u6 stderr, end of run:  gx: 4148 frames presented, 753487 unhandled io writes
```

* The tick keeps rendering to the very end: 2.30e9 steps / 4112 frames =
  **559,143 steps per frame**; 2.32e9 / 4148 = 559,305.  The nominal rate is
  `CYCLES_PER_SCANLINE * SCANLINES = 2130 * 263 = 560,190`, so the clock runs
  at 99.8 % of nominal with the last frames added at `nnk_display_close`.
* The captures also continue **past the movie start** (probe window
  `2.256e9..2.32e9`, display frames 4033..4147) — they are simply black:

```
shots2/f.03980.top.ppm  mean=224.5  (the pre-movie white frame)
shots2/f.04020.top.ppm  mean=224.5
shots2/f.04100.top.ppm  mean=  0.0  <- movie started at frame ~4028
shots2/f.04110.top.ppm  mean=  0.0
```

* **Why Phase-5T read a freeze:** its budget was 2.33e9 steps and
  `4160 * 560,190 = 2.3304e9` — the "frozen frame 4160" is exactly the budget
  boundary.  There is no `cost = 0` ITCM path: `trace_pc()` charges `cycles++`
  for every dispatched instruction, compiled or live-decoded, and
  `nnk_smc_exec_any` calls it before executing.

## 3. B2 root-caused: the player's thread is asleep with `CPSR.I = 1`

The callback itself still fires exactly where Phase-5T put it, and then stops:

```
p5u6 report: window=[2256000000,2320000001] cb_entry=2 cb_ret=2 samples=33
hot #1 pc=0x02008D2C hits=1097539      (SDK OS_EnableInterrupts/restore)
hot #2..11 0x02008D20..0x02008D48       (the disable/enable + CPSR-mode helpers)
hot #12/13 0x02006E90/0x02006E94        (OS_SelectThread list walk)
hot #14..19 0x020067E0..0x0200680C      (OS_RescheduleThread)
```

So over 64 M steps (114 display frames) the guest runs **one** 58-instruction
loop 1,097,539 times: the SDK scheduler is rescheduling with **no ready
thread**.  The state at every sample in that window:

```
pc=0x02006DAC disp=0008 ie=00040018 if=000C0019 ime=1 cpsr=9F
```

* `dispstat = 0x0008` — DISPSTAT bit 3 (VBlank IRQ enable) **is** set;
* `ie = 0x00040018` — bits 3 (Timer0), 4 (Timer1), 18 (IPC) are enabled; the
  guest **cleared bit 0 (VBlank)** at the movie start (boot had `0x00040019`);
* `iflags = 0x000C0019` — bits 0, 3, 4, 18, 19 are **pending**, so
  `ie & iflags = 0x00040018` is non-zero and `ime = 1`;
* `cpsr = 0x9F` in **32 of 32** samples after the movie start (0x1F before it):
  **I is set**, so `!(cpsr_ctl & 0x80)` in the delivery gate is false and not
  one interrupt is delivered.

Measured delivery history in the same run: **21,971 IRQ entries / 21,971
returns**, the last one

```
irq enter step=2256303742 resume=0x01FFADB4 vector=0x01FF8000 ie=0x00040019 if=0x00080010
irq return step=2256303812 pc=0x01FFADB4 cpsr_ctl=0x0000001F mode=0
```

i.e. an IRQ taken **inside** the second decode step, returning cleanly with
`I = 0` — and then nothing for the remaining 64 M steps.  The guest disables
interrupts somewhere in the movie player's first iterations and never runs the
matching restore (every sample's disable call returns `r0 = 0x80`, i.e. the
interrupts were *already* masked when the nested disable was entered).

### The same window in melonDS (the A/B that makes this a divergence, not a norm)

```
callback 0x01FF8540 entries, ts 2.25e9..2.40e9: 106 entries over 127.3 M ARM9
  cycles (~113 display frames x 1.12 M) => 0.94 callback entries per display frame
IRQ dispatcher 0x01FF8000 entries, ts 2.27e9..2.29e9: 243 entries over 20 M
  cycles (~17.8 display frames) => 13.7 interrupts per display frame,
  median inter-IRQ delta 70,014 cycles
```

Native: **0.018 callback entries per display frame and 0 interrupts** in the
same window.  So the movie really is interrupt-driven at ~60 Hz and up, and
the native runtime stops supplying interrupts 47 k steps into the second
decode step.

### What is *not* the cause (checked, so the next phase need not)

* the VBlank raise is modelled: `tools/split_dispatch.py` splices the Phase-5F
  producer (`if (p5f_vc == 192u && (dispstat & 0x8u)) iflags |= 0x1u;`) into
  `trace_pc`, and `iflags` bit 0 *is* pending in every sample;
* rule-based delivery is modelled (Phase-4N splice: `irq_trig_n >= IRQ_TRIG_N
  || steps > irq_trig_log[...]`), and `IME = 1`;
* the callback is not lost: `cb_entry = cb_ret = 2` (its return path at
  `0x0207B21C` is taken, and the HLE present hook ran twice);
* the codec ring **is** populated after the first decode
  (`codec[0C]=0x02278800 [10]=0x022D28A0 [20]=0x0228A820`), so Phase-5T's
  "ring slots still zero" was read at the entry, before the guest filled them.

## 4. The exact remaining gap (one measurement away) and the follow-up

The stall is the guest's own masked state: `CPSR.I = 1` from step ~2.2563e9
onwards.  Two candidates remain, and one run separates them:

1. **the restore is decoded but never reached** — a critical section held by
   the thread that the missing interrupt would release (deadlock, the DQ8
   "consumer never runs" shape); or
2. **an `msr CPSR_c` in live-decoded code does not clear I** — the compiled
   `Kind::Msr` path sets `cpsr_ctl = ctl & 0xFF` (`recompile.rs:482`,
   `:1004`), but `tools/nnk_smc.inc` has **no `msr` case at all**, so an `msr`
   that is only ever reached through the live re-decoder is a decode failure
   away from leaving `I` set.

**Named follow-up (bounded, ~2 runs):** store-watch the runtime's `cpsr_ctl`
(the existing `NNK_DUMP`/`p5u` sample already prints it every 2 M steps — drop
it to `NNK_P5U_EVERY=100000` and `NNK_P5U_FROM=2256200000`) and log the PC of
the last `I 0 -> 1` edge and of every `msr` executed through `nnk_smc_exec_any`
(`NNK_SMC_TRACE` names the PC + word).  If the edge writer is the disable
helper and no matching restore exists, the fix is the DQ8 recipe at the NNK
site: emulate the ladder side-effect — post the event the player waits for
from the decode-step hook (`p5t_present`) and let the guest run its own
scheduler; if instead an `msr` arrives through the live decoder, implement
`msr CPSR_c` in `smc_exec_arm` (four lines, and a new decoder class).

No fix is claimed here: the acceptance ("the intro plays") is **not met**.

## 5. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 33 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh
RESULT: PASS
```

Class 19 (`runtime_interrupt_model_reaches_the_vblank_and_stops_at_cpsr_i`)
locks the three facts a future fix must keep: the Phase-5F VBlank producer,
the Phase-4N rule-based delivery, and the `ime && (ie & iflags) && !I` gate —
plus the Phase-5U probe and its `trace_pc` splice.

`NNK_P5U` unset leaves the runtime byte-for-byte unchanged (one call through a
function that returns immediately, and one `getenv` on first dispatch).

## 6. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5u
python3 tools/p5u_probe_splice.py work/phase3a/arm9_thumb_ovl_itcm_p5s2.c \
        work/phase3a/arm9_thumb_ovl_itcm_p5u.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
  SPLIT_DIR=$S/split bash tools/build_split.sh $S/p5u
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2320000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_MOVIE_YUV=$PWD/work/p5t/op_u.yuv NNK_GX_SHOT=$S/shots/f NNK_GX_SHOT_EVERY=10 \
    NNK_P5U=1 NNK_P5U_FROM=2256000000 NNK_P5U_EVERY=2000000 NNK_P5U_MEM=$S/probe \
    $S/p5u work/phase3a/arm9_stored_padded.bin
# melonDS A/B (the callback cadence and the IRQ cadence)
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
    SDL_AUDIODRIVER=dummy \
    MELONDS_ARM9TRACE_PATH=$S/mel/cb.txt MELONDS_ARM9TRACE_LO=0x01FF8540 \
    MELONDS_ARM9TRACE_HI=0x01FF8548 MELONDS_ARM9TRACE_TSLO=2250000000 \
    MELONDS_ARM9TRACE_TSHI=2400000000 MELONDS_ARM9TRACE_N=200000 \
    timeout 600 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS 'Ni No Kuni.nds'
```
