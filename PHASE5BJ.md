# Phase-5BJ — the prompt text: `smulbb r1, r1, r5` at 0x02030784 was decoded as `cmn`

Follows Phase-5BI (master `0261715`).  Branch `wt/t_6269fe84`.

Build (isolated split dir — see §8): `SPLIT_DIR=work/p5bj/split2
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh
work/p5bj/nnk2`

**Result: fixed in the CPU model.**  One instruction was missing from the
decoder: the ARMv5TE DSP halfword multiplies (`SMULxy` / `SMLAxy` / `SMULWx` /
`SMLAWx`).  Their encodings live inside the data-processing space and are told
apart only by bit 7, so `smulbb r1, r1, r5` (**0xE1610581** at **0x02030784**)
decoded as `cmn r1, r1, lsl #11` — a flag-only operation, i.e. a **NO-OP on its
destination**.  The guest's glyph blitter computes its 1-bpp source *row
stride* with exactly that multiply (glyph stride 1 byte/row × 11 rows), so the
stride stayed **1** instead of **11**: each glyph row was read one bit further
into the bitmap instead of eleven, which is precisely the measured signature —
the survivors of a dense bitmap at shifted positions, ~1/3 of the ink in the
two text strips and 0 in most tiles.

## 1. The measured fault, to the operand

`NNK_DEBUG_AT_PC` on the multiply and on the store that consumes it
(savestate `work/p5bj/s3500.nnks`, resumed — a resume is seconds):

```
# 0x02030784  smulbb r1, r1, r5      (stride byte at font header +6) x (11 rows)
dbg step=3551213308 pc=02030784 r1=00000001 r5=0000000B ...       <- before
dbg step=3551213309 pc=02030788 r1=00000001 ...                   <- after: 1, not 11
# the same value goes straight into the blit descriptor
0x027E39CC (par+24 = source row stride in bits) = 0x00000001      <- should be 0x0000000B
```

The blitter (`0x02030278`) reads pixel `(row r, column x)` at source bit
`r*stride + (x-x0)`; with `stride = 1` the read walks *diagonally* through the
bitmap, so each row lands on different, mostly blank bits — the "shifted
positions" Phase-5BI reported.  `NNK_DEBUG_AT_PC=0x0203039C` (the per-pixel
`cmp r0, #0` after the 1-bit reader) shows the diagonal directly: for rows
0,1,2,3 of one call the reader returns bits 1,2,3 / 2,3,4 / 3,4,5 / 4,5,6 of
the source stream instead of the same three columns of four different rows.

## 2. The negative control: the source bitmap is NOT the defect

The card's second branch asked whether the bitmap the rasteriser *reads* is
already sparse.  It is not — it is byte-identical to melonDS's:

```
blit descriptor at 0x027E39B4 (DTCM stack), first call of the text strip:
  +0x00 dest 0x0640071C   +0x04 src 0x02278678   +0x08 x0 5   +0x0C y0 7
  +0x10 w 4   +0x14 h 11  +0x18 stride 1  +0x1C bpp 1  +0x20 ink 4
ours  RAM[0x02278678..0x02278778] at the blit  ==
melon RAM[0x02278678..0x02278778] (state.mln)   ->  256 / 256 bytes equal,
                                                    228 ink bits each
```

So everything upstream (font loader, bitmap) and everything downstream (VRAM
window, tile addressing, OAM, palette) was already faithful; the only wrong
thing was the stride the *caller* computed, and it was wrong because the
multiply that computes it was a no-op.

## 3. Why the decoder missed it

`nnktool/src/decode.rs` has a data-processing catch-all on bits 27-26 = 00.
0xE1610581 has bits 27-26 = 00, bit 25 = 0, bits 24-21 = 1011 (CMN) and bit 20
= 0 (S), so it was accepted as `cmn`.  A register-shifted or immediate-shifted
data-processing instruction never sets bit 7 (bits 7-4 are `0`/`shamt,stype`),
while the DSP multiplies require bit 7 = 1 and bits 27-20 ∈ {0x16, 0x10, 0x12}.
The extra-load/store rule above it requires bits 6-5 ≠ 0, so nothing else
claimed the word.  Encodings were taken from `arm-none-eabi-as -march=armv5te`
(all twelve forms are pinned by the new class test).

## 4. The fix (three places, one rule)

| place | what |
|---|---|
| `nnktool/src/decode.rs` | `MulKind::Smulxy/Smlaxy/Smulwx/Smlawx` + a decode rule **before** the data-processing catch-all: `(w & 0x0FF00090) == 0x01600080` (SMULxy), `== 0x01000080` (SMLAxy), `(w & 0x0FF000B0) == 0x012000A0` (SMULWx), `== 0x01200080` (SMLAWx); bit 5 = Rm half (x), bit 6 = Rs half (y) |
| `nnktool/src/recompile.rs` | the emitter's halfword expressions (signed 16x16, signed 32x16 `>> 16`) plus the Rn addend for the SMLA forms |
| `tools/nnk_smc.inc` | the same rule in the runtime's live decoder (overlay pages and ITCM re-decodes run there) |
| `tools/p5bj_dsp_mul_splice.py` + `tools/build_split.sh` + manifest row | the frozen image `arm9_thumb_ovl_itcm_p5u.c` predates the rule: the splice asks the fixed recompiler for each affected case (`nnktool recompile-cases --max-insns 1`) and grafts it. 21 sites, all `smulbb`/`smlabb`, listed in the script's table and re-checked against the recompiler's own mnemonic before grafting. `NNK_PRELUDE_DISABLE_SPLICE=5bj_dsp_mul` is the negative control |

No faked guest behaviour: the guest's own code computes 11 and the guest's own
blitter then reads the bitmap correctly; the emulator only stopped dropping the
instruction.

## 5. Verification

* **The text tiles are byte-identical to melonDS at frame 6400** (the card's
  acceptance): `NNK_GX_STATE_DUMP` bank B vs `work/phase5ba/melds_gpu/vramB.bin`
  — the 32 text tiles (AOBJ 64-79, 96-111) differ by **0 bytes**, and the whole
  bank differs by **0 bytes** with the same ink total (**29 599** 1-bits both).
  Before the fix the same comparison gave **195** differing bytes in exactly
  those tiles (Phase-5BI's number), e.g. tile 64: 3 vs 7 ink bits *and*
  differing, tile 65: 2 vs 26, tile 66: 0 vs 14, tile 96: 5 vs 17.
* **The sentence renders**: `work/p5bj/fix/f.06400.bot.ppm.png` (frame 6400,
  fixed build) shows the parchment box with **"Skip the cutscene?"** and the
  **Yes / No** buttons legible — see `work/repro_5au/melonds_reference.jpg` for
  the operator's reference.
* `cargo test`: **64 passed** (suite 63 → 64; the new class test pins all
  twelve encodings, the emitted halfword expressions and the live-decoder rule).
* `tools/verify_phase2a.sh`: see §7 (result recorded there).
* Savestate round trips with `NNK_SAVESTATE_STRICT=1`: 5M→6M
  `fnv=0x2EBE7C43 crc=0x06544741`, 30M→32M `fnv=0x0496F54E crc=0xBB28A57C` —
  identical on save and reload.
* No halt: the cold boot reaches the prompt and the 4.4e9-step budget with
  `halt=step budget exhausted`, 26 203 distinct PCs.

## 6. Evidence (under `work/p5bj/`, never committed)

`run_a.log` (cold boot 3.7e9 + `NNK_ITCM_DUMP` + first-hit checkpoint),
`run_c..run_f.log` (savestate resumes: the multiply, the descriptor, the
per-pixel bit reads), `ram_blit2.bin` / `mel_ram.bin` (the source-bitmap
negative control), `state6400.bin` (pre-fix bank B) / `state6400b.bin`
(post-fix bank B), `fix/f.0640*.{bot,top}.ppm(.png)`, `cold/` (the 4.4e9 cold
boot shots), `melds_gpu`-derived art scripts `p5bj_*.py` under `work/`.

## 7. Regressions

Cold boot 0 → 4.4e9 steps with `NNK_GX_SHOT` (`work/p5bj/run_cold.log`, shots
in `work/p5bj/cold/`), same input script as the defect repro:

| check | result |
|---|---|
| intro band, **top** screen vs `work/p5t/op_u.yuv` | frame 4200 vs f027 **corr 0.9977** (baseline 0.9979), frame 4300 vs f040 **0.9984** (baseline 0.9981) — both ≥ 0.99 |
| intro band at frame 4400 | our top screen is already black there (mean luma 1.9 vs 77.6): the movie ends a few frames earlier in this capture than in the baseline run, so the third correlation is not comparable.  The two measurable frames are unchanged, and nothing in this fix touches the movie pipeline (one multiply result register) |
| title/opening artwork, frame 6300 | non-blank: `work/p5bj/cold/f.06300.bot.ppm.png` shows the full-colour "Welcome to The MOTOR CITY HOTROIT!" card.  The card expected the sepia tint; the *picture* is the known bank C/D `stream=2` background card Phase-5BI already recorded as different from melonDS, which this card does not touch (the fix feeds one sprite-strip bitmap) |
| speed | 4.4e9 steps in **483 s** = 9.11 M steps/s; the 600 M ≈ 61 s baseline is 9.84 M/s → **8 % slower**, inside the 10 % budget (shots every 25 frames are part of that cost) |
| halts | `done steps=4400000001 … halt_pc=0x0207CBD0 halt=step budget exhausted` — no new halt, 26 312 distinct PCs |
| `tools/verify_phase2a.sh` | `rc=0`; section F melonDS-vs-native **1000/1000** in all three variants (strict prefix, window-filtered, in-order after resync) — `work/p5bj/verify.log` |
| `cargo test` | **64 passed**, 0 failed |
| savestate round trips (`NNK_SAVESTATE_STRICT=1`) | 5M→6M `fnv=0x2EBE7C43 crc=0x06544741`, 30M→32M `fnv=0x0496F54E crc=0xBB28A57C` — identical on save and reload |
| splice negative control | `NNK_PRELUDE_DISABLE_SPLICE=5bj_dsp_mul` skips the transform (same policy as the 5X/5AE/5AF/5AJ/5AP/5AT/5AW/5BB splices).  The manifest row `5bj_dsp_mul` is `required` and the prelude audit reports **61 required … AUDIT OK** with the splice in place (`work/p5bj/build2.log`); the disabled-build audit was not run in this phase |

`NNK_EXC_TRACE` was sampled on two windows (cold boot to 40 M steps, and a
savestate resume across the prompt) and shows the IRQ/exception returns in
both states (`target=0x02015EC4 spsr=0x0000001F thumb=0`,
`target=0x02007258 spsr=0x8000009F thumb=0`); the Phase-5BB signature
`target=0x02137A92 spsr=0x4000003F thumb=1` was **not** reproduced in those
windows — it is an ov17 IRQ-over-Thumb moment and this fix adds no ARM/Thumb
state carrier, so its code path is unchanged.

## 8. Toolchain note (still true, Phase-5BI §7)

`work/split` is shared between worktrees, so a build while another worker uses
it links *their* fragments.  Every measurement here used a private
`SPLIT_DIR=work/p5bj/split{,2}`.
