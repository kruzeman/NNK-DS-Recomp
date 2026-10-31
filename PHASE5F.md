# Phase-5F — the card-IRQ producer is already unbounded; the WFI loop is starved of IE bit 19, and the guest then parks in a melonDS-only-never-reached halt helper

Follows Phase-5E (merged `d0923ca`).  Scope: make the card-IRQ producer unbounded
so the ARM9 WFI loop `0x02009110` wakes past step 9,097,801, inventory the
remaining finite replay tables, and reach the melonDS frames-102 content.

**Headline (measured, not inferred):**

1. **The premise is falsified.**  IRQ *delivery* has been unbounded since
   Phase-4N (`!irq_active && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])`):
   this run delivers **263** IRQs, the last at **step 39,192,472**, i.e. 30.1M
   steps past the last `irq_trig_log` entry (9,097,801).  The scripted table
   never starved the loop.
2. **The real first differing word is IE bit 19.**  At the ARM9 WFI helper
   `0x02009118` (step **23,490,852**, `cpsr=0x1F` = IRQ unmasked, `ime=1`)
   native has `IF=0x00080000` (bit 19 pending, raised by the card transfer end)
   but `IE=0x00040019` (**bit 19 not enabled**) so `IE&IF = 0`: every WFI returns
   immediately.  melonDS at the same guest point (`p5f/wfi3.txt`, first 24 wakes
   from `ts=56,548,252`, the same card stream) has `IE=0x000C0009` — bit 19
   **set** — and takes the card IRQ (`enabled=0x00080000`).
3. **Second barrier, named:** after the card stream stops the guest enters
   `0x020D3250 -> 0x020090D8`, the SDK idle/halt helper, whose body
   (`0x02009108`: `bl 0x02008D20` = *disable IRQ*; `0x02009110: bl 0x02009118`
   = WFI; `b 0x02009110`) loops with **CPSR.I = 1**, so no IRQ can ever be
   delivered there (`cpsr=0x9F` at the loop head).  **melonDS never executes
   `0x020090D8` at all** (ARM9 trace window `0x020090D8..0x020090E0`, N=4,
   90 s of emulation covering ts ≥ 2.16B: **0 hits**); its 25,232 WFI wakes are
   *direct* calls to the WFI helper `0x02009118` from the card driver with
   `I` clear.  Class: **guest control-flow / IRQ-mask state**, not a missing
   runtime producer.

## 1. What is unbounded today (evidence)

`p5f/run0.err` (46M-step run, branch code without the Phase-5F splice):

```
263  "irq enter" lines
last: irq enter step=39192472 resume=0x02009BB4 ie=0x00040019 if=0x00080010
p4t: last B7 block ... starts=9093 last_op=0xB8     <- card stream tail
done steps=46000001 distinct_pcs=16447 halt_pc=0x02009114
```

The last `irq_trig_log` entry is `9097801`; deliveries continue 30.1M steps
past it.  `irq_raise_log` ends at `{9097800, 0x00000008}`; the card-transfer-end
raise (`card_end_transfer`, Phase-4N) is the only other IF writer and it fires
per transfer (~1,876 B7 reads + dummy reads in this run).

## 2. The first differing word: IE bit 19 at the WFI helper

New probe (this branch, `NNK_WFI_PROBE=1`, `tools/split_dispatch.py`):

```
wfiprobe n=1 pc=0x02009118 step=23490852 ime=1 ie=0x00040019 if=0x00080000 cpsr=1F
         cardnext=NEVER romcnt=0x27416000 spicnt=0xC000 dma2=0x05000001 dispstat=0x0008
...   (20,000 hits, all identical state: the helper is polled, never woken)
```

* `IE` is written only through the SDK helper `0x020060B4` (`IE = IE | mask`,
  RMW at `0x020060CC`) and `0x020060E4` (`IE &= ~mask`, RMW at `0x02006100`).
  Every logged write is `enable 0x00040019` / `disable 0x00000010`
  (`p5f/ie.err`), so native's IE settles at `0x00040009`..`0x00040019` and never
  contains `0x00080000`.
* melonDS at the matching transfer (`p5f/wfi3.txt`, ts 56.5M..2.16B): the first
  24 wakes have `ie=0x000C0009` (bits 0,3,18,**19**) and
  `enabled=0x00080000`; from then on `ie=0x00040019` with `enabled` bits
  0/3/4/18.  So the guest **does** program bit 19, and native never reproduces
  that mask.
* Consequence: the difference is inside the guest's card-driver IRQ
  programming, i.e. a data/path-dependent enable mask — not the runtime's IF
  producer (bit 19 *is* raised: `IF=0x00080000`), and not the delivery gate.

Cross-check of the two card streams (they agree up to the divergence):
`p4t: start op=0xB7 addr=0x02CF2C00 steps=38924880` is byte-for-byte the same
command as melonDS's 1,876th B7 (`cmd=B702CF2C00000000 ts=100800219`); after it
melonDS continues into a new region (`cmd=B70DA446...`, 550 dummy 4-byte
`0xB8` reads), while native issues ~12 dummy reads and parks.

## 3. The halt helper: a code path melonDS never enters

Native tail (`p5f/trace1.out`, 4M-line trace from step 38,970,000):

```
... 0x020d25ac 0x020d25b2 0x020d3250 0x020090d8 0x020090dc ... 0x02009108
    0x0200910c 0x02008d20 ... 0x02009110 0x02009118 0x0200911c 0x02009120
    0x02009114 0x02009110 ...              <- 746,220 iterations, forever
```

`0x020090D8` = `push {r3,lr}; if ([0x0206CA14]) { [0x0206CA14]=0; blx cb }; bl 0x02009108; pop`
`0x02009108` = `push {r3,lr}; bl irq_disable(0x02008D20); L: bl wfi(0x02009118); b L`
`0x02009118` = `mov r0,#0; mcr p15,0,r0,c7,c0,{4} (WFI); bx lr`

melonDS measurement: `MELONDS_ARM9TRACE_LO=0x020090D8 HI=0x020090E0 N=4 EXIT=1`
over 90 s (the loopwatch capture from the same invocation stream reaches
ts 2.16B) logs **no hit** — the reference never reaches that helper.

## 4. Phase-5F changes on this branch

* `tools/split_dispatch.py`: **periodic VBlank IRQ producer** in `trace_pc`
  (melonDS `GPU.cpp` `SetDispStatIRQ(0,0)` at `VCount==192`, gated on DISPSTAT
  bit 3, edge-triggered).  Bounded only by the frame clock, no captured trace.
  Measured effect on this run: **none** — the 263-delivery list is identical
  with and without it (`p5f/run0.err` vs `p5f/f.err`), because IF bit 0 is
  already pending at those edges and melonDS shows the same (3,818 wakes with
  `if` bit 0).  It closes the "no periodic producer in the model at all" gap for
  the next worker; it cannot wake the halt loop, whose `CPSR.I` is 1.
* `tools/split_dispatch.py`: **`NNK_WFI_PROBE=1` state probe** at `0x02009110`
  and `0x02009118` (IE/IF/IME/CPSR.I, card state machine, DMA2, DISPSTAT), capped
  at 20,000 lines.  This is what produced §2; Phase-5E could only say "the guest
  spins at 0x02009110".

No decoder/CPU-semantics change, so the cargo suite stays at **21 passed**.

### 4b. Negative result: the timer-IRQ producer (tried, measured, reverted)

The IE mask is programmed through `os_irqEnable` (`0x020060B4`); `NNK_DEBUG_AT_PC=0x020060B4`
with `NNK_DEBUG_N=300` (`p5f/dbg.err`, 111 calls in 30M steps) shows the *last*
calls are `r0=0x00000010` (bit 4 = **Timer 1**) every ~200k steps, returning to
`0x0200851C` — i.e. the guest's wait loop is a *timer* wait.  melonDS agrees:
8,303 of its 25,232 WFI wakes carry `enabled=0x00000010`.

So a timer-overflow producer (IF bits 3..6, sharing the TMxVAL read model's own
wrap points, spliced the same way) was implemented and measured
(`out/split/p5f_t`, `p5f/t.err`): it fires bit 4 (265 deliveries vs 263) but the
run is **slightly worse** — the card stream stops at step 35,487,542 (vs
35,8xx/39.0M baseline), `distinct_pcs` 16,444 (vs 16,447), still ends at
`0x02009118` with 113 uniform frames.  Reverted: it perturbs the guest without
removing the barrier, and shipping an uncharacterised behaviour change would
violate the strict-prefix/honesty rules.  The finding that matters is recorded
here: **the guest enables Timer 1 and never sees it run** (TM1CNT reads 0 at the
halt probe while IE bit 4 is enabled — `p5f/p2.err` `tm=…/0x000000C1/0x0000EFA3/0x00000000`),
so the next worker should compare TM0CNT/TM1CNT with melonDS at
`ts≈56.5M` before re-attempting the producer.

## 5. Finite replay tables still in the runtime (the starvation inventory)

| table | where | last entry | what starves |
|---|---|---|---|
| `irq_raise_log[]` | generated image, spliced by `tools/gen_irq_script.py` | `{9097800, 0x00000008}` | scripted IF raises: no bit other than 0x08/0x80000/… is raised by script after 9.10M; the card/VBlank paths are the only live producers |
| `irq_trig_log[]` | same | `9097801` (3 entries) | *delivery gate*; already bypassed by Phase-4N's `irq_trig_n >= IRQ_TRIG_N` arm — kept for the pre-9.1M trace-accurate window only |
| `arm7_evt_log[]` | generated prelude | `{3768189, 0x00037FF0}` (14 entries) | the ARM7 IPC data-word script: after 3.77M steps the ARM7's sync word only advances through the separately-measured responder schedule |
| ARM7 responder hold schedule | `tools/nnk_dma.inc` | measured read-for-read, finite per round | IPC round replies; a new request pattern falls back to the modelled default |
| card FIFO event clock | `card_tick`/`card_next` | model-driven (unbounded) | **not** a replay: timing comes from ROMCNT/AUXSPICNT (Phase-4D/4O) |
| Phase-5F VBlank producer | `trace_pc` | — | model-driven (unbounded), added here |

## 6. Verification (branch code)

| check | result |
|---|---|
| 60M-step run (`p5f/f.err`) | `steps=60000001 distinct_pcs=16447 halt_pc=0x02009110`, 113 frames, 114,448 unhandled io writes |
| IRQ deliveries | 263, last at step 39,192,472 (≫ 9,097,801) |
| WFI probe | 20,000 consecutive identical states at `0x02009118` from step 23,490,852 (`IE&IF=0`) |
| `cargo test` (nnktool) | **21 passed** |
| `SKIP_MELONDS=1 bash tools/verify_phase2a.sh` | **PASS** — strict common prefix 1000/1000 melonDS PCs (100.0%), windowed 1000/1000, in-order 1000/1000 (`p5f/verify.log`) |
| pictures | still uniform: `p5f/f_sh.00113.{top,bot}.ppm` single colour; PNGs (443 bytes each) `/home/hermes/NiNoKuni/shots/p5f_f113_top.png`, `..._bot.png` |

## 7. The next barrier (named for the next worker)

**Follow-up barrier A (root cause of the IE mask):** find where the boot's card
driver computes the IRQ-enable mask handed to `os_irqEnable` (`0x020060B4`) and
why bit 19 is absent in native while melonDS programs `IE=0x000C0009` at the
same transfer.  Start at the RMW sites `0x020060CC` / `0x02006100` and walk the
callers' mask sources (the mask is data-dependent — the same helper is called
with `0x00040019` and `0x00000010`), then diff the value against melonDS at
`ts=56,548,252`.

**Follow-up barrier B (the halt):** before that is fixed the guest ends in
`0x020090D8` (SDK halt/idle, `CPSR.I=1`, WFI loop) — a path the reference never
enters (0 hits in a 90 s ARM9-trace window).  The caller is the Thumb state
machine handler `0x020D3250` inside the `0x020D2xxx` dispatch helper band;
identify which state variable selects that case.

## 8. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/card-irq-producer && ln -sfn /home/hermes/NiNoKuni/work work
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/f bash tools/build_split.sh out/split/p5f_f
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5f
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless NNK_STEP_LIMIT=60000000 \
  NNK_WFI_PROBE=1 NNK_GX_SHOT=$S/f_sh NNK_GX_SHOT_EVERY=1 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/p5f_f work/phase3a/arm9_stored_padded.bin
(cd nnktool && cargo test)                     # 21 passed
SKIP_MELONDS=1 bash tools/verify_phase2a.sh    # PASS, 1000/1000

# melonDS ground truth used here (config dir matters: an empty XDG_CONFIG_HOME
# makes the emulator never boot the ROM):
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
  SDL_AUDIODRIVER=dummy \
  MELONDS_LOOPWATCH_PATH=$S/wfi3.txt MELONDS_LOOPWATCH_LO=0x02009110 \
  MELONDS_LOOPWATCH_HI=0x02009114 MELONDS_LOOPWATCH_AFTER=0 \
  timeout 120 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
  SDL_AUDIODRIVER=dummy MELONDS_ARM9TRACE_PATH=$S/halt_tr.txt \
  MELONDS_ARM9TRACE_LO=0x020090D8 MELONDS_ARM9TRACE_HI=0x020090E0 \
  MELONDS_ARM9TRACE_N=4 MELONDS_ARM9TRACE_EXIT=1 \
  timeout 90 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
```

Evidence (`$S`): `run0.{out,err}` (46M baseline), `trace1.out` (4M-line trace
from step 38,970,000), `ie.err` (IE RMW writes), `card0.log` (card events),
`f.{out,err}` + `f_sh.*.ppm` (60M with the Phase-5F splice), `wfi3.txt` (25,232
melonDS wakes), `halt_tr.txt` (0 melonDS hits at `0x020090D8`), `mel2..6.out`,
`verify.log`.

Changed in this phase: `tools/split_dispatch.py` (VBlank producer + `NNK_WFI_PROBE`
probe), `PHASE5F.md`.
