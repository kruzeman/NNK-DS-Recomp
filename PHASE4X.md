# Phase-4X — the VBlank IRQ source and the class-0x0B ARM7 handshake: the
# guest starts frame work and branches into the 0x02988xxx module (stop-report)

Follows Phase-4V2 (merged `66aaebf`).  Scope: ship the measured VBlank IRQ
source, make the ARM7 producer unbounded past step 3,768,189, push the guest
to frame work / the picture — or deliver the honest stop-report naming the
next barrier.  **Two measured barriers fixed, one new barrier pinned with
evidence; no picture yet (honest).**

## 0. Headline

* **Fixed 1 — the VBlank IRQ source.**  melonDS ground truth
  (`p4q_mel_io.txt`, 6M-event IO capture): the guest writes DISPSTAT=0x0008
  (bit3 = VBlank IRQ enable) at pc `0x02002BB8` ts 19,909,621; the ITCM stub
  then acks IF bit0 (`W32 0x04000214 0x00000001` at pc `0x01FF8038`) first at
  ts 20,015,653 and every 560,190 ts after (measured deltas 560,177..560,193)
  — exactly one frame (2130 cycles/scanline x 263).  Native was missing the
  source entirely (Phase-4V stop-report suspect 1).  `p4x_vblank_tick()`
  (tools/nnk_dma.inc, called from `nnk_dma_tick()`) now raises IF bit0 at the
  `vcount()<192 -> >=192` edge iff DISPSTAT bit3 is set — melonDS
  `GPU::StartVBlank` semantics — gated to steps > 9,097,800 so the measured
  boot raise script stays byte-exact (same gate as Phase-4U; the measured
  DISPSTAT enable lands far later, ~step 13.2M).
* **Result 1:** in the 60M run the VBlank IRQ is delivered **83 times**
  (first at step 14,199,466, cadence one per frame), `IE=0x00000019` had the
  source armed since boot — only IF was missing.  The guest's VCOUNT polling
  already saw real scanline progression (2130 cyc/line from `cycles`);
  what was missing was the IRQ, and it now fires.
* **Fixed 2 — the ARM7 producer's missing reply (the "unbounded producer"
  item).**  The periodic IPC rhythm (Phase-4R p4r) and the class-7 request
  effect (Phase-4S p4s) were already period-driven/unbounded; what was
  FINITE was the reply table: at native step 38,987,833 the guest sends IPC
  `0x0000002B` then `0x81BA702B` (pc `0x0200BAD4`) — a class-0x0B handshake
  — and the native ARM7 never answered ("no measured reply in the melonDS
  capture" — the Phase-4Q table predates the long IO capture), so the
  mailbox receive never completed, every thread parked, and the card stream
  died at step 38,981,422 (9,093 starts, last op B8).
* **melonDS ground truth for the handshake (whole capture — exactly one
  round, 6 sends / 5 replies):**

  ```
  send 0x0000002B   ts 50,484,317
  send 0x81BA702B   ts 50,484,371   (parameter word: bit31 set, no own reply)
  recv 0x0000006B   ts 50,485,392   (+1,075)
  send 0x000000AB   ts 50,485,825
  recv 0x0000006B   ts 50,487,241   (+1,416)
  send 0x000001AB   ts 50,487,701
  recv 0x0000006B   ts 50,490,173   (+2,472)
  send 0x000001AB   ts 50,491,108
  recv 0x0000006B   ts 50,494,439   (+3,331)
  send 0x000001AB   ts 50,495,455
  recv 0x0000006B   ts 50,498,023   (+2,568)
  ```

  Strictly alternating; commands are `(v & 0x1F) == 0x0B` with bit31 clear.
  `nnk_p4b_*` (tools/nnk_dma.inc) replies one `0x0000006B` per command at the
  measured gap (first 5 exact, then 2,500 = the mean).  Nothing is faked:
  the guest's mailbox handler really receives the word and really sends the
  next request after each reply (native sends land at steps 38,989,281 /
  38,991,090 / 38,994,311 / 38,998,469 — the measured melonDS alternating
  pattern reproduced).
* **Result 2:** the guest is unstuck: **distinct PCs 15,542 -> 15,742**, and
  it proceeds ~770 steps past the last reply into a code path that had never
  executed natively before.

## 1. The new stop-report (first differing word + class)

* After the last handshake reply (step 39,001,038) the guest branches at
  step **39,001,655** from overlay pc **`0x020D1A32`** (LR `0x020D1A21`)
  into **pc `0x02988AA0`** — main-RAM high space.  New probe
  (`p4x band:` lines, injected by tools/split_dispatch.py) shows ~250 Thumb
  instructions executing there (2- and 4-byte steps, r3 walking a pointer
  table in `0x020Cxxxx`, r0 = 0x65/0x4B/0x02/0x26/...).
* The run halts at step **39,001,806** with `dispatch to untranslated pc`
  in the same band; the checkpoint dump (`NNK_DUMP_AT_PC=0x02988CF6`)
  shows the **entire 0x02980000..0x02990000 region ZERO at halt** — the
  bytes that executed 150 instructions earlier are gone (the region is
  zeroed during/after the run-through).
* **First differing word:** native `pc 0x02988AA0` (step 39,001,655) vs the
  melonDS run, which at the analogous point (after the 5th `0x6B` reply,
  ts 50,498,023) continues its card streaming (B7s resume ts 53,007,794 from
  pc `0x020168EC`) and reaches the overlay-resident display switch
  (`DISPCNT=0x0021021B`, pc `0x0207CB10`, ts 54,276,936).  Native's card
  stream does **not** resume (starts frozen at 9,093, last op B8).
* **Class:** main-RAM MIRROR handling + the module download behind the
  stalled card stream.  The runtime models main RAM as one flat 24 MiB
  window (`NNK_GSIZE 0x01800000`); melonDS maps `0x02000000..0x03FFFFFF`
  to `MainRAM[addr & 0x3FFFFF]` (4 MiB mirror).  `0x02988AA0` aliases
  `MainRAM[0x188AA0]`; native's flat cell there is zero while the game's
  file table ("stone91.bin", "stone92Dummy.bin", ...) sits at flat
  `0x02188CF0` — the mirror-visible data set and the flat set differ.
  Two measured candidate mechanisms for the next task, in order:
  (a) the guest's module load/eviction targets the mirror address
  `0x02988xxx` while the payload arrives via flat-path writes (or vice
  versa) — a store watch on `0x02988AA0..0x02988D00` pins writer + step;
  (b) the module download is truncated because the card stream does not
  resume after the handshake — in melonDS the driver issues B7s again
  ~2.5M ts (4.5 frames) after the last reply; find which wait parks the
  native driver thread (likely on a counter/sequence the ARM7 must move).

## 2. Honest negatives

* Frames remain uniform (DISPCNT A = `0x0021111B` unchanged, all VRAM stores
  went to the LCDC bank, display switch `0x0207CB10` still ahead) — **no
  picture yet**, and no non-uniform frame is claimed.
* `starts=9093 last_op=0xB8` — the card stream did not resume within 60M
  steps; the p4r periodic producer keeps producing (words delivered to step
  284M+ in the earlier 300M run), so the producer is unbounded; the missing
  piece is guest-side (the parked driver thread), named above.

## 3. Verification (branch build `out/split/arm9_gx`)

```
60M run (p4x_run60b/60d): VBlank IRQ delivered 83x (first step 14,199,466),
    p4b handshake 5/5 replies delivered, distinct_pcs 15,742,
    halt at step 39,001,806 pc 0x02988CF6 (untranslated, diagnosed above)
verify_phase2a.sh (SKIP_MELONDS=1): RESULT PASS,
    strict common prefix 1000/1000 melonDS PCs (100.0%)
```

## 4. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/vblank   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK_GX=1 NNK_IMAGE_C=/home/hermes/NiNoKuni/work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  bash tools/build_split.sh out/split/arm9_gx
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 \
  NNK_GX=headless \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  NNK_DUMP_AT_PC=0x02988CF6 NNK_CHECKPOINT_MEM=$S/p4x_halt2.mem \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
SKIP_MELONDS=1 bash tools/verify_phase2a.sh      # RESULT PASS, 1000/1000
```

Evidence: `$S/p4x_run60{,b,c,d}.{log,err}` (native runs),
`$S/p4x_halt.mem` / `$S/p4x_halt2.mem` (guest-window dumps at the halt),
`$S/p4q_mel_io.txt` (melonDS IO ground truth), probe scripts
`$S/p4x_{melwin,acktally,melipc,burst,memlook*,case}.py`.
