# Phase-4O — the cart-transfer producer: ARM9 DMA mode 5 (game card DRQ)

Follows Phase-4N (merged `2dc7258`).  Scope: identify the producer that keeps a
card transfer in flight in melonDS at the SDK idle loop `0x0200712C`, root-cause
native's missing wake, fix it, and get the boot past the loop.

## 0. Headline

* **The producer is the guest's own ARM9 DMA in start mode 5 (game card DRQ)**,
  armed by the ITCM card-read helper — *not* a guest poll loop and *not* a
  "special mode 3" channel (Phase-4N's guess).
* **Phase-4N's premise "native's card interface is idle at the loop" was a
  measurement artifact**: the generated image's own card log stops after four
  transfers (`card_xfer_log_n < 4`), so "no start after card_cycles=7,812,062"
  only meant the log cap.  With an unbounded native card trace the native guest
  streams transfers continuously (5,812 events) and does reach the loop with a
  `0x200` read **in flight**; the transfer then stalls at melonDS's 2-word FIFO
  because nothing drains it, so `ROMEndTransfer` never runs and IF bit 19 is
  never raised.
* **Fixed**: the runtime now models ARM9 DMA start mode 5 (melonDS
  `NDSCart::Interface::CheckDMA -> NDS::CheckDMAs(0, 0x05)`), draining the card
  FIFO on the card-bus clock.  The guest **exits `0x0200712C`** and follows
  melonDS's wake path (WFI at `0x02009118` → ITCM vector `0x01FF8000` → card
  helper), one IRQ per transfer at the measured 21,7xx-cycle cadence.

## 1. Ground truth (melonDS instrumentation, committed)

`tools/melonds-cardlog.patch` (env-gated, inactive by default):

* `MELONDS_CARDLOG_PATH` — every ROMCNT write / transfer start / start-reject /
  `ROMEndTransfer` / ROM-data read, with the ARM9 timestamp and the ARM9 PC that
  caused it (`NDSCart.cpp`).
* `MELONDS_DMALOG_PATH` — every ARM9 `DMACNT` write (channel, CNT, mode, SAD,
  DAD, PC) and every `DMA::Start` (`DMA.cpp`).

Measured sequence (`p4o_dma.log`, `p4o_card.log`):

```
start ts=56526794 pc=0x02016930 size=0x200 spicnt=0xC000 romcnt=0xA1416657
                                                   cmd=B7000E0C00000000
dmawr ts=56526612 ch=2 pc=0x01FF84D0 val=0xAF000001 mode=5
                          src=0x04100010 dst=0x02079F80     <- 182 cycles earlier
romdata ts=56543172 pc=0x0200911C halted=1 ... reads 1..128  <- CPU stopped, DMA reads
end     ts=56548252 pc=0x0200911C ... irqen=1 len=0x200 reads=128
```

* CNT `0xAF000001` decoded with melonDS `DMA::WriteCnt`: bit31 enable,
  bits27-29 = **5** (cart DRQ), bit25 repeat, bit26 = 1 (32-bit units),
  src ctrl bits23-24 = 3 (reload ⇒ SAD stays `0x04100010`), dst ctrl = 0 (inc).
* The drain reads are issued **while the ARM9 is stopped** (`halted=1`) at the
  halted PC, i.e. by the DMA engine, 128 word pops = exactly the `0x200` bytes;
  the last pop reaches `ROMEndTransfer`, which raises IF bit 19 because
  AUXSPICNT bit14 is set (`spicnt=0xC000`) — the loop's wake, matching the
  Phase-4N loopwatch cadence (first wake +20,235 cycles, steady 21,700).
* Destination advances `0x02079F80 → 0x0207A180` per transfer (+0x200): the
  guest streams ROM bytes into a RAM buffer, one `0x200` chunk per IRQ.

## 2. Native root cause (exact first differing word)

Phase-4N's stop-report named `if/Ie = 0` at the loop.  This phase names the
first differing **state word** and its cause:

* native **DMA2CNT (0x040000C8)** is armed with `0xAF000001`, identical to
  melonDS, but no runtime code acts on start mode 5: `io_do_dma()` returns for
  any non-immediate mode, and `tools/nnk_dma.inc` dispatched only modes 0 / 1 /
  2 plus Phase-4N's **mode 3** (never a cart trigger on ARM9 — melonDS calls
  `CheckDMAs(0, 0x05)` only).
* Consequence, measured with the new native card trace (`NNK_CARDLOG`):
  `start cyc=33113500 cmd=B7000E0C...` followed by **no** `end`; the probe at
  the loop (`step=18387047`) shows `card_next=33121680` still pending,
  `romcnt=A1416657` (busy), `drq=0`, `iflags=0x00000000`, `dma2=AF000001`.
  Class: **missing DMA producer in the card-DRQ path** (not a decoder, not a
  display, not an IRQ-delivery bug — the Phase-4N IRQ machinery is correct and
  fires the moment the transfer actually ends).

## 3. Fix (measured semantics)

`tools/nnk_dma.inc` (spliced into the pre-generated image by
`tools/split_dispatch.py`, which also gained the Phase-4O trace/probe hooks):

1. `nnk_dma_card()` is now dispatched for **mode 5**, not mode 3.
2. The drain fires only card events that are **already due** on the card-bus
   clock (`card_next <= card_cycles`) instead of fast-forwarding the clock, so
   a `0x200` read really takes its modelled 21,7xx cycles and the guest reaches
   the SDK WFI — the same ordering melonDS produces by running the DMA only up
   to `ARM9Target`.
3. 16-bit units store two halfwords per FIFO word (byte-stream fidelity);
   repeat bit (25) reloads the count, otherwise the enable bit clears.
4. `DAD` advances with the channel's destination control, so the buffer lands
   where the guest expects it.

## 4. Result (native, this branch's image)

```
loopprobe step=18387047 ... card_next=33121680 romcnt=A1416657 drq=0 if=0
irq enter step=18391707 resume=0x02009118 ie=0x000C0009 if=0x00080000   <- WFI wake
irq enter step=18391797 resume=0x0200911C ...
irq enter step=18404739 resume=0x02007138 ...
...
done steps=25000001 distinct_pcs=9050 halt_pc=0x020168A4 (step budget)
done steps=60000001 distinct_pcs=9377 halt_pc=0x0200FEFC (step budget)
```

* 3 IRQs before the loop → **27 IRQs** and one per transfer afterwards
  (~6,470 steps ≈ 21,7xx card cycles), resumes on the WFI instruction and the
  card helper — melonDS's post-wakeup path.
* `distinct_pcs` 9,019 → 9,377 at 60M steps: the boot advances through the card
  streaming loop instead of spinning at `0x02007138`.

## 5. Honest stop-report for the picture

* **Still no real picture.** 22 headless frames (`p4o_sh.*.ppm`, one pair every
  10 VBlanks over 60M steps) are all uniform: black for the first ~20 frames,
  then white (`mean=255`, `distinct=1`) — the same backdrop Phase-4M/4N left,
  with the guest now streaming card data instead of idling.
* **Next barrier (named, not yet root-caused)**: the boot is inside a long
  card-streaming phase (one `0x200` DMA read per transfer, destination buffer
  advancing 0x200 per transfer) — i.e. asset/overlay loading after the loop,
  and it is still doing that at the 60M-step budget.  The display registers and
  VRAM bank map stay as Phase-4M measured them (no BG layer has content yet), so
  the composite is the white backdrop in *both* implementations at this guest
  point.  The next bounded step is a lockstep diff of native vs melonDS at
  `halt_pc=0x0200FEFC` / the equivalent melonDS ts to name the first differing
  word *after* the card stream, and a melonDS frame capture at the matching
  point to know whether the reference is still blank there too.
* **Known simplification**: `nnktool/src/recompile.rs` (the authoritative
  emitter for the next regeneration) still has no DMA start-mode-5 model — the
  behaviour lives only in `tools/nnk_dma.inc` for the pre-generated image, the
  same split Phase-4L/4M/4N used.

## 6. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
ln -s /home/hermes/NiNoKuni/work work         # worktree -> main checkout work/

# melonDS ground truth (patch first: tools/melonds-cardlog.patch against melonDS src)
env XDG_CONFIG_HOME=$S/melonds_cfg SDL_AUDIODRIVER=dummy \
    MELONDS_DMALOG_PATH=$S/p4o_dma.log \
    MELONDS_CARDLOG_PATH=$S/p4o_card.log \
    xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"

# native
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c tools/build_split.sh out/split/arm9_gx
NNK_CARDLOG=$S/p4o_nat_card.log NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" \
  NNK_GX=headless NNK_GX_SHOT=$S/p4o_sh NNK_GX_SHOT_EVERY=10 NNK_STEP_LIMIT=60000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

`SKIP_MELONDS=1 sh tools/verify_phase2a.sh` on this branch: green (log
`p4o_verify.log`; strict prefix 1000/1000 melonDS PCs, A2 PASS).
