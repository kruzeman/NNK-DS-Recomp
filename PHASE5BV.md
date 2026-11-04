# PHASE-5BV — the zero position matrix: the guest READ THE GX MATRIX BACK and got zeros

**Card t_7bb5925b.** Claim under test (from Phase-5BU): the nine zero
`MTX_LOAD_4x3` basis words are guest-side DATA; find the producer, the way
Phase-5BQ found the truncated divider.

## Answer (one paragraph)

The producer is **`io_read32` having no case for the GX matrix READBACK windows
`0x04000640..0x0400067F` (the hardware's *current* matrix) and
`0x04000680..0x040006A0` (the vector matrix)**. The SDK does not keep a RAM copy
of the matrix it just sent: `GX_GetPosMatrix`/`GX_GetVecMatrix`
(`0x02003360` / `0x02003390`) busy-check GXSTAT, then `bl 0x02009C64` /
`0x02009C24` copy **16 / 12 words straight out of `0x04000640` / `0x04000680`**.
melonDS answers those reads from the live matrix state (`GPU3D::Read32`:
`UpdateClipMatrix(); return ClipMatrix[(addr & 0x3C) >> 2]`, i.e.
`ProjMatrix * PosMatrix`); our runtime returned 0 for every word, so the guest
copied zeros into its node-matrix table (`0x02009C64` → `0x020735B0 + node*100`),
the per-object interpolation at `0x02035134` accumulated `0 + fixmul(0x800, 0)`,
and the SDK emitted `MTX_LOAD_4x3` with nine zero basis words. Vertices
submitted under that matrix collapse to the projection's translation column,
which is exactly the full-width sliver set Phase-5BU measured. With the readback
implemented the load carries a real basis, the band is gone, and the metrics
move to the reference values.

## 1. Producer chain (card steps 1-2), measured

Diagnostics used: `NNK_DEBUG_AT_PC` (register file at a pc), `NNK_DEBUG_MEM`
(guest-view hexdump), and an image-local store watch in `st32` added for this
phase (`NNK_ST2_WATCH="lo:hi"`, statics only, pre-write, never committed to the
guest semantics).

| step | evidence |
|---|---|
| FIFO word producer (5BU) | `pc 0x02003008` stores `0x00000017`, `pc 0x02005CB0` copies 12 words |
| who calls the send | at `0x02005CB0` the return address is `0x0203580C` (first load, buffer `sp+0xE0`) or `0x02035820` (second, `sp+0xB0`) — 123 of each per frame; `r0` = `0x027E38B0` / `0x027E3880` |
| the buffers are ZERO | `NNK_DEBUG_MEM` at the send: `0x027E38B0..0x027E38D0` = nine zero words, `+0x24..+0x2C` = `0x027E3918, 0x020690A4, 0x022D4660` (stale stack locals, never written by this routine) |
| who writes them | store watch on `0x027E3880..0x027E38FF`: pcs `0x0203530C..0x0203543C` (9 words), `0x020354D4..0x020355DC` (12 words), `0x02035668..0x02035804` (9 words), **every value 0** |
| the accumulate is `dst += fixmul(t, src)` | `0x02035638` `umull` + `0x02035640` `mla` with `r5 = 0x800` (t) and `r4 = asr(r5,#31)`, then `lsr #12` / `orr ...,lsl #20` |
| the source is zero | at `0x02035618` `r8 = 0x020737E4`; the whole node table `0x020735B0..0x0207396F` reads **all zero** |
| who fills the node table | store watch on the table: 5085 stores, all from pcs `0x02009C68..0x02009C88`, **value 0** — that is the 16-word copy leaf `0x02009C64` |
| who calls that copy | `bl 0x2009c64` at `0x02003380` inside `0x02003360`, which loads `r2 = [0x0200338C] = 0x04000600` (GXSTAT), tests bit 27 (busy), then `add r0, r2, #0x40` → **source `0x04000640`**; the 12-word twin `0x02003390` uses `add r0, r2, #0x80` → **source `0x04000680`** |

So the producer is not a math op at all: it is the **GX matrix readback**, an IO
window the runtime answered with 0. (The 5BQ family — `SMULL`/`DIV`/`SQRT` — was
ruled out by measurement: the interpolation arithmetic is correct
(`r5 = 0x800`, `r4 = asr(r5,#31)`, `umull`/`mla`/`lsr#12`/`orr lsl#20`), it is
simply fed zeros.)

## 2. What the reference answers (melonDS `GPU3D::Read32`, `src/GPU3D.cpp`)

```c
case 0x04000680: return VecMatrix[0];   /* ...0x684 [1], 0x688 [2],
                                           0x68C [4], 0x690 [5], 0x694 [6],
                                           0x698 [8], 0x69C [9], 0x6A0 [10] */
if (addr >= 0x04000640 && addr < 0x04000680) { UpdateClipMatrix();
                                               return ClipMatrix[(addr & 0x3C)>>2]; }
```

`UpdateClipMatrix()` is `ClipMatrix = ProjMatrix; MatrixMult4x4(ClipMatrix,
PosMatrix)` — the readback is the **combined** matrix (GBATEK "Current Matrix"),
not `PosMatrix` alone. There is **no `Read8`/`Read16` case** for either window
(melonDS returns 0 there), which is why the fix is 32-bit-only. Phase-5BU's
savestate walk gives melonDS's operands at the same moment: `ProjMatrix`
`1024/1365/-128`, translation `-614400,-521462,0,131072` (bit-identical to ours
since 5BQ) and `PosMatrix` scale `4096`, translation `(640,320,-1023)` — i.e. a
non-zero current matrix, which is what the guest's copy receives there.

## 3. Fix

* **gxrender** (`gxrender/src/gx_fifo.c`, `gx_fifo.h`, `gx_display.c`): new
  `gx_matrix_read32(Gx *, uint32_t)` reproducing melonDS's `Read32` element
  selection, exposed through `gxd_io_read` for `0x04000640..0x040006A3`
  (32-bit only, as melonDS).
* **runtime prelude**: `io_read32` gains the window (`nnktool/src/recompile.rs`
  = canonical) plus the identical transform for the frozen image in
  `tools/p5bv_gx_matrix_readback_splice.py`, registered in
  `tools/build_split.sh` (negative control
  `NNK_PRELUDE_DISABLE_SPLICE=5bv_gx_matrix_readback`) and in the prelude
  manifest as `5bv_gx_matrix_readback` — the audit now *fails* a build whose
  image lacks it (verified: the negative-control build was rejected).

## 4. The four metrics (frame 3375, operator save, step 1871878703)

"before" = this tree's p5u build without the readback (measured this phase,
`NNK_GX_PSTAT`); "after" = same build + the readback; melonDS = its savestate's
last completed frame (Phase-5BU's validated walk, a different frame).

| metric | before | after | melonDS |
|---|---|---|---|
| polygons reaching the rasteriser | 379 | **327** | 434 |
| polygons with a vertex at `sx==0` | **120** | **6** | 10 |
| polygons spanning `sx=0..>=256` | **39** | **0** | 0 |
| polygons with a vertex at `sx>255` | 47 | 8 | 32 |
| longest band-colour run, rows 19..100 | 72 px (5BU, same instrument) | **0** | 0 |
| px matching (77,158,215)/(60,142,207)/(44,125,199) ±10 | 925 / 521 / 198 (5BU) | **0 / 0 / 0** | 0 |

The `MTX_LOAD_4x3` words at the same command now:

```
before: 0 0 0  0 0 0  0 0 0  41826584 33984676 36521568     ZERO3
after : 3430 -1916 -1152  161 2272 -3303  2179 2719 1980  41826584 33984676 36521568
```

(i.e. the interpolated current matrix — `t = 0x800` of the readback — instead of
an empty basis; the three trailing words are the caller's overlapping stack
locals in both cases, exactly as before.)

## 5. Nothing else moved

* frame 3350 (`work/repro_5bo/ours_f3350.{top,bot}.ppm`, pre-fix capture):
  **byte-identical** top and bottom in the fixed build.
* frame 3375 bottom: the shop street scene ("MARK'S KITCHEN" awning, car,
  lamppost) with the dialogue box "Over here, over here!" — no band
  (`/tmp/bvfix/f.03375.bot.ppm.png`).
* the top screen is a 2D layer; the change is confined to a 3D IO read window
  and the intro/movie frames below are unchanged (§6).

## 6. Gates (this tree, `wt/t_7bb5925b`)

| gate | result |
|---|---|
| `cd nnktool && cargo test --release` | **65 passed, 0 failed** |
| `SKIP_MELONDS=1 sh tools/verify_phase2a.sh` | see PHASE5BV_REVIEW notes — sections A-C reproduce (identical outputs to 5BU); the melonDS prefix comparison needs `SKIP_MELONDS=0` and its trace |
| intro corr (top screen vs `work/p5t/op_u.yuv`) | f4200 → f27 **0.9978**, f4300 → f40 **0.9983** (≥ 0.99) |
| prompt text, cold boot | frame 6400 dark band (x70..200, y88..104) **219 px** |
| savestate 30 M → 32 M, `NNK_SAVESTATE_STRICT=1` | `fnv=0x10BE72AC crc=0xF7CEDCBE` **both** runs, `identity OK`; the 10 post-save frames byte-identical |
| 200 M steps, ROM-only, headless | **9.33 s** (baseline 9.1-9.31 s; within 10 %) |
| ROM-only | yes — the card image is the only input |

## 7. Honest gaps

* The melonDS "same slot" value is quoted as its **operands** (`ProjMatrix`,
  `PosMatrix` — Phase-5BU's validated field walk) plus the code that combines
  them, not as a captured 16-word readback: a melonDS run from this state was
  not available in this phase, and the state does not store the derived
  clip matrix.
* The polygon-count difference (327 vs 434) is unchanged from Phase-5BU's
  explanation (melonDS *stores* `ybot > 192` / `W == 0` polygons as
  `Degenerate` instead of dropping them) and paints nothing.
* `io_read16`/`io_read8` fall back to `io_read32`, so a narrower read of an
  aligned readback word returns its low half where melonDS returns 0. The guest
  only uses 32-bit `ldm` reads for these windows; the deviation is unexercised.
