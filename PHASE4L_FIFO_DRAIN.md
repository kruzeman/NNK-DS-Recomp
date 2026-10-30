# Phase-4L — the card-data FIFO drain, measured; the untranslated-PC wall removed

Follows Phase-4K (merged `beb6ac8`).  Scope: model the card-data FIFO drain at
`0x02016B38` (ROMCNT bit23/bit31 as the guest's read loop sees them), get the
run past the card-data phase, and push toward display init.

**Outcome: the drain is measured end to end and the loop provably completes
with melonDS's data-word sequence — but the real wall was never the FIFO.  It
was the untranslated PC `0x0215C034`; with decode-on-demand for unrooted PCs
the run advances from 13,676,011 to 20,000,001 instructions, and behind it the
next wall appeared: a DMA whose start-timing field the runtime decoded from the
wrong bits, which parked the guest in a 3-PC wait loop.  With that fixed the
run reaches **25,000,001 instructions and 9,029 distinct PCs** (5,217 at
Phase-4K) and executes the display/engine-init code around the DMA.  The
display powers on and the screen goes from black to configured-black: **no
picture content yet** (frames are uniform, no layer content).**

## 1. Ground truth: a new, longer capture

```sh
MELONDS_ARM9TRACE_PATH=$S/p4l_mel_pc.txt  MELONDS_ARM9TRACE_N=15000000 \
MELONDS_IOTRACE_PATH=$S/p4l_mel_io.txt    MELONDS_IOTRACE_N=1200000 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
```

15,000,000 ARM9 PCs and 1,199,999 IO accesses (`ts=` = `ARM9Timestamp >>
ARM9ClockShift`, the Phase-4D instrument).  It covers 936 complete 128-word
card transfers, i.e. the whole card-data phase, not just its start.

## 2. The drain sequence as the guest's loop sees it (melonDS)

`tools/p4l_io_drain.py` (`scratch/p4l_io_drain.py`, per-transfer view) on the
first transfer, `ROMCnt = 0xA1416657`, `datasize = 0x200`, cmd `0xB7`:

| quantity | measured |
|---|---|
| ROMCNT value at every poll while busy | `0xA1416657` — **bit31 = 1, bit23 = 0** |
| first poll with bit23 set | the **910th** ROMCNT read (0-based 909) |
| polls with bit23 set | **128**, one per data word |
| `ts` poll -> poll | 9 cycles (constant) |
| first word - first poll | 8187 cycles (word scheduled 8180 after the write) |
| per-word polls between data reads | `2,2,1` repeating (period 3) |
| total ROMCNT polls per transfer | **1121** (909 + 212) |
| data words | 128, then bit31 clears on the read that empties the FIFO |

The words are the ROM bytes at the commanded file address: transfer 1 reads
`0x1D126600` and returns `20152253 7A003214 04023052 83A30001 ...`, which is
byte-for-byte the ROM image there (checked against the `.nds`).

## 3. The native model against the same measurements

Native at the Phase-4K divergence transfer (`tools/p4l_drain.py` on the PC
traces, both aligned on the 5th decryptor entry `0x0215B468`):

| quantity | melonDS | native |
|---|---|---|
| data reads | 128 | **128** |
| first-word wait | 909 polls | 910 counted (the DRQ poll included — Phase-4D convention) |
| per-word polls | `2,2,1` repeating | `2,1,2,1,2,2,...` (phase wanders) |
| total polls | 1121 | **1113** (8 fewer) |
| loop-body PCs in the run | 6122 | 6082 |
| copied words | ROM[addr] | **byte-identical** |

The *contents* side is exact: the native guest buffer at `0x0206F120` (where
`0x02016B44` stores each word) is a 512-byte slice that matches the ROM
exactly at `0x1AB800` (`p4l_words.py`: 512/512 bytes equal).  The *cadence*
side carries the Phase-4D residual (per-instruction pipeline cost): native
charges instruction costs at the top of the dispatch while melonDS charges
them when the instruction completes, so the word-arrival phase relative to the
polls drifts until one poll sees bit23 clear where melonDS sees it set.

That is exactly the stop point Phase-4K named: 86,059 instructions after the
5th tap both traces are identical, and then

```
mel 0x02016B3C   (bit23 set -> data read)
nat 0x02016B4C   (bit23 clear -> beq taken)
```

**This is a one-poll slip, not lost data.**  The loop re-synchronises three
iterations later (both at `0x02016B4C`), native reads all 128 words in the
same order, and the run continues.  The drain loop is therefore not a barrier
by itself; the barrier was elsewhere.

## 4. The real wall: `0x0215C034` is not in the compiled image

Both traces were re-compared with the anchor at the 5th and 10th decryptor
entry (`p4l_cmp.py`): from the 10th tap, **17,183 instructions are identical
and then the native trace ends** — at `0x0215C034`, `halt=dispatch to
untranslated pc`.  melonDS executes that address (measured: it is the
`ldmfd sp!,{pc}` tail of the arena routine at `0x0215C014`, reached at
13.2M ARM9 instructions, right after the ten decrypt passes).

Root cause: the generated image carries exactly the recovery roots; a PC that
only the *later* trace reaches has no case, and `nnk_smc_try()` cannot help
because it refuses to re-decode a PC with no generation-0 word
(`smc_img_word`).  So the dispatch default halted on perfectly valid code.

Fix (Phase-4L): `nnk_smc_exec_any()` in `tools/nnk_smc.inc` decodes the LIVE
word of an unrooted PC (ARM first, Thumb fallback, the same decoder the
generation tracker uses) and executes it in place; `tools/split_dispatch.py`
calls it in the dispatch default before the honest halt.  A word that decodes
as neither mode still halts with the unchanged message, so a real
data-as-code jump is not papered over.

## 4b. The next wall: the DMA start-timing field (bits 27..29, not 24..25)

With `0x0215C034` unblocked the run walks 6M instructions further and then
parks in a 3-PC loop:

```
0x0200929C  ldr r0,[r4]        ; r4 = 0x040000DC (DMA3CNT)
0x020092A0  tst r0,#0x80000000
0x020092A4  bne 0x0200929C     ; wait for bit31 to clear
```

Measured on both sides:

| | melonDS (`p4l_mel_io.txt`) | native (before the fix) |
|---|---|---|
| write DMA3CNT | `0x85000018` at `0x01FF84D0` (ITCM), ts=20387028 | same write |
| first read after | `0x05000018` (bit31 **clear**), ts=20387141 | `0x85000018` **forever** |
| where it ends up | display init: DISPCNT `0x00011100` (ts 20.40M), VRAMCNT writes (20.85M), DISPCNT `0x0021111B` (20.86M) | stalled: 2,500,001 traced PCs, **3 distinct PCs**, from <19M to >60M instructions |

Root cause (code-level): the ARM9 start-timing field is `DMA_CNT` bits 27..29
(melonDS `DMA::WriteCnt`: `StartMode = (Cnt >> 27) & 7`).  The runtime's
`io_do_dma()` read `(cnt >> 24) & 3`; for `0x85000018` that yields 1 ("timed,
not immediate") although the field is 0 (**immediate**), because bits 24 and
26 in that value are the source-address control (fixed) and the 32-bit size
flag.  `io_do_dma()` therefore returned without running the transfer and
without clearing bit31 — the guest waited forever for a DMA that had in fact
completed 113 cycles later on hardware.

Fix, two parts:

* `nnktool/src/recompile.rs` — the authoritative mask fix (`(cnt >> 27) & 7`)
  for the next image regeneration.
* `tools/nnk_dma.inc` (new, spliced by `tools/split_dispatch.py`, called once
  per dispatch) — because the current image
  (`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`) was generated before the fix and
  regenerating 25 MB of C was out of scope: the shim completes an enabled
  immediate-mode channel that `io_do_dma()` missed, with melonDS
  `DMA::Start`/`WriteCnt` semantics (word/half, source/destination
  increment/decrement/fixed, repeat bit), and triggers vblank (1) / hblank (2)
  channels on the corresponding `vcount` edges, which the runtime never did at
  all.  The copy really happens and the enable bit is really cleared.

Measured effect of the shim (25M-step run):

```
dma: shim completed DMA3 cnt=0x85000018 count=24 word 0x040000EC->0x04000008 pc=0x01FF84D0
dma: shim completed DMA3 cnt=0x8500001C count=28 word 0x040000EC->0x04001000 pc=0x01FF84D0
dma: shim completed DMA3 cnt=0x85000004 count=4  word 0x040000EC->0x04000330 pc=0x01FF84D0
done steps=25000001 traced=25000000 distinct_pcs=9029 ... halt=step budget exhausted
```

`distinct_pcs` rises from 5,332 to **9,029** — the run now executes the
post-DMA display/engine-init code instead of spinning on one register.

## 5. Measured result

Same command as Phase-4K (stored guest image, live gx bridge), 20M-step
budget:

```sh
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless NNK_STEP_LIMIT=20000000 \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

| | Phase-4K | Phase-4L (decode-on-demand) | Phase-4L (+ DMA shim) |
|---|---|---|---|
| steps | 13,676,011 | 20,000,001 | **25,000,001** |
| halt | untranslated `0x0215C034` | step budget | **step budget** |
| distinct PCs | 5,217 | 5,332 | **9,029** |
| gx frames presented | 30 (all black) | 41 | 50 |

Frames (headless SDL, `NNK_GX_SHOT`, PPM every 4th/6th frame,
`p4l_ppmstat.py`): with decode-on-demand only, frames 0-31 are pure black
`(0,0,0)` and 32+ are pure white `(255,255,255)` — the "LCD on, nothing
composed" state; the guest never got past the DMA wait to program DISPCNT.
With the DMA shim the run does program the display (nine new code pages,
9,029 distinct PCs) and every frame up to 48 is pure black `(0,0,0)` again:
the screen is configured but no layer content is composed.  Honest reading:
the display is initialised, **the first picture is still not there** — a
uniform frame in both directions (white = unpowered/empty, black = configured
and empty) is not evidence of a picture, so no picture claim is made.

## 6. Script hygiene (Phase-4L)

* `tools/verify_phase2a.sh` and `tools/verify_phase1a.sh` no longer `cd` to
  `/home/hermes/NiNoKuni`; they `cd "$(dirname "$0")/.."`, so a worktree run
  tests the worktree's code.  `verify_phase2a.sh` also fails early with a
  clear message when `./work` is missing instead of silently using another
  checkout's state.
* `tools/build_split.sh` re-execs itself under bash when started by a POSIX
  shell (`sh tools/build_split.sh` used to die on `set -o pipefail`), and the
  header now documents the two supported invocations.

## 7. Verification status

* `tools/verify_phase2a.sh` run **from this worktree** with `SKIP_MELONDS=1`
  (`p4l_verify.log`): A/A2/B/C1/C2 all report PASS, step D builds and runs the
  mono image (`done steps=20000001 ... halt=step budget exhausted`), step E
  reuses the melonDS trace and step F reports

  ```
  1) strict common prefix        : 1000 / 1000 melonDS PCs (100.0%)
  2) window-filtered prefix      : 1000 / 1000 (window 0x02000000..0x02400000)
  3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
  ```

  i.e. the script now exercises THIS checkout instead of the main one (the
  Phase-4K harness gap is gone) and the acceptance checks stay green.
* Build: `NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c
  SPLIT_DIR=work/split/c tools/build_split.sh out/split/arm9_gx` —
  `BUILD OK`; after the Phase-4L fragment changes only `nnk_main.c`
  recompiles (`1 compiled, 31 reused`).
* All acceptance-relevant runs used `work/phase3a/arm9_stored_padded.bin`
  (the stored image) through the `work` symlink to the main checkout.
* The `recompile.rs` DMA-mask fix is authoritative but not in the image this
  branch ships (`arm9_thumb_ovl_itcm_p4j.c` predates it); `tools/nnk_dma.inc`
  covers the current image.  A future image regeneration picks up both.

## 8. Next frontier

The run now executes the display/engine-init code (9,029 distinct PCs, budget
exhausted at `0x02007138`) with the display programmed and the framebuffer
still uniform.  Concrete next measurements, in order:

1. **Where does the run sit at 25M?**  A PC histogram of the last traced
   window (`p4l_pchist.py`) decides between "still making progress, needs a
   bigger budget" and "new wait loop".  If it is a wait loop, the register it
   polls is the next model gap (the pattern of §4b).
2. **DMA sources that look wrong**: the shim logged
   `0x040000EC -> 0x04000008 / 0x04001000 / 0x04000330`, i.e. copies whose
   *source* is an IO register address.  That is what the guest wrote, but it
   is worth confirming against melonDS's DMA3 SAD/DAD writes before trusting
   the destination content.
3. **VRAM bank mapping**: `0x04000240..0x04000249` are still store-only in the
   runtime IO model, while melonDS writes `0x82,0x83,0x84,0x81,0x8B...` there
   at ts 20.85M.  A layer whose tile map lives in an unmapped bank renders
   empty — that is the most likely reason a configured screen stays uniform.
4. **The poll-cadence residual** (1121 vs 1113 ROMCNT polls per transfer).
   Same class as Phase-4D's pre-loop offset: it needs melonDS's per-instruction
   pipeline accounting, not another fitted constant.  It does not change the
   data words and does not block progress.

## 9. Files

* `tools/nnk_smc.inc` — `nnk_smc_exec_any()` (decode-on-demand) + the
  `on_demand` counter in the SMC stats line.
* `tools/nnk_dma.inc` (new) — DMA completion/timing shim: completes an enabled
  immediate-mode channel that the stale prelude's `io_do_dma()` misread, and
  triggers vblank/hblank channels on the `vcount` edges.
* `tools/split_dispatch.py` — call `nnk_smc_exec_any()` in the dispatch
  default before the halt; splice `nnk_dma.inc` and call `nnk_dma_tick()` once
  per dispatch; copy the new fragment next to the generated TU.
* `nnktool/src/recompile.rs` — authoritative `io_do_dma()` fix: the ARM9
  start-timing field is `(cnt >> 27) & 7`, not `(cnt >> 24) & 3`.
* `tools/verify_phase2a.sh`, `tools/verify_phase1a.sh` — repo-root `cd`
  (Phase-4L hygiene), missing-`work` guard.
* `tools/build_split.sh` — bash re-exec guard + documented invocations.

Guest images, arenas, traces and frames stay under gitignored `work/` and
scratch.  No ROM bytes are committed.
