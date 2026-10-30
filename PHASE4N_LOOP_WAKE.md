# Phase-4N — what wakes the SDK idle loop (measured), and why native still cannot

Follows Phase-4M (merged `4276785`).  Scope: identify the event that ends the
guest's wait at `0x0200712C` with melonDS ground truth, deliver it in native,
or stop with the exact class named.

## 0. Headline

* **The loop is not a terminal/error loop and not a hang.** Disassembly:

  ```
  0x0200712C  push {r3,lr}
  0x02007130  bl 0x02008D0C    ; mrs/bic #0x80/msr -> ENABLE IRQ (returns old I)
  0x02007134  bl 0x02009118    ; mov r0,#0; mcr p15,0,r0,c7,c0,4  -> WFI
  0x02007138  b  0x02007134    ; wait again
  ```

  `0x02008D0C` clears CPSR.I (it is *enable*, not disable: the probe shows
  `cpsr=0x0000001F` in the loop, and the identical function is used by the
  critical-section helper at `0x02007104`).  The loop is the SDK's
  **IRQ-wait idle body**: it only leaves via an IRQ that changes the saved
  context — WFI resumes at `0x02007138`, so a plain return would spin forever.
* **The wake event, named with ground truth: the cart transfer-done IRQ,
  IF/IE bit 19 (`IRQ_CartXferDone`), one per card transfer.**  New melonDS
  hook `MELONDS_LOOPWATCH_PATH` (`src/ARM.cpp`, `LoopWatch::` — entry, every
  IRQ taken in the loop, departure + the following PCs) gives:

  ```
  enter ts=56528013 pc=0x0200712C cpsr=0x0000001F r13=0x0206C8B0 r14=0x02006AF0
        ie=0x000C0009 if=0x00200000 ime=1
  ... 0x02009118 (WFI) ...
  irqwake ts=56548252 pc9=0x0200911C ie=0x000C0009 if=0x00280000 ime=1 enabled=0x00080000
  irqwake ts=56569952 ...                    (steady state: every 21,700 cycles)
  ```

  `enabled = IE & IF = 0x00080000` = bit 19.  In melonDS that bit is raised by
  `NDSCart::Interface::ROMEndTransfer` when `SPICnt & (1<<14)`.  Loop entry at
  ARM9 ts **56,528,013**, first wake **20,235** cycles later, then a
  **21,700-cycle** cadence (one transfer per wake).
* **The post-wakeup path (melonDS, `p4n_wake.txt`)**: the IRQ goes
  `0x0200911C -> 0xFFFF0018 -> 0xFFFF06D8..06EC` (BIOS dispatcher) ->
  ITCM user vector `0x01FF8000..0x01FF804C` -> **`0x02016BC8`** (main-RAM
  handler/continuation) -> `0x0201688C` … i.e. the handler resumes the guest's
  card-read work, not the loop.
* **Native still does not wake, and the class is now exact**: native delivers
  only the three scripted IRQs (last at step 9,097,801) and then spins at
  `0x02007134/0x02007138` to the step budget (`halt_pc=0x02007138`,
  `distinct_pcs=9019`, 25,000,001 steps).  melonDS enters the same PC with a
  **card transfer already in flight**; native enters it with the card **idle**
  (probe at the loop: no special-mode DMA armed, last card start logged at
  `card_cycles=7,812,062`).  Class: **missing wake-event producer** — the
  transfer stream melonDS keeps re-arming (one completed read per IRQ) does
  not exist in the native run, so `ROMEndTransfer`/IF bit 19 can never fire.

## 1. What was implemented anyway (real hardware semantics, no fakery)

Written for both the authoritative emitter (`nnktool/src/recompile.rs`) and the
pre-generated image used by this branch's runs (`tools/split_dispatch.py`
injections, exact-once anchors, split aborts if an anchor is missing):

1. `ROMAdvanceSend` semantics: a data-read transfer now schedules
   `ROMTransfer_End` after the last word (`card_advance_receive`).
2. `ROMEndTransfer` raises `IF bit19` when AUXSPICNT bit14 is set
   (`card_end_transfer`) — Phase-4D had deliberately left it out because the
   capture window used then (ts <= 23.1M) contained no such raise; the new
   capture falsifies that note.
3. IRQ delivery is rule-based after the scripted window:
   `(irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])` replaces
   the 3-entry cap, keeping the measured 3 triggers as the floor so the
   verified prefix is unchanged.
4. `tools/nnk_dma.inc`: ARM9 DMA start mode 3 (**special = game card**) is now
   drained word-by-word from the card FIFO, walking the card state machine to
   each scheduled event (melonDS `DMA::WriteCnt` StartMode 3 + NDSCart DRQ).

Measured effect on the run: **none** — still 3 IRQs, same `distinct_pcs`,
`halt_pc=0x02007138`.  No special-mode DMA is armed in the guest at all, and no
`ROMCNT` write occurs after `card_cycles=7,812,062`, so the fixes sit downstream
of the missing producer (they are still correct per melonDS and are what the
event will need once the producer exists).

## 2. Honest stop-report — exact first differing state + class

* **Class**: missing event producer / guest control flow — the native guest
  reaches the SDK IRQ-wait loop (`0x0200712C`) at step **18,387,059** with the
  card interface idle, whereas melonDS reaches it at ARM9 ts **56,528,013**
  with a transfer in flight whose completion (IF bit19, `IRQ_CartXferDone`,
  enabled in `IE=0x000C0009`) is the first wake.
* **First differing observable**: the cart-transfer arm/stream — native
  `romcnt` (`0x040001A4`) shows no new start after `card_cycles=7,812,062`
  (last `io: card start cmd=0xB7 addr=0x001D4200 size=0x200`), while melonDS
  produces one completed transfer every 21,700 cycles at the same guest PC.
  Equivalently: `IE & IF = 0` in native at the loop (no wake source) vs
  `0x00080000` in melonDS.
* **Not a graphics problem**: the display register/VRAM state stays as
  Phase-4M left it (POWCNT1=0x020F, DISPCNT_A=0x0021111B,
  DISPCNT_B=0x80211013, identical bank map); with the guest stuck, the
  composite remains the white backdrop.  Frames from this branch's runs
  (`p4n_sh.*.ppm`) are uniform — and correctly so at this guest PC, since no BG
  layer has content in either implementation.
* **Next bounded step**: the melonDS IO trace only reached ts 23.1M, so the
  `0x040001A4` writes of the 18M..56M window are not captured yet.  Capture a
  60M-cycle IO trace and replay the arm sequence (which PC writes ROMCNT, with
  which value) into the guest's path before `0x0200712C` — that names the
  producer whose absence is the barrier.

## 3. Measurement surface (committed tooling)

* `tools/melonds-loopwatch.patch` — the melonDS hook (LoopWatch: loop entry,
  every IRQ taken in the loop with IE/IF/IME, departure + next N PCs).
* `tools/nnk_dma.inc` — Phase-4N additions (mode-3 card DMA drain; one-shot
  loop probe), `tools/split_dispatch.py` — Phase-4N injections.

## 4. Verification

`SKIP_MELONDS=1 bash tools/verify_phase2a.sh` on this branch (see
`p4n_verify.log` in the scratch dir for the full log).  Native runs in this
phase used `work/phase3a/arm9_stored_padded.bin` through the `work` symlink;
numbers above are from `NNK_STEP_LIMIT=25000000` and from a state probe at
`NNK_STEP_LIMIT=18400000`.
