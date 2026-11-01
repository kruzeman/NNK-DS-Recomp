# Phase-5R — barrier I6 executed: the 0x8A "DC seed" is the codec DC-offset field

Follows Phase-5Q (merged `2e90753`), which localized the corrupted movie frames to
an `0x8A` fill performed by the ITCM intra predictor and named this phase's
measurement: "the first store that puts `0x8A` into the seed row, and the
container field `0x02278500` (native `0x00000280` vs melonDS `0xFFFFDF80`)".

Both measurements were run, in both engines.  The result **promotes** I6 one
level: the `0x8A` is not a neighbour-row seed that self-propagates.  It is the
**codec field `codec+0x1B4`** (`0x02278500`), consumed as a *signed DC offset*
by the 4x4/8x8 reconstruction, and it is what turns the `0x80` flat prediction
into `0x8A` on every block.  The predictor then propagates it and the ring copy
loop carries it to the compositor, which Phase-5P proved faithful.

## 1. Result

| # | measurement | value |
|---|---|---|
| 1 | first byte that is not the `0x80` baseline | `0x02278800..03` = `0x8A`, steps `2256160861..864`, `pc=0x01FFCBDC..0x1FFCBE8` |
| 2 | value it replaced | `0x80` (written at step `2256160777` by the constant intra fill `0x01FFBF10`) |
| 3 | instruction that produces it | `r9 = (int32)[codec+0x1B4]; r9 = (r9+32)>>6; pixel = clamp(pred + r9)` — `0x01FFCB90..0x1FFCBF0` |
| 4 | `codec+0x1B4` in native | `+640` → `r9 = +10` → `0x80+10 = 0x8A` |
| 5 | `codec+0x1B4` in melonDS | `-8320` → `r9 = -130` → `0x80-130 = -50` → table at `[codec+0x44]` clamps to `0` (black) |
| 6 | decoded plane, 256x192 8-bit, at return 1 | native `0x02278800`: `8A:49152` (49152/49152 samples); melonDS same address: `00:49152` |
| 7 | first `0x8A` inside a predicted band row | `0x02278F00`, step `2256160992`, `pc=0x01FFBEF8` (the 4x4 DC handler `0x01FFBEB8`) |
| 8 | 8x8 left-column DC at `0x01FFAE40` | step `2256161083`: `r4=0x8A`, `r6=r7=0x8A8A8A8A`, while its own `above` row `0x02278708` and `block` are still `0` |
| 9 | how it reaches the screen | copy loop `0x01FFCF3C`, step `2256754046`: `r0=0x02278900` (src), `r1=0x0228A820` (ring slot), `r3=0x8A8A8A8A` |

So the corruption is generated **once per block** by a signed field of the
codec state, and everything Phase-5Q saw downstream — the "mean of the row
above", the "self-propagating 0x8A", the plane-sized runs — is propagation of
that one value.  The value is identical to the word Phase-5Q flagged as "a
signed field of the movie container already disagrees (+640 vs -8320)"; this
phase measures **who consumes it, how, and with what result**:

```
0x01FFCB90  add  lr, r0, #436        ; lr = codec + 0x1B4  (0x02278500)
0x01FFCB94  ldr  r9, [lr]            ; signed DC term, whole word
0x01FFCB98  add  r9, r9, #32
0x01FFCB9C  asr  r9, r9, #6          ; +640 -> +10 ; -8320 -> -130
0x01FFCBA0  ldr  lr, [sp, #4]        ; destination (the plane)
0x01FFCBA8  ldr  r8, [r0, #68]       ; clamp/lookup table at codec+0x44
0x01FFCBAC  ldrb r4, [lr]            ; 4 predicted bytes (0x80 after the DC fill)
0x01FFCBBC  add  r4, r4, r9
0x01FFCBCC  ldrb r4, [r8, r4]        ; clamp
0x01FFCBDC  strb r7, [lr, #3]  ... 0x1FFCBE8 strb r4,[lr],#256
```

The 4x4/8x8 DC handlers themselves are complete and consistent (measured, not
assumed): `0x1FFBDF4` (above+left), `0x1FFBE6C` (left), `0x1FFBEB8` (above),
`0x1FFBF04` (constant `0x80`) for 4x4 blocks — sub-dispatch table at
`0x01FFBF28`; `0x1FFACFC`, `0x1FFADE0`, `0x1FFAE84`, `0x1FFAF20` for 8x8 blocks
— table at `0x01FFAF74`.  The index is `(fp&0xFF ? 8:0) + (fp>=0x100 ? 4:0)`,
i.e. which neighbours exist.  Every one of them was observed executing with the
expected operand values; none is mis-decoded.

## 2. Named follow-up barrier (I7)

**I7: the producer of `codec+0x1B4`.**  `0x02278500` is `0` in both engines at
the decode-callback entry and is `+640` / `-8320` when the reconstruction first
reads it (step `2256160861`).  The arithmetic is confirmed by the stored pixels
-- `(640+32)>>6 = +10` gives exactly the observed `0x80+10 = 0x8A`, and
`(-8320+32)>>6 = -130` gives `0x80-130 = -50 → 0xCE`, which the table at
`[codec+0x44]` clamps to `0`, i.e. melonDS's black plane -- so the field really
does hold `0x280` at that read.  The *writer* is still unnamed: neither watch
window caught it,

```
$ NNK_STOREWATCH="0x02278500:0x02278504"     (Phase-5Q window)
W32 0x02278500 old=0x00000000 pc=0x02009BB4 lr=0x0207A9F8 step=2255796630   driver clear
W32 0x02278500 old=0x00000000 pc=0x01FFA54C lr=0x01FFA528 step=2256160792   stm of 4 words (16 B)
W32 0x02278500 old=0x00000000 pc=0x01FFC53C lr=0x01FFC40C step=2256160833   last logged store: r3=0x7F9FC000
$ NNK_STOREWATCH="0x022784F0:0x02278510"     (this phase, wider window)
   ... same 17 lines, still no store carrying 0x280
```

The field therefore changes between step `2256160833` and the read at
`2256160861` through a store whose *start* address is below `0x022784F0` (the
watch filters on the store start, `tools/nnk_smc.inc:302`) or through a path
that does not call the guest-store note.  Next measurement, exactly:

1. `NNK_STOREWATCH="0x02278400:0x02278520"` on the same run recipe — wide
   enough that the 4-word `stm` family and any multi-register store covering
   the field is caught by its start address; names pc/lr/r0-r3 of the store
   that installs `0x00000280`.
2. The same field in melonDS at the same program point (`ts`-gated RAM dump
   just before the first reconstruction) -- melonDS holds `0xFFFFDF80`.
3. Only then the arithmetic that produces +640 vs -8320 can be named and
   fixed; the class will be a *dataflow* class (a table/bit-read word feeding
   the codec), not a decode-form class -- the ITCM form audit is closed
   (Phase-5Q) and every predictor handler this phase measured is correct.

## 3. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5r
# 1. operand probe (registers + the two rows the predictor works on)
python3 tools/p5r_pred_probe.py work/phase3a/arm9_thumb_ovl_itcm_p5o.c \
        work/phase3a/arm9_thumb_ovl_itcm_p5r.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5r.c SPLIT_DIR=$S/split \
  bash tools/build_split.sh $S/p5r1

env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2300000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_PRED_PC=0x01FFAD9C,0x01FFAE40,0x01FFAEDC,0x01FFAF30 NNK_PRED_N=24 \
    NNK_PRED_PATH=$S/nat/pred.log $S/p5r1 work/phase3a/arm9_stored_padded.bin

# 2. who writes the plane origin / the band row / the codec field
env ... NNK_STOREWATCH="0x02278800:0x02278808"  NNK_STOREWATCH_PATH=$S/nat/sw_origin.log ...
env ... NNK_STOREWATCH="0x02278F00:0x02278F08"  NNK_STOREWATCH_PATH=$S/nat/sw_seed.log   ...
env ... NNK_STOREWATCH="0x02278500:0x02278504"  NNK_STOREWATCH_PATH=$S/nat/sw_field.log  ...
env ... NNK_STOREWATCH="0x0228A820:0x0228A828"  NNK_STOREWATCH_PATH=$S/nat/sw_slot.log   ...
```

The disassembly used here is `arm-none-eabi-objdump` on the 32 KiB ITCM blob the
Phase-5Q PC-triggered dump produces (`itcm_cb.bin`, byte-identical between the
engines, 0/32768), sliced at `base = 0x01FF8000`.

Fixed addresses are comparable across the engines: the codec state
(`0x0227834C`), the decode plane (`0x02278800`) and the ring
(`0x02284810`+`0xC010`) are allocated identically by both, as checked by the
codec field itself and by the ring being word-identical at returns 1 and 2.

## 4. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 32 passed; 0 failed
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh
1) strict common prefix        : 1000 / 1000 melonDS PCs (100.0%)
2) window-filtered prefix      : 1000 / 1000 (window 0x02000000..0x02400000)
3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
RESULT: PASS
```

No engine code changed in this phase: it is a measurement phase, and the
measurement moved the barrier from "the predictor's DC seed" (I6, confirmed and
consumed) to "the producer of `codec+0x1B4`" (I7, named above).
