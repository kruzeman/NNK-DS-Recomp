# Phase-4Q — the handler-mask barrier: why u16[0x0206E93A] bit 1 never clears

Follows Phase-4P (merged `3d3a3b4`).  Scope: root-cause the mask/unmask gap
at the guest's wait `0x0200FEF8`, fix it with measured semantics, and push
through to the first real picture or an honest stop-report.

## 0. Headline

* **The mask/unmask pair is not an IME/IE bug at all — it is a missing ARM7
  IPC reply.**  `u16[0x0206E93A]` bit 1 is the card/stream driver's
  "operation in flight" flag: the SDK API at `0x0200FA38` sets it (pc
  `0x0200FB20`) and **then sends the ARM7 two IPC-FIFO requests at pc
  `0x0200BAD4`**.  melonDS's ARM7 answers the first one with the word
  `0xC0204006` 2,076 cycles later; the guest's IPC handler (`0x0200BB44` ->
  driver entry `0x0200F5CC`) decodes it as the driver's **"done" event**
  (handler id 6, dispatch path, type 1) and runs the finish tail
  `0x0200F784..0x0200F7A0`, which writes `+0x36 = 2` and **clears the mask
  bit**.  The waiter at `0x0200FEFC` then exits.
* **Native dropped the requests.**  The runtime's ARM7 is a scripted
  responder that knew exactly one IPC request (Phase-4C's `0x037A404D`); the
  two card-phase requests fell into its "unscripted" path:

  ```
  io: unscripted IPCFIFO request 0x80004046 at pc=0x0200BAD4
  io: unscripted IPCFIFO request 0x40400006 at pc=0x0200BAD4
  ```

  No reply -> no IPC-recv IRQ -> no done event -> the mask bit stays set and
  the guest spins at `0x0200FEFC` to the step budget.  This is the **exact
  first differing state**: the guest's own send-FIFO write at pc
  `0x0200BAD4` is acknowledged by melonDS's ARM7 and silently discarded by
  native.
* **The fix is the measured reply** (Phase-4Q section of `tools/nnk_dma.inc`):
  request `0x80004046` queues reply `0xC0204006`, delivered into the ARM9
  recv FIFO 2,076 modelled cycles later with IF bit 18 (IPCRecv) raised —
  the latency and the word both come from the melonDS IO capture, nothing is
  invented.

## 1. Ground truth used (all captured earlier, re-analysed here)

* `p4p_mw.log` (melonDS MemWatch, main RAM `0x0206E900..0x0206E940`): exactly
  three writes to `0x0206E93A` — `0x0000` at pc9 `0x0200F878` (init),
  `0x0002` at pc9 `0x0200FB20` (operation start), `0x0000` at pc9
  `0x0200F7A0` (finish).
* `p4q_mel_io.txt` (melonDS IO trace, 6.0M accesses, ts in ARM9-cycle units —
  the card start->start gap in the same trace is 10,900..11,400 ts against
  the measured 10,850 ARM9 cycles): the whole-run IPC send vocabulary is
  `{0x037A404D, 0x80004046, 0x40400006, 0x000001AB x3, 0x000000AB,
  0x0000002B, 0x81BA702B, 0x81B39C07, 0x81B36C07, 0x81B34E07, 0x81B34807}`
  and exactly **one** reply word is ever read back from the recv FIFO:
  `0xC0204006` at pc `0x0200BB44`, ts 28,709,423 — 2,076 cycles after the
  request `0x80004046` (pc `0x0200BAD4`, ts 28,707,347), delivered through
  the IPC-recv IRQ (the BIOS stub acks IF `0x00040000` at ts 28,709,384).
* melonDS IRQ enum (`src/NDS.h`): bit 18 is `IRQ_IPCRecv`, bit 19
  `IRQ_CartXferDone`; IE at the barrier is `0x00040009` (VBlank, Timer0,
  IPCRecv).
* Guest disassembly (`work/ghidra_src/arm9.bin`):
  `0x0200FB0C..0x0200FB34` operation start (sets mask bit 1, clears the
  IE-image bit, restores I), `0x0200F710`/`0x0200F784..0x0200F7A0` finish
  (writes `+0x36=2`, clears the mask bit, calls the done callback),
  `0x0200F5CC` the driver's IPC entry (dispatch on `(word>>14)&0x7F`),
  `0x0200BB00..0x0200BB88` the SDK IPC mailbox loop (handler table at
  `0x0206E588`, selected by `word & 0x1F`), `0x0200FEF8..0x0200FF08` the
  waiter (poll mask bit, return).
* The call site: `0x0204A720..0x0204A74C` — `start(0, ..., buf, 0x80)` then
  `wait(2)`, i.e. the two writes at `0x0200BAD4` are made from inside the
  start API (the `bl 0x0200BA70` IPC-send calls at `0x0200FAA4`/`0x0200FAC0`).

## 2. Native vs melonDS at the barrier (measured)

| quantity | melonDS | native (before the fix) |
|---|---|---|
| mask-set pc | `0x0200FB20` | `0x0200FB20` (step 18,554,864, same path) |
| send-FIFO write `0x80004046` at `0x0200BAD4` | answered | **dropped ("unscripted")** |
| reply `0xC0204006` in recv FIFO | ts 28,709,423 | never |
| IPC-recv IRQ (IF bit18 ack `0x00040000`) | ts 28,709,384 | never |
| mask-clear pc `0x0200F7A0` | runs | never (mask stays `0x0002`) |
| IRQ enters / returns over the run | — | 27 / 27 (balanced; not a lost-IRQ bug) |

Two probes on the native run (`NNK_DEBUG_AT_PC=0x0200FB20` and
`0x0200FEFC`, `NNK_DEBUG_MEM`) show the barrier state: `IME=1`, `IF=0`,
`u16[0x0206E93A]=0x0002`, and the last card transfer ended **165,882 steps
earlier** (step 18,388,975, the final B8 chip-id read) — so no card event
can complete the operation either; the only possible completion in melonDS
is the ARM7's IPC reply.  (The debug dump appeared to show `IE=0x00000009`;
that was a probe artifact — the dump reads memory byte-wise through
`io_read16`, whose IE path folds to the low halfword.  The Phase-4Q `iewr`
probe reads the register model directly and shows the true history:
`0 -> 0x00040000` (step 3,628,384, IPCRecv) `-> 0x00040008` (4,281,595,
Timer0) `-> 0x00040009` (13,908,599, VBlank) `-> 0x000C0009` (18,386,440,
card phase) `-> 0x00040009` (18,388,766, card done — bit 19 cleared, IPCRecv
kept), i.e. exactly melonDS's IE at the barrier.  There was never an IE
divergence; the barrier was purely the missing reply.)

## 3. The fix

`tools/nnk_dma.inc` (Phase-4Q section) + `tools/split_dispatch.py`
(Phase-4Q injections, exact-once anchors):

1. `nnk_ipc_send(v)` — hooked into `io_write32_sw`'s `0x04000188` case in
   place of the "unscripted" halt.  Request `0x80004046` schedules the
   measured reply `0xC0204006` at `cycles + 2076`; other unscripted
   requests are logged (first 8) instead of tripping the halt reason.
2. `nnk_ipc_tick()` — called once per dispatch from `nnk_dma_tick()`;
   when the modelled ARM7 latency expires it pushes the reply into the
   ARM9 recv FIFO (`arm9_recv_fifo`) and raises `IF bit 18`
   (`IRQ_IPCRecv`), exactly the melonDS hand-off.  Delivery stays
   rule-based: the reply only becomes an IRQ if the guest's own
   `IME / IE & IF / CPSR.I` state says so.
3. A Phase-4Q IE-history probe (`iewr ...`) logs every IE change with the
   writing pc, so any IE-side divergence is named, not guessed.

## 4. Result (measured, this branch's build)

The fix works: the reply is produced and delivered, and the guest passes the
barrier.

```
ipc: ARM7 request 0x80004046 -> reply 0xC0204006 in 2076 cycles step=18554801 pc=0x0200BAD4
ipc: ARM7 request 0x40400006 (no measured reply in the melonDS capture) step=18554845 pc=0x0200BAD4
ipc: reply 0xC0204006 queued if=0x000C0000 ie=0x00040009 ime=1 cpsr=1F step=18556878
ipc: ARM7 request 0x81B34807 (no measured reply ...) step=18568551 pc=0x0200BAD4
ipc: ARM7 request 0x81B34E07 (no measured reply ...) step=18578090 pc=0x0200BAD4
```

* **Bit 1 clears and the guest leaves `0x0200FEFC`.**  The reply (queued
  with `IE=0x00040009`, i.e. IPCRecv enabled exactly as in melonDS) is
  delivered by the unchanged rule-based path; the guest then sends the next
  requests of the same stream protocol melonDS sends (`0x81B34807`,
  `0x81B34E07` — both in the measured vocabulary) and keeps running:
  `distinct_pcs` rises from 9,377 to **11,093** (+1,716 previously
  unreachable guest PCs), the `0x0200FEFC` spin is gone, and the old
  `halt_pc` moves off the barrier to `0x02008D90`.
* **Frames: still uniform, honestly.**  `NNK_GX_SHOT` captures at frames
  10/20 (all `0x00`) and 30/40/50 (all `0xFF`) are single-valued —
  the display is the white backdrop, correct for a guest that has not yet
  written BG content (`gx: 59 frames presented`).  The picture is one stage
  further on, past the next barrier below.
* `SKIP_MELONDS=1 bash tools/verify_phase2a.sh`: **green, rc=0** —
  `RESULT: PASS`, strict common prefix **1,000 / 1,000 melonDS PCs
  (100.0 %)**, Ghidra bodies 100 % covered (log `p4q_verify.log`).

## 6. Honest stop-report for the picture — next barrier, named

* **First differing state after the fix**: at the 30M-step budget the guest
  is inside the SDK software-delay loop

  ```
  0x02008D90  subs r0, r0, #4
  0x02008D94  bcs  0x02008D90
  ```

  (tail-PC histogram over steps 29.5M..30M: 125,001 / 124,999 hits — one long
  delay call), while melonDS at the equivalent time (IO-trace ts 28.71M..31M)
  is already **streaming card data again**: 30,208 ROMCNT polls at
  `0x02016B30`, 18,576 uses of the ITCM card-read helper `0x01FF84D0`, and
  10,214 command writes at `0x02016974`.  Native's card stream still ends at
  the same 1,963 transfers (last B8 chip-id at step 18,388,982).
* **Class (named, not fixed here)**: the post-barrier stage — the guest's
  long SDK delay completes and the next card phase needs re-arming; the
  leading candidate producer is the **ARM7's periodic IPC messages**
  (`0x00008007` x4,631 and `0x00008087` x1,578 in the melonDS run, one every
  ~100..300K cycles), which the scripted ARM7 responder still does not
  produce — same class as this phase's fix, one level deeper.  A second
  candidate is the delay's own duration (native charges ~1 cycle per
  iteration against the SDK's 4-cycle `swiDelay` semantics, so the delay may
  be long in step terms but short in emulated time).
* **Next bounded step**: extend `nnk_ipc_send`/`nnk_ipc_tick` with the
  measured periodic ARM7 messages (cadence + words from `p4q_mel_io.txt`),
  or measure with `NNK_TRACE_FROM` whether the delay at `0x02008D90`
  terminates and what the guest re-arms next.

## 5. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
cd <worktree>; ln -s /home/hermes/NiNoKuni/work work

NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c bash tools/build_split.sh out/split/arm9_gx

NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_GX=headless \
  NNK_STEP_LIMIT=30000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

melonDS IO ground truth: `tools/melonds-io-trace.patch` build with
`MELONDS_IOTRACE_PATH=$S/p4q_mel_io.txt` (already captured this phase).

`SKIP_MELONDS=1 bash tools/verify_phase2a.sh` on this branch: see §4.
