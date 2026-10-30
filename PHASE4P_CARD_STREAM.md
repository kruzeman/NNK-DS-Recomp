# Phase-4P — the card-streaming phase: why it is long, and the transfer that stranded it

Follows Phase-4O (merged `ab344ba`).  Scope: explain why the post-loop card
stream is long in native steps, push the boot through it, and reach either the
first real picture or an honest stop-report with the exact first differing
state word.

## 0. Headline

* **The streaming phase is not a cadence gap, and it is only partly a budget
  question.**  Native's card *clock* matches melonDS to **0.3 %** per `0x200`
  transfer (10,761 vs 10,728 ARM9 cycles) and the guest's **command stream is
  identical for the first 1,963 transfers** (op, ROM address and size).  What
  inflates the native budget is the **per-instruction cycle cost**: native
  charges the ROMCNT poll loop 3.85 cycles/iteration where melonDS charges 9.0,
  so native spins **2,127 polls per transfer** against melonDS's measured
  **909** — a 2.3x instruction multiplier for the same emulated time.
* **The real blocker was a card-DRQ bus-ownership bug, now root-caused and
  fixed.**  Native did not model the ARM9 halt while the cart DMA drains
  (melonDS `CPUStop_DMA9_n`), so the guest executed *inside* a transfer: at
  guest step 18,534,214 (69 steps after the transfer started, 8,180-cycle
  command delay still running) it issued its 1-word FIFO DMA
  (`DMA2CNT=0x85000001`, SAD `0x04100010`) against an **empty FIFO**.  The
  transfer then stranded at `card_pos=8/512` (FIFO full, `count=2`, `drq=1`,
  `card_next=CARD_NEVER`, DMA2 disabled) and the guest waited forever in the
  ROMCNT-busy loop at `0x020168A0`.
* After the fix the run follows melonDS's command stream through transfer
  **#1,963**, including the measured `B7` → `B8` transition at the end of the
  `0x200` stream, and the `0x020168A0` loop is gone.
* **Still no picture.**  Next barrier (named, not fixed): the guest waits at
  `0x0200FEF8` for bit 1 of `u16[0x0206E93A]` to clear; native holds `0x0002`,
  melonDS's last write to that halfword is `0x0000`.  Class: **IRQ-handler
  mask/unmask handshake never completes**.

## 1. Ground truth added this phase

Nothing about melonDS had to be re-derived; the Phase-4L/4O captures
(`p4o_card.log`, `p4o_card3.log`, `p4o_dma.log`) were re-analysed, plus one new
capture:

* `tools/add_memwatch_window.py`-style watch on main RAM `0x0206E900..0x0206E940`
  via the existing env knobs (`MELONDS_MEMWATCH_LO/HI`), 1,933 writes, with the
  writing ARM9 PC (`p4p_mw.log`).  Result for `0x0206E93A`: exactly three
  writes — `0x0000` at pc `0x0200F878` (init), `0x0002` at pc `0x0200FB20`
  (handler-mask entry, which also clears IE bit1 at `0x0206E938`), `0x0000` at
  pc `0x0200F7A0` (handler-mask exit).
* melonDS timestamps are **raw** `ARM9Timestamp`; `ARM9ClockShift = 1`
  (134 MHz), so `start→end` 21,457 raw = **10,728 ARM9 cycles** per `0x200`
  transfer, and the inter-start gap 21,700 raw = 10,850 ARM9 cycles.
* Cart-DMA ground truth over the whole run (`p4o_card3.log`): only **72** of
  75,470 FIFO words were ever read with `halted=1` (the DMA drain at pc
  `0x0200911C`); the other 75,398 were read by the **CPU itself** at pc
  `0x02016B3C` / `0x02016984`.  The mode-5 channel is therefore a *short*
  assistant, not the main mover — which is what made the stranded-FIFO case
  reachable.

## 2. Why the phase is long in native steps (measured, not inferred)

| quantity | melonDS | native | ratio |
|---|---|---|---|
| card cycles per `0x200` transfer (start→end) | 10,728 ARM9 | 10,761 (`card_cycles`) | 1.003 |
| ROMCNT polls before data / per transfer (measured) | 909 | 2,127 (`polls=` in the card log) | 2.34 |
| modelled cycles charged to one poll iteration | 9.0 | 3.85 | 2.34 |
| guest steps per transfer | — | 6,471 (constant) | — |
| command stream | — | identical for 1,963 transfers | — |

So the streaming phase is long because (a) the SDK really does poll ~900 times
per 512-byte chunk and (b) native under-charges each poll iteration.  It is
**not** a divergence in transfers-per-emulated-cycle: the card model and the
guest's command stream agree.

## 3. The strand (exact first differing state), and the fix

First differing state, native, guest step **18,534,214**, pc **0x02009680**
(the guest's 1-word FIFO DMA helper):

```
DMA2CNT 0xAF000001 -> 0x85000001   SAD=0x04100010 DAD=0x0207CD80 (1 word, src fixed)
card_pos=0/512  card_len=512  card_count=0  card_drq=0
card_next=33,369,535  (card_cycles 33,361,355 -> first word 8,180 cycles away)
ROMCnt = 0xA1416657   (bit31 busy)
```

69 steps after the transfer start, i.e. **before the first word existed**,
because native let the guest run straight through the transfer.  melonDS halts
the ARM9 while the cart DMA owns the bus, so its guest never sees a half-arrived
`0x200` read.  After the empty-FIFO read the model could never finish:

```
busyprobe step=18,634,267 card_cycles=33,528,204 card_next=-1 endpend=0
          romcnt=A1416657 card_pos=8/512 count=2 late=1 drq=1 dma2=05000001
```

and the guest spun **535,399** iterations of

```
0x020168A0  ldr  r0,[r1]      ; r1 = 0x040001A4 (ROMCNT)
0x020168A4  tst  r0,#0x80000000
0x020168A8  bne  0x020168A0
```

with `halt_pc=0x020168A4` at the 20M budget.  Class: **missing DMA
bus-ownership (CPU-halt) model in the card-DRQ path** — not a decoder, not a
display, not an IRQ-delivery bug (the transfers before it all completed and the
IRQ fired correctly).

Fix (both in `tools/nnk_dma.inc`, spliced into the split image):

1. `nnk_card_stall()` — while a start-mode-5 channel is armed and a transfer is
   in flight, the transfer runs to completion in modelled card time **before the
   guest gets its next instruction**, and the elapsed cycles are added to the
   ARM9 clock (`cycles`), exactly as melonDS advances `ARM9Timestamp` across the
   halt.  Called first thing in `nnk_dma_tick()` (once per dispatch).
2. `nnk_dma_copy()` now performs its accesses through the **IO bus**
   (`io_read32`/`io_write32` when the address is in `0x04000000..0x04FFFFFF`)
   instead of the flat accessor, so a DMA sourced from the card data FIFO
   (`0x04100010`) really pops the FIFO and advances the transfer — melonDS's
   `DMA::Run9` calls `ARM9Read32`/`ARM9Write32` for the same reason.

Result (native, this branch's image, 30M-step budget):

```
done steps=30000001 distinct_pcs=9377 hle_hits=7251 halt_pc=0x0200FEFC
card starts: 1,963, last two B8 (chip-id) — the same transfer #1,963/1,964
             melonDS issues at ts 57,048,233 / 57,048,591
command stream identical for the first 1,963 transfers
```

The `0x020168A0` busy-wait loop no longer appears at all.

## 4. Honest stop-report for the picture

* **Still no non-uniform frame.**  The display state did not move this phase:
  the guest has not written new VRAM/palette/OAM content (Phase-4M's bank map
  and DISPCNT values stand), and the run now stops before the guest reaches the
  next stage.
* **New barrier (named with ground truth).**  The guest is at

  ```
  0x0200FEF8  ldr  r1,=0x0206E900
  0x0200FEFC  ldrh r2,[r1,#0x3A]      ; u16[0x0206E93A]
  0x0200FF00  tst  r2, r0             ; r0 = 2
  0x0200FF04  bne  0x0200FEFC         ; spin until the bit clears
  ```

  First differing state word: **`u16[0x0206E93A]` = `0x0002` (native) vs
  `0x0000` (melonDS's last write, pc `0x0200F7A0`)**.  `0x0206E93A` is the
  handler mask word: `0x0200FB0C` sets bit n on handler entry (and clears the
  matching bit of the IE image at `0x0206E938`), `0x0200F780..0x0200F7A0`
  clears it on exit and returns the mask.  Native has bit 1 set with no exit,
  i.e. an IRQ handler's mask/unmask pair is not completing — consistent with the
  27-IRQ burst measured after the WFI wake (each `irq enter` 90 steps after the
  previous one, all with `if=0x00080000`, while the handler's own resume PC
  walks +4 per entry).
* **Next bounded step** (for the next phase, not done here): instrument the
  runtime's `irq_enter`/`bios_irq_return` pair against the melonDS
  handler-mask PCs (`0x0200FB20` set, `0x0200F7A0` clear) and name the first
  handler entry with no matching exit; the same 27-IRQ burst is the profile to
  match.

## 5. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
cd <worktree>; ln -s /home/hermes/NiNoKuni/work work      # work/ lives in the main checkout

# build the split image (pre-generated Phase-4J image + this branch's .inc files)
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c bash tools/build_split.sh out/split/arm9_gx

# native run with the card trace and the new probes
NNK_CARDLOG=$S/p4p_nat_card5.log NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" \
  NNK_GX=headless NNK_STEP_LIMIT=30000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin

# melonDS ground truth for the handler-mask word
env XDG_CONFIG_HOME=$S/melonds_cfg SDL_AUDIODRIVER=dummy \
    MELONDS_MEMWATCH_PATH=$S/p4p_mw.log MELONDS_MEMWATCH_N=4000 \
    MELONDS_MEMWATCH_LO=0x0206E900 MELONDS_MEMWATCH_HI=0x0206E940 \
    xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"

# analysis helpers written this phase (scratch, not committed):
#   p4p_cmdseq.py    command-stream lockstep (op/addr/size) melonDS vs native
#   p4p_dur.py       start->end duration and inter-start gap per transfer
#   p4p_romdata.py   who consumes FIFO words (DMA halted=1 vs CPU halted=0)
#   p4p_tail.py      tail PC histogram -> the spinning loop
```

`SKIP_MELONDS=1 sh tools/verify_phase2a.sh` on this branch: green (log
`p4p_verify.log`).

## 6. Known simplifications / follow-ups

* `nnktool/src/recompile.rs` (the authoritative emitter) still emits the old
  `io_do_dma` (bits 24/26/`0x7FFF` field decode, flat accessor) and has no
  bus-stall model for start mode 5; this phase's behaviour lives in
  `tools/nnk_dma.inc` for the pre-generated image, the same split Phase-4L/4M/
  4O used.
* The stall model completes the transfer at the *first* dispatch after the
  `ROMCNT` write; melonDS halts at DRQ onset.  The difference is a few guest
  instructions before the halt, not a state difference (the guest is stopped in
  both cases until the same transfer end).
* Phase-4O's "one IRQ per transfer at the 21,7xx-cycle cadence" reading was a
  raw-timestamp vs ARM9-cycle confusion (21,457 raw = 10,728 ARM9 cycles) and,
  in native, the 27 IRQs after the wake are one burst of ~2,070 steps, not one
  IRQ per transfer.  The card log's per-transfer `polls=` field and the
  `start`/`end` step numbers are the numbers to quote.
