# PHASE5BP — the 3D pipeline is black: every polygon is dropped in clip space

**Measured** (worktree `wt/t_1aa4e6b8`, savestate `work/repro_5bo/operator_save.nnks`,
frame 3350/3351, `NNK_GX=headless`):

```
gx: fb3d frame=3350 saw_swap=0 polys current=0 pending=0 submitted=424853 drawn=0 verts=938695
    dropped(clip=391928 degen=32925 cullf=0 cullb=0) unknown_cmds=57057
    disp3dcnt=0000003b clear=001f0000/00007fff ...
```

Reproduced byte-for-byte (same numbers) before any change.

## 1. One polygon, followed end to end

`NNK_GX_CLIPDBG=1` on the first primitive of the frame-3351 burst (the burst is
24 427 FIFO words, 1 375 polygons):

```
clipdbg proj: 0 0 0 0  0 0 0 0  0 0 0 0  -131072 -131072 0 131072
clipdbg pos : 65536 0 0 0  0 65536 0 0  0 0 65536 0  1572864 786432 -4190208 4096
clipdbg vtx : (-131072,-131072,0|131072,  ... ) × 3 identical vertices
```

`proj` (the latched projection matrix) has an **all-zero 3×3**: the transform
`v->clip = proj·pos·v` therefore reduces to the projection's translation column,
so every vertex of every polygon lands on the *same* clip point
`(-131072, -131072, 0, 131072)` = `(-1w, -1w, 0, w)`. The triangle has zero area,
its vertices are identical, and `cullf=cullb=0` (a collapsed polygon has a zero
facing dot). `clip_polygon()` then returns < 3 for 92 % of them and the rest die
in the zero-dot/degenerate test — exactly the 391 928 / 32 925 split measured.
Nothing reaches `fill_ras_vertex()`, so `drawn = 0` and the buffer keeps the
clear colour.

## 2. What zeroed the projection

`NNK_GX_WORDS=<n>` (raw FIFO words) + an independent Python decoder that mirrors
melonDS `GPU3D::WriteToGXFIFO`/`CmdNumParams` (same result as the C decoder):

```
w0      00001610   cmd 0x10 MTX_MODE        param w1 = 0     -> projection mode
w2..w17            cmd 0x16 LOAD_4x4        16 params = 1024,0,0,0 0,1365,0,0 0,0,262016,0 -131072,-131072,0,131072
w18     00000019   cmd 0x19 MTX_MULT_4x3    in mode 0 (PROJECTION)
w19..w30           12 params = 0 0 0 0 0 0 0 0 0 0 0 0        <-- all zero
w31/w32 00000010/00000002   MTX_MODE = 2
```

`MatrixMult4x3(proj, 0)` zeroes `proj[0..11]` and keeps `proj[12..15]` — the exact
`proj` printed above. The pattern repeats **once per object**: over 9 000 words,
every object emits `MTX_MODE 0 → LOAD_4x4(proj) → MULT_4x3(0) → MTX_MODE 2 →
pos-matrix setup`; the frame contains 41 of them. So the whole run draws nothing
for one reason and it is this operand.

## 3. The clear colour is *not* the problem

`clear_attr1 = 0x001F0000` is RGB(0,0,0) with alpha 31 and `clear_attr2 = 0x7FFF`.
`render_3d_sw()` does call `clear_frame()`; the dumped buffer is exactly that
colour. A missing clear would leave *stale* pixels, not a uniform black — so the
black buffer is correct behaviour for "no polygon reached the rasteriser".

## 4. `saw_swap=0` is a reporting artefact, not a defect

`latch_unit()` (gx_display.c:1727) `memset`s the tracker at the top of every
frame, and `dbg_fb3d()` runs later in the same `gxd_frame()`; `tracker.saw_swap`
therefore reads 0 in every dump. The latch itself works: `clear=001f0000`,
`disp3dcnt=0x3b`, `view=0,0,256,192` are all non-zero latched state, and the
frame's polygons do reach the retained list — so SWAP_BUFFERS is being seen. The
`NNK_GX_GXLOG` per-frame counters show one `swaps=1` at frame 3350.

## 5. Is the gxrender path faithful?

Checked line by line against melonDS 1.1 (`/home/hermes/tools/melonDS`):
`WriteToGXFIFO` + `CmdNumParams` (identical, incl. the packed-word rule),
`MatrixLoad/Mult4x4/4x3/3x3/Scale/Translate` (identical, `m = s*m`),
`UpdateClipMatrix` (`proj` copied then `MatrixMult4x4(·, pos)`), `SubmitVertex`,
`ClipAgainstPlane`/`ClipPolygon`, `SubmitPolygon` (cull dot, W truncation, zero-dot
test, bottom bound). No skew found. Executing the **same** word stream through
melonDS's algorithm gives the same collapse.

## 6. The melonDS-side value for the same moment

`tools`-style parse of the `GP3D` section of `work/repro_5bo/melonds_oliver.mln`
(layout validated against our own run: `DispCnt 0x3b`, `ClearAttr1 0x001f0000`,
`ClearAttr2 0x7fff`, `FogColor 0x001f7fff` all match):

```
ProjMatrix  1024     0     0     0
               0  1365     0     0
               0     0  -128     0
         -614400 -521462    0  131072
PosMatrix   4096     0     0     0
               0  4096     0     0
               0     0  4096     0
         2621440 1310720 -4190208 4096
```

The real game's projection has a **non-zero 3×3** (`z` scale −128) and a
translation of (−150, −127.3) instead of our untouched load (−32, −32, z 63.97).
That is exactly what a *camera/view* 4×3 multiply would produce — i.e. at the
operator's moment the 12 operand words are a real matrix, not zeros.

## 7. Measurement that isolates it

`NNK_GX_SKIPZEROMULT=1` (diagnostic only) suppresses those 41 zero multiplies so
the projection keeps the loaded value: `drawn=0` still, `clip=393280`. Expected:
without the camera matrix the object translations (pos scale 16, translate
384,192) map to `x_clip = 2w`, so the geometry is still outside the frustum. The
whole chain is consistent with a single missing 12-word camera matrix; no second
defect is visible behind it.

## 8. Where the words come from

The words are 32-bit stores to 0x04000400 (width histogram: `w4=24427`, no 8/16-bit
writes — so this is not the "melonDS drops sub-word FIFO writes" trap). In the
operator savestate, the same projection values exist in a display-list buffer at
file offset 0x5b1e80 whose packed command word is `0x17101610` =
`[MTX_MODE][LOAD_4x4][MTX_MODE][LOAD_4x3]`, followed by `2` then the same twelve
zero words. In the game's own template those zeros are a **position** load after
`MTX_MODE 2` (harmless — the position matrix is rebuilt immediately after by
`MULT_4x3 identity / SCALE / TRANS`); in the FIFO stream our run records them as a
**projection** `MULT_4x3` with the packed word truncated to `0x00001610`. So the
divergence is in the guest-side command list the game hands to the GX FIFO, not in
the fifo decoder or the geometry pipeline.

## Remaining work

Tag FIFO writes with the producing guest PC (`io_write32` currently drops the PC)
and disassemble that writer to find why the packed word loses its upper half /
why the 12 camera words are zero. The renderer needs no change for this defect.

## Diagnostics left in the tree (all env-gated, no behaviour change when unset)

| Variable | Effect |
|---|---|
| `NNK_GX_WORDS=n` | print the first n raw GX FIFO words (`word #i xxxxxxxx`) |
| `NNK_GX_CLIPDBG=1` | per-polygon clip dump + latched `proj`/`pos`/clip vertices at the first polygon + FIFO ring |
| `NNK_GX_CMDBG=n` | print the first n matrix/setup commands with all parameters |
| `NNK_GX_SKIPZEROMULT=1` | diagnostic: suppress all-zero `MTX_MULT_4x3` into the projection slot |
| `gx: unknown-cmd histogram` / `fifo-write widths` | printed with the fb3d dump |

None of these touch `Gx`/`Gxd`, so the savestate layout fingerprint is unchanged
(verified: the operator state still loads with `NNK_SAVESTATE_IGNORE_BUILD=1`,
and the frame-3350 counters reproduce the pre-change numbers exactly).
