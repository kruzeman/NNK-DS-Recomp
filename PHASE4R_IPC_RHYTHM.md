# Phase-4R — the ARM7 periodic IPC rhythm: measured producer + the SDK wait at 0x02008D90

Follows Phase-4Q (merged `e4636b7`).  Scope: measure the ARM7's periodic
IPC messages with melonDS ground truth, implement them in the runtime's
scripted ARM7 responder, and push the guest past the swiDelay loop
0x02008D90 to the first real picture or an honest stop-report.

## 0. Headline

* **The Phase-4Q stop-report barrier is an SDK message-queue wait whose
  wakeup is an ARM7 message.**  After the card-phase requests the guest
  calls the SDK wait at `0x0200ACC8..0x0200AD20`: snapshot the queue
  sequence, then loop `{ restore-IRQ (0x02008D34), swiDelay(50) (wrapper
  0x02008D9C -> loop body 0x02008D90, 21 iterations), disable-IRQ
  (0x02008D20), re-read the sequence }` while `*(0x0206CA5C) ==
  *(*(0x0206E580))`.  The probe (`NNK_DEBUG_AT_PC=0x02008D90` + stack
  dump) shows the loop entered at step 18,578,180 — 90 steps after the
  request send `0x81B34E07` — with r0=0x54 (21 iterations per chunk,
  caller r0 = 0x32 = the r5 in the dump), same frame (r13=0x027E3A00,
  r14=0x02008DB4) for the whole budget.
* **The ARM7's rhythm is now measured and implemented.**  melonDS's ARM7
  sends three periodic words into the ARM9 recv FIFO (all read by the SDK
  mailbox loop at pc `0x0200BB44`): the driver heartbeat `0xC0240006`
  every **one frame (560,190 ts)**, the 100 Hz tick `0x00008007` every
  **335,104 ts** (median; 33,513,982/100 = 335,140), and its 3-tick
  variant `0x00008087` every **983,040 ts**.  The producer
  (`tools/nnk_dma.inc`, Phase-4R section) is armed when the Phase-4Q
  reply is delivered and pushes each word into `arm9_recv_fifo` with
  IF bit 18 at the measured offsets (+423,472 / +503,723 / +1,499,049)
  and periods — nothing invented, all from `p4q_mel_io.txt`.
* **The guest passes 0x02008D90 and runs new code**: `distinct_pcs`
  11,093 -> **11,371** (+278), 64 IPC-recv IRQs are taken where Phase-4Q
  took none, and the post-delay path is the driver continuation
  (`0x0200BA40`, `0x02010088..0x02010094`) plus bulk LZ decompression
  (`0x02000984..0x020009D0`, ~300K loop iterations in the tail ring).
* **The next barrier is named (§6)**: a later SDK wait on the same queue
  never wakes — the sequence word stays `0x00000000` (measured at every
  producer delivery through step 20.1M and frozen to the 60M budget,
  `distinct_pcs` stuck at 11,371 from 30M to 60M).  melonDS's wakeup for
  that wait is the next card phase (EXMEMCNT bus switch ts 29,024,936,
  ROMCTRL commands from ts 29,034,972, IF bit19 CartXferDone pending at
  ts 29,034,056) — i.e. the ARM7 must PERFORM the requested phase, one
  level deeper than Phase-4Q's missing reply.

## 1. Ground truth (p4q_mel_io.txt, 6.0M accesses; ARM9-side only)

The IO trace has no ARM7 PCs, so the ARM7's sends are observed as ARM9
recv-FIFO reads `R32 0x04100000` (all at pc `0x0200BB44`).  Whole-run
recv vocabulary and cadence (ts = ARM9-cycle units):

| word | n | first ts | period (measured) | decoded |
|---|---|---|---|---|
| `0x00008007` | 4,631 | 29,213,146 | med 335,104 (332,734..337,398) | handler id 7, the ARM7 100 Hz tick |
| `0xC0240006` | 2,770 | 29,132,895 | **560,190 = one frame** (559,154..560,235) | handler id 6, the driver's per-frame "done" heartbeat |
| `0x00008087` | 1,578 | 30,208,472 | med 983,040 | handler id 7, the 3-tick variant |
| `0x0000006B` | 5 | 50,485,392 | burst to 50,498,023 | later event, not modelled here |
| `0x0000004D`, `0xC0204006` | 1 each | 3,975,109 / 28,709,423 | one-shots | Phase-4C / Phase-4Q |

Offsets relative to the barrier reply read (ts 28,709,423): first
`0xC0240006` **+423,472**, first `0x00008007` **+503,723**, first
`0x00008087` **+1,499,049**.  The stream is not idle traffic: the guest
ANSWERS each tick with the next card request — send `0x81B36C07` at ts
29,218,077 (+5,061 after the tick at 29,213,146) and send `0x81B39C07`
at ts 29,552,703 (+4,402 after the tick at 29,548,301).

melonDS post-barrier choreography (all in the trace): reply 28,709,423
-> sends `0x81B34807`/`0x81B34E07` -> EXMEMCNT bus switch from ts
29,024,936 -> first ROMCTRL command ts 29,034,972 (pc `0x020168F4`)
-> IF read `0x00280000` (bits 19+21) at ts 29,034,056 -> IE `0x00040019`
-> replies/ticks flow.  The largest ARM9 IO gap in ts 28.7M..40M is
**141,230 ts** — melonDS never runs a multi-million-cycle delay.

## 2. Native before the fix (measured)

After the same sends (step 18,578,090) the guest entered the wait at
step 18,578,180 and did **zero IO and zero IRQs** for the rest of the
30M-step budget (`halt_pc=0x02008D90`, halt state `r0=0x00000008
r14=0x02008DB4` = the tail of one 21-iteration chunk, i.e. the loop had
no end in sight — a fresh 60M run keeps it, see §6).

## 3. The fix (`tools/nnk_dma.inc`, Phase-4R section)

`nnk_ipc_send`/`nnk_ipc_tick` from Phase-4Q extended with a measured
periodic producer:

1. When the `0xC0204006` reply is delivered, `nnk_p4r_arm(cycles)` starts
   three streams with the measured first-offsets and periods (§1).
2. Each dispatch, `nnk_p4r_schedule()` enqueues every due word into a
   bounded host-side queue (32 entries, FIFO-full drops are logged), and
   due words are pushed into `arm9_recv_fifo` with IF bit 18 — the same
   rule-based delivery path as the Phase-4Q reply (the IRQ only fires if
   the guest's own IME / IE & IF / CPSR.I state says so).
3. The delivery log carries a Phase-4R probe: `recv_n` (the FIFO depth
   the guest leaves behind), the wait's snapshot `*(0x0206CA5C)` and the
   queue sequence `*(*(0x0206E580))`.

## 4. Result (this branch's build)

```
ipc: reply 0xC0204006 queued ... step=18556878
ipc: p4r periodic producer armed t0=21269531 step=18556878
ipc: p4r ARM7 -> 0xC0240006 queued if=0x000C0000 ie=0x00040009 ime=1 cpsr=1F recv_n=1 snap=00000000 seq=00000000 step=18980334
ipc: p4r ARM7 -> 0x00008007 queued ... step=19060569
ipc: p4r ARM7 -> 0x00008007 queued ... step=19395657        (delta 335,088 = the measured period)
...
done steps=30000001 distinct_pcs=11371 halt_pc=0x02008D30 halt=step budget exhausted
```

* The producer fires on the measured cadence (tick deltas 335,088 /
  335,072 steps at ~1 cycle/step) and the guest takes the IPC-recv IRQs
  (`irq enter` count 27 -> 92 at 30M, 266 at 60M), draining the FIFO
  (`recv_n=1` transient at each delivery).
* **The guest passes 0x02008D90** — the tail PC ring (last 4M steps) has
  the wait region only at indices 3,628,052..3,768,234 (steps ~29.6M to
  ~29.77M) and then runs the driver continuation (`0x0200BA40`,
  `0x02010088..0x02010094`) and the LZ decompressor
  (`0x02000984..0x020009D0`, 296,748 iterations each).  `distinct_pcs`
  grows 11,093 -> 11,371; a 60M-step run presents 113 frames.
* **Frames: still uniform, honestly.**  `NNK_GX_SHOT` captures at frames
  10..113 (both screens) are single-valued (0x00 at frames 10/20, 0xFF
  from 30 on) — the white backdrop, correct for a guest that has not yet
  written BG content.
* `SKIP_MELONDS=1 bash tools/verify_phase2a.sh`: green, rc=0 — strict
  common prefix 1,000/1,000 melonDS PCs, `RESULT: PASS`
  (log `p4r_verify.log`).

## 6. Honest stop-report — next barrier, named

* **First differing state (exact)**: the SDK wait on the same queue is
  still unsatisfied long after the producer was fixed.  The Phase-4R
  probe measures at every producer delivery through step 20,100,602:
  `snap = *(0x0206CA5C) = 0x00000000` and `seq = *(*(0x0206E580)) =
  0x00000000` — equal, so the wait loops.  The 60M run confirms it: the
  tail ring's wait region persists to step ~29.77M, the guest briefly
  visits `0x02010088..0x02010094`, ends in the SDK VBlank wait
  (`0x020078E8..0x02007904`, VCOUNT loop), and `distinct_pcs` is frozen
  at 11,371 from the 30M run to the 60M run; the 60M halt is
  `halt_pc=0x02008D28` (the wait loop's disable-IRQ helper).
* **Class (named, not fixed here)**: the queue sequence only moves when
  the card phase COMPLETES — in melonDS the next phase runs right after
  the requests (`0x81B34807`/`0x81B34E07`): the EXMEMCNT bus switch at
  ts 29,024,936, the ROMCTRL commands from ts 29,034,972, and CartXferDone
  (IF bit 19) pending at ts 29,034,056; the driver's ISR then posts to
  the queue and the wait ends (melonDS leaves the wait region within a
  141,230 ts IO gap, with NO IPC message and NO IRQ in that gap).  Native's
  scripted ARM7 acknowledges requests (Phase-4Q) and produces the
  measured heartbeat/tick stream (this phase) but does not PERFORM the
  requested card phase, so the completion never fires and the sequence
  stays 0.  Same class as Phase-4Q one level deeper: missing ARM7 request
  EFFECT, not missing reply.
* **Next bounded step**: MemWatch both runs on `0x0206CA58..0x0206CA70`
  (the queue object: snapshot at +4, head index at +20) and on
  `0x0206E580` (the sequence pointer) to name the exact first differing
  word and its writer; then give the responder the measured card-phase
  effect behind the requests `0x81B34807`/`0x81B34E07` (the Phase-4P
  card-stream model already produces ROMCTRL transfers — the missing
  piece is what re-arms it after the barrier).

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

# the wait-loop entry probe (caller + argument + stack):
#   NNK_DEBUG_AT_PC=0x02008D90 NNK_DEBUG_N=512 NNK_DEBUG_MEM=0x027E3980:0xC0
#   -> caller 0x0200AD08 (bl 0x02008D9C at 0x0200AD04), r0=0x54, r5=0x32
```

melonDS IO ground truth: `tools/melonds-io-trace.patch` build with
`MELONDS_IOTRACE_PATH=$S/p4q_mel_io.txt` (captured in Phase-4Q).
`SKIP_MELONDS=1 bash tools/verify_phase2a.sh` on this branch: §4.
