# PHASE-5BQ — the twelve zero MTX_MULT_4x3 operands: produced by the guest, caused by a 32-bit-truncated DIV unit

**Answer (card t_7732ccd7).** The zero words are produced by the **guest**, and
the runtime gives them to it: the DS math unit's `DIV_RESULT` is 64-bit
(`0x040002A0` low / `0x040002A4` high) and the runtime exposed **only the low
word** (no read path for `0x040002A4` at all, and `div_quot`/`div_rem` were
`uint32_t`). The SDK's 1.19.12 `Vec3` normalise at `0x02002604` divides
`2^56 / |v|^2` and multiplies the SQRT result by the **full 64-bit** quotient,
so every normalised basis vector whose `|v|^2 < 2^24` came out **zero**. The
camera basis builder then wrote twelve zero words into its staging matrix and
the guest submitted `MTX_MULT_4x3` with them.

## 1. Producer of the FIFO words (step 1 of the card)

New diagnostic (`NNK_GX_FIFOPC=<n>`, `NNK_GX_FIFOPC_FRAME=<f>`, plus the
producer ring / `gxd_fifo_prod_dump()`), env-gated, statics only — `Gxd` is
untouched, so the savestate layout is unchanged. Frame 3351:

```
fifopc #1  pc=02036560 word=00001610     <- packed cmd [MTX_MODE][LOAD_4x4]
fifopc #2..18  pc=02009b78  ...          <- 1 + 16 parameter words
fifopc #19 pc=02036560 word=00000019     <- MTX_MULT_4x3
fifopc #20..31 pc=02009b78 word=00000000 <- the twelve operands, all zero
```

Two producer pcs, both inside the SDK's `GX_SendCommand(cmd, src, n)` helper:

```
0x02036554  ldr r1,[pc,#20]  @ 0x04000400
            mov r0,r5 ; lsl r2,r4,#2
0x02036560  str r6,[r1]                  <- packed command word
            bl  0x02009b6c               <- ARM word copy from r5
```

So the words are **not** produced by the FIFO parser and **not** by a DMA path
(no `GXDMA` anywhere in the burst; every word is a 32-bit guest store, width
histogram `w4` only). The copy is faithful: the source buffer really holds the
zeros.

## 2. Where the zeros are written

Scratch store-watch on `0x02071F40..0x02071FD0` (the staging block the sender
reads, a BSS display-list image):

```
pc=020017bc..02001834  writes the twelve words at 0x02071F94..0x02071FC0
```

Those pcs are the epilogue of the 4x3 **basis builder** `0x0200172C`
(three `Vec3` arguments -> a 4x3 matrix at `r3`):

```
0x0200177c  bl 0x02002604      ; normalise(a - c)  -> sp+24
0x02001790  bl 0x02002524      ; cross(b, ·)      -> sp+12
0x0200179c  bl 0x02002604      ; normalise(cross)
0x020017b0  bl 0x02002524      ; cross(·, ·)      -> sp+0
0x020017b4..0x02001834         ; write the nine basis words + 3 translation
```

With the operator's save the three inputs are real and non-degenerate
(`a=(0x1d7000,0x100000,0xe2000)`, `b=(0,0,-0x1000)`, `c=(0x1d7000,0,0xe2000)`),
yet the twelve stored words were `00000000` — melonDS's state at the same
moment holds `ProjMatrix` `1024/1365/-128`, translation `-614400,-521462`, and
its copy of this same staging block differs **only** in these twelve words.

## 3. The instruction/op the runtime gets wrong

`0x02002604` (normalise) is a fixed-point reciprocal built on the DS math unit:

```
0x0200262c  str  r3(=2), [r5]        ; DIVCNT = 2  (64/64)
0x02002630  str  0,      [r5+0x10]   ; DIV_NUMER_L     = 0
0x02002634  str  0x1000000,[r5+0x14] ; DIV_NUMER_H     = 2^24  (num = 2^56)
0x0200263c  str  r6,     [r5+0x18]   ; DIV_DENOM_L     = |v|^2 low
0x02002644  str  r7,     [r5+0x1c]   ; DIV_DENOM_H
...
0x020026a0  ldr  fp, [r7,#4]         ; fp = 0x040002A4 == DIV_RESULT **high**
0x020026ac  mla  r2, fp, ip, r2      ; 64-bit quotient * SQRT result
```

`0x040002A4` is `DivQuotient[1]` in melonDS (`NDS.cpp:3117-3120, 3339-3342`).
The runtime's `io_read32` had cases for `0x040002A0`/`0x040002A8` only, and
`io_div_do()` stored `(uint32_t)q` — so for `|v|^2 < 2^24` (quotient >= 2^32)
the high word read as 0 and the whole reciprocal was lost.

Measured on the second basis vector (cross of `b` with the first), in the
patched build's predecessor: at `0x02002690` the guest had
`quot_low=0x00000000`, `sqrt=0x2000`, `in[0]=0x1000`; the correct quotient is
`2^56/2^24 = 2^32` = high `1`, low `0`, so every product became 0 and the
component came out `0` instead of `0x1000`. The cross product itself is right
(`smull`/`smlal` emitted correctly: `0x02002538` computes
`(int64_t)r[14]*r[6]`); the zeros appear exactly where the 64-bit quotient is
consumed.

## 4. Fix

`nnktool/src/recompile.rs` (canonical) and
`tools/p5bq_runtime_splice.py` (frozen image, registered in
`tools/build_split.sh` as `5bq_div64`):

* keep the full 64-bit signed quotient/remainder (`div_quot_hi`,
  `div_rem_hi`), expose `0x040002A4` / `0x040002AC`;
* `DIVCNT` mode 3 behaves like mode 1 (64/32), as `NDS::DivDone` does;
* divide-by-zero answers quotient `1/-1` by numerator sign, remainder =
  numerator, bit14 set (melonDS `DivDone`);
* plus the Phase-5BQ producer-pc tag `gxd_note_pc(last_pc)` on the guest
  store path (statics only, no `Gxd` change).

## 5. What the fix changes (frame 3351, operator save)

```
before: clipdbg proj: 0 0 0 0  0 0 0 0  0 0 0 0  -131072 -131072 0 131072
        submitted=424853 drawn=0   clip=391928 degen=32925 cullf=0
after : clipdbg proj: 1024 0 0 0  0 1365 0 0  0 0 -128 0  -613376 -521462 0 131072
        submitted=426228 drawn=379 clip=392457 degen=32926 cullf=466
        v0=(-220160,-259382,130944) w=131072, per-object pos matrices real
```

The projection now matches melonDS's `ProjMatrix` (`1024/1365/-128`,
translation `-521462`; `x` differs by `1024` = 1/4 unit, consistent with the
camera advancing one frame between the save moment and our frame 3351 — not
proven yet). `drawn` 0 -> 379 and vertex data is real geometry.

**The 3D colour buffer is still uniformly black** (`NNK_GX_FB3D`, frame 3351),
so a second defect remains between "polygons drawn" and "pixels written": the
composed bottom screen is still black where melonDS shows the street scene and
the portrait. Candidates not yet measured: retained-set render ordering /
`pending` vs `current` at the swap, rasteriser y-sort + `poly->attr` blend in
this frame, or a viewport/depth-test parameter in the latched env
(`disp3dcnt=0x3b`, `view=0,0,256,192`). The next task should start from
`NNK_GX_FB3D` at frame 3351 plus a per-polygon "drawn but covered 0 pixels"
counter.

## Diagnostic surface left in the tree

| Variable | Effect |
|---|---|
| `NNK_GX_FIFOPC=n` / `NNK_GX_FIFOPC_FRAME=f` | print each GXFIFO word with its producing guest pc |
| `gxd_fifo_prod_dump()` | dump the last 64 (pc, word) producer entries |

Both are env-gated statics in `gx_display.c`; nothing in `Gxd` changed, and the
operator state still loads (`NNK_SAVESTATE_IGNORE_BUILD=1`, frame-3350
counters reproduce 424853/0 before the fix).
