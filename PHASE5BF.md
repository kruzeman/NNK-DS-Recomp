# Phase-5BF — the AOBJ tile upload at the "Skip the cutscene?" prompt: every 32-bit DMA is executed as a 16-bit copy, so exactly HALF of each tile block is written

Follows Phase-5BE.  Branch `wt/t_5aa1bcc2`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh work/p5bf/nnk_runner`

**Result: stop-report (the card's second acceptance branch) — but the defect is
now named to the byte, per transfer.**  The missing AOBJ bytes are *not* a
dropped block, a stride, or a capped queue: the guest arms ordinary ARM9 word
DMAs of the exact length melonDS uses, and our immediate-DMA model executes
each one as a **16-bit** copy of **half** that length.  Every AOBJ upload
therefore lands its first half and loses its second half — the "period-8 /
4-tile (128-byte)" fingerprint is simply `512 B written / 512 B lost` inside
each 1 KB (256-word) burst.

The corrected model is implemented and verified to fill the AOBJ exactly
(`NNK_DMA_FIX=1`), but enabling it by default moves the guest off the melonDS
VRAMCNT layout at the prompt frame and the frame renders black (§5).  That
regression is a separate, unnamed defect, so the fix is left **opt-in** and
the shipped build's behaviour is unchanged (§6).

## 1. Reproduce and measure (baseline, unmodified model)

Healthy state: `work/repro_5bb/st3400.nnks` (step 3.4e9, frame 6076, inside the
in-game cutscene) + `work/repro_5bc/start.nnk` (START on frames 6170-6189).
Cold boot to step 3.47e9 (≈6 min 20 s; a resumed run of the same window is
2.8 s, which is the iteration loop used below).

GPU state at presentation frame 6182 vs `work/phase5ba/melds_gpu/`:

```
palette  EQUAL          vramA EQUAL          vramE..I EQUAL
oam      DIFF 491/661   vramB 3 186 / 6 546 nonzero   (3 371 bytes never written)
vramC/D  different picture (the wrong-film / stream-2 decode card)
```

Per 128-byte block of bank B (`work/p5bf/blockcov.py`):

```
blk 0 EQUAL   blk 1 LOST(121)   blk 2 EQUAL   blk 3 LOST(116)
blk 39-51 EQUAL   blk 52 ours 64 B of 128   blk 53-65 LOST
blk 66-77 EQUAL   blk 78-89 LOST
```

i.e. alternating *halves* of contiguous runs — not a periodic skip.

## 2. The writer, per transfer

`NNK_AOBJ_TRACE=1` + `NNK_DMA_TRACE=1` (new diagnostics, §7) on a cold boot.
Every store into 0x06400000..0x0643FFFF comes from **one** guest PC,
`0x01FF84D0`, and every store of one burst shares one `step`, i.e. it is a
DMA, not a copy loop.  0x01FF84C8..D0 is the ITCM DMA helper
(`str r9,[r1]` / `str r8,[r1,#4]` / `str r6,[r1,#8]`, r1 = 0x040000D4 = DMA3
SAD/DAD/CNT), confirmed in the generated image
(`work/phase3a/arm9_thumb_ovl_itcm_p5u.c` cases 0x01FF84C8-0x01FF84D0).

The prompt's tile upload is these five DMA3 transfers (dst = VRAM offset in
the AOBJ bank):

| src | dst | cnt | count | our bytes written | melonDS bytes |
|---|---|---|---|---|---|
| 0x022CF080 | 0x06400000 | 0x84000040 | 64 | 128 (blk 0) | 256 (blk 0-1) |
| 0x022CFE7C | 0x06400100 | 0x84000040 | 64 | 128 (blk 2) | 256 (blk 2-3) |
| 0x022D0900 | 0x06401380 | 0x84000360 | 864 | 1 728 (blk 39-52) | 3 456 (blk 39-65) |
| 0x022D18F8 | 0x06402100 | 0x84000300 | 768 | 1 536 (blk 66-77) | 3 072 (blk 66-89) |
| 0x022D2C90 | 0x06402D00 | 0x84002B40 | 11072 | 22 144 (blk 90-182) | 44 288 |

`0x84000360 = enable + bit26 + 0x360`, so melonDS (`DMA::Run9`, `Cnt & (1<<26)`
→ `ARM9Write32`, `SrcAddrInc<<2`) copies 864 **words** = 3 456 bytes —
exactly blocks 39-65, which is exactly what melonDS's bank B holds.  We copy
864 **halfwords** = 1 728 bytes = blocks 39-52.  Same for the 768-word burst
(0x2100..0x2D00 = blocks 66-89) and the two 64-word bursts (0x0000/0x0100,
each 2 blocks → 1 written, 1 lost).  Verified against melonDS: where we did
write (blocks 39-51, 0/2, 66-77) our bytes are **equal** to melonDS's; only
the second halves are missing.

Mechanism, in one line — `io_do_dma()` in the generated prelude (and the same
lines in `nnktool/src/recompile.rs`):

```c
int words = (cnt & (1u << 30)) != 0u;   /* bit30 is the ARM7 layout */
```

The ARM9 transfer-size bit is **bit26** (melonDS `DMA::Run9`), so every
`0x84xxxxxx` burst is copied as halfwords: `count` halfwords from an
incrementing source = exactly half the programmed byte length.  (The count
mask, `& 0x7FFF` vs the ARM9's 21 bits, is also wrong but inert here — the
largest observed count is 24 576 < 0x8000.  Source/destination address
control is also ignored, `s += step; d += step;`, which is wrong for the
`0x85xxxxxx` fixed-source fills but those are executed by the Phase-4L shim,
which honours it.)

The `0x85`/`0x84` split explains the "every other transfer survives" look:
`0x85000100` bursts (fixed source 0x040000EC = DMA3CNT_H used as a fill
pattern, e.g. the OAM clears at 0x020700C0 → 0x07000000 and 0x02070600 →
0x07000400) were skipped by `io_do_dma`'s old `(cnt >> 24) & 3` timing mask
and executed correctly by `tools/nnk_dma.inc`'s shim; only the `0x84` ones
took the broken path.  That is why palette/bank A/OAM looked "mostly right"
while dense tile data lost its second half.

## 3. The corrected model (implemented, opt-in)

`tools/split_dispatch.py` now splices the per-melonDS immediate-DMA model —
count `& 0x001FFFFF` (0 → 0x200000), size bit **26**, `sadir`/`dadir` from
bits 23/24 and 21/22, and the StartMode field bits 27..29 — into the
generated prelude, **gated on `NNK_DMA_FIX=1`**.  With it enabled the AOBJ
coverage becomes complete:

```
default : aobj: stores=12896 blocks_written=202 map=#.#....................##############.............############............######
NNK_DMA_FIX : aobj: stores=12896 blocks_written=403 map=####...................................#########################################################
```

i.e. blocks 0-3 and 39-95 (the whole atlas region the guest uploads) are all
written, and the two bursts' lengths are exactly the melonDS ones.

## 4. Why the fix is not enabled by default — measured, not assumed

A cold boot with `NNK_DMA_FIX=1` does not reproduce the melonDS frame.  At
presentation frame 6182:

```
melonDS        VRAMCNT A..I = 81 82 89 91 00 85 00 00 00   (measured, 5BA)
our shipped    VRAMCNT A..I = 81 82 89 91 00 85 00 00 00   (EQUAL)
NNK_DMA_FIX=1  VRAMCNT A..I = 82 83 8B 84 81 83 8B 81 83   (differs)
```

The guest programs a *different* VRAM bank layout, so the flat ABG/AOBJ
windows are built from different banks and frame 6182 renders black
(`work/p5bf/shots3/f.06180.bot.png`); the intro is still correct at frame 5000
(`work/p5bf/s5000.png`), so the divergence happens in the cutscene/prompt
window, not at boot.  Both models agree bit-for-bit on the *pre*-fix
signature when resumed from the same state (`NNK_DMA_LEGACY_SIZE=1`), so the
DMA splice is the only difference.

Since the guest only ever gets *more correct* DMA data from the fix, the
divergence means some other runtime model depends on the truncated writes —
the next card's job.  Candidates, in order of suspicion, all measurable with
the diagnostics in §6: the guest reads VRAM/OAM back through the Phase-5X
shadow (`gxd_sh_rd`) and takes a bank-layout decision from what it reads; the
OAM window model (0x07000000 A / 0x07000400 B, 2 KB in `gx_2d.h`) vs melonDS's
1 KB+1 KB; or a 0x40-byte `0x84000080` copy into 0x0689A000/0x06600000 whose
full-length write now lands in the LCDC window instead of being truncated.

## 5. What the prompt frame still needs after this card

1. **The DMA divergence** (§4) — then `NNK_DMA_FIX=1` becomes the default and
   AOBJ matches melonDS's 6 546 nonzero bytes (the two atlas bursts alone add
   the 3 371 missing bytes: 1 728+1 536 = 3 264 from blocks 39-89, plus
   128+128 from the two 64-word bursts' second halves).
2. The background banks C/D (the `stream=2` residual-vs-reconstructed decode,
   Phase-5AX/5BD territory) — untouched here, quantified in 5BA §4.
3. OAM still differs by 170 bytes (the same half-length copy applied to the
   OAM bursts 0x020700C0 → 0x07000000, 0x02070600 → 0x07000400: our OAM has
   491 nonzero bytes vs melonDS's 661).

## 6. Repository state

* **No behavioural change by default.**  A rebuild reproduces the pre-5BF
  coverage signature exactly (`blocks_written=202`,
  `map=#.#...##############.............############............######`), and
  `git diff` touches only diagnostics + the opt-in splice (§7).  The 62-test
  suite, `verify_phase2a.sh`, the intro band correlation, the s01 bit-exact
  presentation, the 5BB `NNK_EXC_TRACE` signature and the savestate round
  trip are therefore untouched by construction (no rendered byte, no guest
  byte and no savestate field changed: the new state is carried by the
  existing `io` region and was exercised by the cold boot -> state -> resume
  round trip in §1).
* `nnktool/src/recompile.rs` keeps the *old* lines with a "READ THIS BEFORE
  FIXING IT" comment naming the exact wrong fields and pointing here, so the
  next image regeneration does not silently reintroduce or un-fix them.
* No new runtime fragment file, so `tools/nnk_prelude_manifest.txt` needs no
  new row.

## 7. Diagnostics added (all env-gated, no state semantics)

| env | where | what |
|---|---|---|
| `NNK_AOBJ_TRACE=1` | `tools/nnk_vramcnt.inc` | every routed store into 0x06400000..0x0643FFFF with `step, pc, addr, width, value, blk`, plus an unconditional per-block **bytes-written map** in the exit report (`aobj: stores=N blocks_written=M map=...` and one line per block with `bytes` and `first_pc`) |
| `NNK_DMA_TRACE=1` | `tools/split_dispatch.py` (immediate-DMA log) | per transfer `cnt, count, words, src, dst` |
| `NNK_DMA_FIX=1` | `tools/split_dispatch.py` | per-melonDS immediate-DMA model (§3) |
| `NNK_DMA_LEGACY_SIZE=1` | same | forces the shipped (truncating) model even when `NNK_DMA_FIX` is set |

The per-block coverage map is the "diagnostic that reports per-transfer AOBJ
coverage" the card asked for: it turns "3 371 bytes are missing" into "these
blocks got 0 bytes, these got exactly half of what the DMA asked for".

## 8. Evidence (under `work/p5bf/`, never committed)

`r8.log` cold boot to the prompt (pre-fix, per-transfer trace) ·
`r12.log` same with `NNK_DMA_FIX=1` (full-length transfers) ·
`r3.txt` per-store AOBJ trace (two DMA bursts, one step each) ·
`try_DEF/`, `try_LEG/`, `try_A/` state dumps + `melds_gpu_diff.py` output for
the default / legacy / fixed model from one state ·
`shots3/f.06180.bot.png` (black, fixed model) · `s5000.png` (intro still
correct, fixed model) · `blockcov.py`, `covsum.py`, `dmatr.py`, `ppm2png.py`,
`p5bf_try.sh` (analysis + the 3-second resume loop) ·
`pre_prompt2.nnks` (state at step 3.452e9 / frame 6168 for reuse).
