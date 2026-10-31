# Phase-4T — the card-stream deadlock (Phase-4N end-event), byte-identical stream proof, and the read-queue-order barrier

Follows Phase-4S (merged `8b27d97`).  Scope: (a) VRAM store routing,
(b) the `0x0207CB10` control-flow gap, (c) the picture — with the Phase-4H
stop-report standard as the fallback.

## 0. Headline

* **(b) Root-caused and fixed: the card stream was deadlocked by a
  Phase-4N model error, not by data.**  `card_advance_receive` scheduled
  `ROMTransfer_End` after the last *received* word (Phase-4N inject (a)), but
  melonDS's `ROMAdvanceReceive` (read mode) returns without scheduling —
  the read transfer ends only when the CPU pops the last FIFO word
  (`ReadROMData -> ROMEndTransfer`).  The scheduled end cleared busy+DRQ
  ~20 cycles after the last word was produced; the guest's B8 dummy-read
  helper polls for DRQ (`tst r0,#0x800000` at `0x02016978`) and deadlocked
  the whole stream the first time the race was lost (step 20,100,589).
* **The stream is now byte-identical to melonDS for 1,360 consecutive
  512-byte blocks** (per-block FNV-1a checksums over the FIFO words, native
  vs `p4q_mel_io.txt`, `n=128` on both sides).  B7 starts 1,398 -> 1,557,
  distinct PCs 13,828 -> 14,498; the guest reaches new code (SDK idle loop
  `0x0200911C`) instead of the frozen ROMCNT poll.
* **Honest stop-report (exact first differing word + class).**  First
  difference: **the queued card-read ADDRESS at native step 18,388,672** —
  native starts a B7 read of ROM `0x000E3A00` (1 word, then aborted),
  melonDS reads `0x000E0C00` and streams it contiguously.  Class: **SDK
  read-queue order divergence at the ARM7-IPC reply boundary** — card data
  is verified identical through block #1359 and the IPC handshake matches
  (send pair `0x80004046`/`0x40400006`, reply `0xC0204006` 2,076 cycles
  later), so the divergence is which queued file stream is consumed first,
  not bytes or IPC.  Native's queue then dries at block 1,534
  (step 23,492,835) and the guest idles in WFI — the melonDS display switch
  (`DISPCNT=0x0021021B` from pc `0x0207CB10`, ts 54,276,936) sits behind
  this barrier.  DISPCNT_A stays `0x0021111B`; frames are uniform white
  (backdrop) — honest.
* **(a) VRAM store routing is NOT the barrier — verified equivalent to
  melonDS.**  In melonDS, LCDC-window stores (`GPU.h:222 WriteVRAM_LCDC`)
  write the **physical bank** raw, selected by the fixed LCDC layout; the
  ABG/BBG/AOBJ/BOBJ windows route through the `MapVRAM` tables.
  `gxrender/src/gx_display.c vram_write_byte` does exactly both (LCDC:
  `g->vram[bank][a - lcdc[bank].base]`, windows: `map_*[idx]` mask).  The
  guest's 327,680 LCDC stores (steps 13,908,717..14,059,910, all banks
  temporarily `0x80`) land in the same physical banks in both runs — "stores
  go to LCDC" is the *correct* hardware behaviour, not a misroute.  The
  framebuffers are empty because the guest never reaches its drawing code
  (the gap above), not because stores are dropped.  A byte-level diff of the
  physical blocks at the same run point would need a melonDS VRAM-dump
  instrument (none exists yet; named as optional follow-up tooling).

## 1. The stall, measured (before the fix)

150M-step run (`p4t_run150.err`, `NNK_GX=headless`): `distinct_pcs=13,828`
frozen from 30M to 150M, `halt_pc=0x02016974/7C` — the ROMCNT poll loop.

New Phase-4T probes (spliced by `tools/split_dispatch.py`, defined in
`tools/nnk_vramcnt.inc`): every card start logged with op/addr/size/cycles/
steps, a dead-transfer dump, busy-wait heartbeats, and a per-block FIFO
checksum.

* The B7 address sequence **matched melonDS for all 1,398 starts**
  (`p4t_b7_cmp.py`) — the stream content was never the problem.
* Last starts: `B7 0x1BB6D000 step=20,094,382`, then `B8 step=20,100,589` —
  and no card command ever again.  PC trace of steps 19.9M..20.4M: the FIFO
  reader (`0x02016B30..50`) disappears at step ~20,100,469 and only the
  3-instruction ROMCNT poll (`0x02016974/78/7C`) remains.
* No dead-transfer dump, no busy-wait heartbeat: on every poll read busy was
  already clear (the premature end had fired) — the guest waited for a DRQ
  that had been dropped.

## 2. Root cause (melonDS `src/NDSCart.cpp` exact)

```
ROMAdvanceReceive():                    card_advance_receive() (was):
  if (ROMTransferPos >= ROMTransferLen)   if (card_pos >= card_len) {
      return;   // NO end scheduled          card_end_pending = 1;
  }                                         card_next = card_cycles + d;
  // schedule next ReceiveData               return; }
```

In read mode melonDS never schedules `ROMTransfer_End` from the receive
advance: the last word sits in the FIFO with DRQ raised until the CPU pops it
(`ReadROMData`: `if (ROMTransferPos >= ROMTransferLen) { if (ROMDataCount == 0)
ROMEndTransfer(0); ... }`).  Phase-4N added the end-scheduling under the label
"ROMAdvanceSend semantics" — that is the WRITE-mode path — and its measured
effect at the time was none (the stream was dead upstream).  With the stream
live (Phase-4S wake), the end fires ~20 cycles after the last word is
produced, clearing busy **and** DRQ; the B8 dummy-read helper
(`0x02016940..0x0201698C`) starts a 4-byte transfer and polls for DRQ — a
window of ~20 cycles it must hit from a ~3-instruction poll loop.  1,397 B8s
won the race; the one at step 20,100,589 lost, and the SDK never recovered.

## 3. Fix

* `tools/split_dispatch.py`: the Phase-4N inject (a) (end-of-transfer
  scheduling) is removed — the pre-generated image
  (`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`) already carries melonDS's exact
  `if (card_pos >= card_len) return;`; the Phase-4N inject had been
  replacing it.  Injects (b) (cart IRQ raise in `card_end_transfer`) and
  (c) (rule-based IRQ delivery) stay.
* `nnktool/src/recompile.rs`: `card_advance_receive` updated to the same
  melonDS-exact semantics (authoritative for the next image generation).

## 4. Numbers (60M steps, stored image, `out/split/arm9_gx`)

| metric | before (Phase-4S build) | after (Phase-4T) |
|---|---|---|
| B7 starts | 1,398 (frozen at step 20,094,382) | **1,557** (last at step 23,492,730) |
| B8 starts | ~1,400 (frozen) | **2,495** |
| distinct PCs | 13,828 | **14,498** |
| halt pc | `0x0201697C` (ROMCNT poll) | `0x0200911C` (SDK WFI idle) |
| per-block FIFO checksums vs melonDS | — | **1,360 / 1,360 identical**, first diff #1360 |
| DISPCNT_A | `0x0021111B` | `0x0021111B` (display switch still behind the queue barrier) |

Evidence: `p4t_run60b.{log,err}` (block checksums `p4t: blk addr=... crc=...`),
`p4t_mel_blkcrc.txt` (melon side), `p4t_blkcrc_cmp.py` (comparison),
`p4t_run150.*` (pre-fix stall), `p4t_run22.log` (PC trace of the stall),
`p4q_mel_io.txt` (melonDS IO ground truth, 6M accesses, ts to 1.58e9).

## 5. Stop-report — exact first differing word, class, next probe

* **First differing value:** the next queued card-read address at native
  step **18,388,672** (`p4t: start op=0xB7 addr=0x000E3A00`) vs melonDS's
  `0x000E0C00` at the same stream position (block #1360, right after the
  matched `0x0004D400`).  Native's read returns 1 word and is aborted;
  melonDS streams `0x000E0C00 -> 0x000E0E00 -> 0x000E1000` contiguously.
* **Class:** guest read-queue ORDER divergence — two real file streams
  (`0x000E0C00`-file and the `0x1BB6xxxx`-file, which melonDS also reads
  later) are consumed in opposite order.  Excluded causes: card data
  (checksums identical through #1359), the IPC handshake (identical send
  pair + reply latency), the VRAM/2D register state (matches, Phase-4M).
  Remaining candidates: the guest's queue-fill control flow before step
  18.39M (a register/RAM read whose value native serves differently), or
  queue entries the real ARM7 posts into shared RAM around the reply that
  the scripted responder does not.
* **Named next probe (bounded):** `MELONDS_MEMWATCH_*` on the SDK read-queue
  objects in BOTH runs around melon ts 28.45M..28.71M / native steps
  18.38M..18.56M (the producer PC of each queue entry decides it), or a
  melonDS ARM9 PC trace to N=26M plus a native `NNK_TRACE_FROM` window for
  the strict-prefix diff (Phase-4H standard).  The display switch at
  `0x0207CB10` is expected to unlock once the queue order matches.

## 6. Verification

* `SKIP_MELONDS=1 bash tools/verify_phase2a.sh` from this worktree:
  `RESULT: PASS`, rc=0 (`p4t_verify.log`).
* All recompiled-image runs use `work/phase3a/arm9_stored_padded.bin`
  (the stored image) through the `work` symlink.

## 7. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/vram-routing   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c BUILD_JOBS=16 bash tools/build_split.sh out/split/arm9_gx

NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin > $S/p4t_run60b.log 2> $S/p4t_run60b.err

python3 $S/p4t_mel_blkcrc.py $S/p4q_mel_io.txt > $S/p4t_mel_blkcrc.txt
python3 $S/p4t_blkcrc_cmp.py $S/p4t_run60b.err $S/p4t_mel_blkcrc.txt   # 1360 identical blocks
```
