# Phase-4U — the real barrier behind the "read-queue order" stop-report: native never raises timer IRQs, so the SDK ready-thread queue starves after B7 #1556

Follows Phase-4T (merged `47fcc05`). Scope: root-cause the named barrier
(first differing queued card-read address at native step 18,388,672) with
measurements that kill the alternatives (Phase-4H standard), fix it with
measured semantics, and push toward the `0x0207CB10` DISPCNT switch.

## 0. Headline

* **Phase-4T's class was wrong: there is NO read-queue ORDER divergence.**
  The ordered B7 address sequences are exact prefixes of each other:
  pre-fix native's 1,557 B7 starts = melonDS's first 1,557, byte for byte
  (`p4u_b7seq.py`: *first index diff: None*; post-fix 1,733/2,279 also
  *None*).  The claimed first difference (native `0x000E3A00 n=1` vs melon
  `0x000E0C00 n=128` at block index 1360) is a **probe artifact**: during
  the E-burst (steps 18,386,614..18,388,672) a mode-5 DMA channel is armed,
  `nnk_card_stall` runs each transfer to completion in card time and the
  words pop through `nnk_dma_card`'s direct `card_fifo` reads, which bypass
  the Phase-4T blk/FIFO checksum hook (`io_read32` 0x04100010).  Native's
  blk list silently skipped those 24 blocks; the command addresses
  `0x000E0C00..0x000E3A00` were issued identically in both runs (cyc
  spacing 10,832 vs melon 10,856).
* **The real barrier is at B7 #1556** (`0x0DA6C400`: native step
  23,486,383 / melon ts 35,250,160, after its 2 B8s at native 23,492,835 /
  melon 35,261,546).  Native's SDK ready-thread queue drains
  `0x0206C770` (2,222 finds) -> `0x0206EA48` (1,258) -> `0x0206F540` (first
  and only at step 23,494,124) and at step **23,494,370 finds only the idle
  node `0x0206C6B0`** (state 1, resume pc `0x02007130`); the guest enters
  the idle dispatcher `0x0200712C` and WFI-loops at `0x02009118` forever
  (distinct PCs frozen at 14,498, last B7 at 23,492,730 pre-fix).
* **First differing state word: IF (0x04000214) bit4.**  melonDS arms TM1
  (pc `0x0200850C`/`0x02008514`, reload `0xFFFE`, ctrl `0xC1`) at ts
  35,262,870; the timer overflow raises **IRQ_Timer1 at ts 35,263,214**
  (ITCM stub acks `0x00000010`), and melonDS acks Timer1 **5,779 times in
  the capture** (first at ts 29,302,442, steady period 268,939 ts).  Native
  had **no timer IF source at all**: the only `iflags |=` sites were the
  measured `irq_raise_log` (ends step **9,097,800**), `ROMEndTransfer`
  (bit19) and the scripted IPC replies (bit18) — the whole run's delivered
  IF values are `0x00000000/0x00040000/0x00080000/0x00080008/0x000C0000`,
  never bit4.  With `IE=0x00040019` and the guest in WFI, nothing can wake
  the ready-thread queue, so the card stream dies after the read whose
  continuation is gated on the timer-driven dispatch.
* **Fixed and measured: the stream continues.**  B7 #1557 `0x0DA6C600` is
  issued at step 23,502,474 (melon ts 35,272,550; mapped drift +1,336 ts),
  contiguous after it; Timer1 IRQ delivered at step 23,494,423 with
  `if=0x00080010`.  B7 starts 1,557 -> **1,733**, distinct PCs 14,498 ->
  **15,158**, address sequence still an exact melonDS prefix.

## 1. Measurements that killed the alternatives

* Card data: identical through block #1359 (Phase-4T, unchanged).  Command
  addresses: exact prefix (above) — kills "queue order / wrong file
  stream"; the Phase-4T E3A00-vs-E0C00 claim is the DMA-pop probe gap.
* IPC: melonDS has 13 IPC-FIFO sends total, none between ts 34M and 35.3M;
  native's responder log ends at step 20,100,602 — both quiet at the
  barrier, so the ARM7 mailbox/reply timing is not the divergence.
* IE: both runs toggle `0x00040019 <-> 0x00040009` around the wait
  (native `iewr` pc `0x020060CC`/`0x02006100`, melon `W32 0x04000210` same
  pcs) — kills "IE mask gap".
* Queue object (native `NNK_CHECKPOINT` dump at step 23,520,000,
  `p4u_queue.py`): 9-node chain from `*(0x0206C6A8)=0x02079200`, nodes
  `state@+0x64 next@+0x68`, sentinel/idle node `0x0206C6B0`.  Walk probe
  (`NNK_DEBUG_AT_PC=0x02006E9C`, 15,404 hits): the final walk skips all
  8 data nodes (state 0) and returns only the idle node — the queue is not
  corrupt, it is *starved*: no waker fires.
* Timer waker: melonDS `W32 0x04000214 0x00000010` count 5,779 (first ts
  29,302,442) vs native zero after step 9,097,800 — the missing waker.

## 2. Fix (`tools/nnk_dma.inc`, Phase-4U block)

`p4u_timer_tick()` called from `nnk_dma_tick()` (once per dispatch, before
the delivery check at `run()`): for each timer enabled with its IRQ bit
set, count overflows in `cycles` since the enable edge (prescale from
`ctrl&3`, period `0x10000 - tm_reload`, melonDS `NDSTimer::Run` reload
semantics) and `iflags |= 1u << (3+t)`.  **Gated to steps > 9,097,800** so
the measured boot raise script (`irq_raise_log`) stays byte-exact for the
strict-prefix verification.  Nothing else changed; `nnktool/src/recompile.rs`
does not yet carry the model (same split as Phase-4L/4O — known
simplification).

## 3. Results (branch build, `out/split/arm9_gx`)

```
fix-validation (23.7M steps): irq enter step=23494423 if=0x00080010 (Timer1)
  B7 0x0DA6C600 step=23502474, 0x0DA6C800 step=23508624, ... contiguous
80M run (p4u_run80h): B7 starts 1733 (vs 1557 pre-fix, melon 2279)
  first B7 index diff vs melon: None (exact prefix)
  distinct_pcs 15158 (was 14498); B7s through step 24,913,870 (0x02CE0E00)
verify_phase2a.sh (SKIP_MELONDS=1): rc=0, PASS, strict prefix 1000/1000
```

## 4. Honest stop-report — no picture yet, next barrier named

* **Frames are still uniform white** (`p4u6_sh.00040.{top,bot}.ppm`: 1
  distinct byte value 0xFF) and `DISPCNT_A=0x0021111B`; the melonDS switch
  (`DISPCNT=0x0021021B`, pc `0x0207CB10`, ts 54,276,936) is still ahead —
  by the current mapping (~11.77M ts offset) around native step ~42M.
* **New barrier (named, not root-caused): every run of the fixed binary
  terminates at exactly step 27,371,209** (windowed under xvfb, headless
  80M limit, headless 30M limit — identical step): `nnk_display_tick()`
  returns nonzero, i.e. `gxd_should_quit()=1` from an SDL_QUIT event (only
  setter `gx_display.c:1337`, queued by `pump_events:1553`); the following
  `step_limit = steps` then rewrites the halt reason to "step budget
  exhausted".  Last B7 at step 24,913,870, then 2.46M steps without a card
  start — next task must (a) find who posts SDL_QUIT deterministically at
  frame ~54 (likely environment/DISPLAY; headless should ignore quit), and
  (b) compare the post-#1733 quiet phase against melonDS (legit long load
  vs a second starvation) and continue to the DISPCNT switch/picture.

## 5. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/read-queue   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c BUILD_JOBS=16 bash tools/build_split.sh out/split/arm9_gx

# barrier fix validation
NNK_CARDLOG=$S/p4u_nat_card2.log NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" \
  NNK_STEP_LIMIT=23700000 NNK_GX=headless \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin

SKIP_MELONDS=1 bash tools/verify_phase2a.sh     # rc=0 PASS 1000/1000
python3 $S/p4u_b7seq.py                         # first index diff: None (1733 vs 2279)
```

Evidence: `p4u_dbg.err` (queue walk probe), `p4u_mem2352.bin` (queue dump),
`p4u_run80h.{log,err}` (80M fixed run), `p4u_run237.{log,err}` (fix
validation), `p4q_mel_io.txt` (melon ground truth), `p4u_verify.log`.
