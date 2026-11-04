# PHASE-5BT — the blue band: the clip is homogeneous; the polygon set is not

**Card t_4fe58333.** Claim under test: the 3D layer paints a solid light-blue
band because geometry that leaves the frustum is *screen-clamped* instead of
being clipped in homogeneous space, and implementing homogeneous clipping will
remove it.

## Answer (one paragraph)

The card's **mechanism is refuted and its observation is confirmed**. The
frustum clip in this tree *is* homogeneous-space Sutherland–Hodgman
(`gx_fifo.c:315-413`, a line-for-line port of melonDS `GPU3D.cpp:1124-1256`:
z(+w/−w) → y → x, far-plane reject on polygon-attr bit 12, W truncated to 24
bits, `fill_ras_vertex()` only masks like melonDS — there is no clamp of a
projected coordinate anywhere in the path). Measured at frame 3375, **0 of the
379 polygons that reach the rasteriser have any post-clip vertex outside
|x|>w, |y|>w or z>w**; the band polygons enter with |x| up to 4.7w and leave
with `clip[0] == ±w` *exactly*, i.e. pinned to the plane, and their two
`sx==0` vertices are the projection of the x = −w plane at viewport x0 = 0 —
the expected result of correct clipping, not of clamping (a clamp would flatten
the polygon to zero width at x=0; the observed polygon is a full-width sliver
whose interior edges are properly projected). What the card is right about is
the symptom: the reference has **no** such geometry. melonDS's own GPU state in
the operator savestate contains **0 polygons spanning sx=0..≥256** and only
6 polygons touching sx==0, while ours has **39 full-width slivers and 120
polygons with a vertex at sx==0** — so the divergence is upstream of the clip,
in the vertex set the strips are built from, not in the frustum algorithm.

## 1. Like-for-like image comparison (my own extraction, not the card's numbers)

Panel extraction from `work/repro_5bo/melonds_071932.png` (1126x600 melonDS
1.1 window capture): the two DS panels sit at x 81..560 / 566..1045,
y 141..500 (1.875× of 256x192). The right panel
(`work/p5bt_mel_bot_480.png`) is a complete bottom screen with the same
dialogue state ("Over here, over here!", Mark, same portrait) as our frame.

| metric (bottom screen) | our `ours_f3375.bot.ppm` | melonDS `work/p5bt_mel_bot_256.png` |
|---|---|---|
| mean luma | 162.2 | 167.6 |
| longest blue run, rows 19..100 | **88 px** (rows 39-43) | **7 px** |
| px matching (77,158,215) ±10 | 925 | **0** |
| px matching (60,142,207) ±10 | 641 | **0** |
| px matching (44,125,199) ±10 | 285 | **0** |

The card's 121.7 luma / 87.8 % lit for the reference does not reproduce with
the panel geometry above (I measure 167.6 / 100 %); its *qualitative* claim is
correct: the band and its exact colours exist only in our frame. Byte-exact
scene comparison is impossible — the capture is a 1.875×-scaled window
screenshot — so the per-row band extents are the right instrument, and they say
"reference clean, ours 88-px runs".

## 2. The band's primitives, with clip-space values

`NNK_GX_VD=<frame>` (new in this tree) dumps, per surviving polygon, the raw
clip-space vertices, the post-clip vertices and the final screen vertices, plus
the model-space vertex stream. At frame 3375 (**step 1871878703**, operator
savestate `work/repro_5bo/operator_save.nnks`, `NNK_SAVESTATE_IGNORE_BUILD=1`):

```
gxvd f=3375 attr=201f0080 tex=312379b2 wbuf=0 nv=3->4 wsize=20 vp=0,0,256,192
  raw0=(-614400,177418,32768|w=131072)   <- model vertex: distant, outside
  raw1=( 144693, 54190,20357|w=131072)
  raw2=( 149602, 57147,20479|w=131072)
  clp0=(-131072,101332,24994|w=131072)   <- exactly x = -w
  clp1=(-131072, 98957,24866|w=131072)
  clp2=( 131072, 56401,20579|w=131072)   <- exactly x = +w
  clp3=( 131072, 60064,20777|w=131072)
  scr0=(0,21) scr1=(0,23) scr2=(256,54) scr3=(256,52)
```

- 379 polygons survive clipping; **0** have a post-clip vertex with |x|>w,
  |y|>w or z>w. 139 have at least one *pre-clip* vertex outside the frustum —
  that is exactly the population the clip exists for, and it is handled.
- The whole scene is orthographic in clip terms: `clip.w == 131072` (=32.0) for
  2513 of 2517 vertices of the frame (4 at 4194304). Clipping against x=±w is
  therefore clipping against a *rectangle* in world space, and it is correct.
- The screenshot signature the card reads as "screen clamping" — every
  left-reaching polygon having exactly two vertices at sx==0 — is what
  `clip_segment()` must produce: it writes `clip[comp] = plane*clip[3]`, so
  `fill_ras_vertex()` computes `posX = clip[0]+w = 0` and sx = vp_x0 = 0.

## 3. What the reference renderer actually contains (melonDS GPU3D state)

`work/repro_5bo/melonds_oliver.mln` is melonDS's own savestate of the operator
moment, and its `GP3D` section (offset 0x10DE488, 0x180000 bytes) carries the
complete GPU3D state: matrices, viewport, both polygon-RAM banks with their
`FinalPosition`/`FinalZ`/`Attr`/`TexParam`. Reading it back
(`work/p5bt_mln3.py`, record stride 188 bytes, VertexRAM stride 64 bytes,
bases located by pattern score 60/60 and 200/200):

| quantity | melonDS (last rendered frame, bank 0) | ours (frame 3375) |
|---|---|---|
| polygons | 413 | 379 |
| polygons with a vertex at sx==0 | **6** | **120** |
| polygons with a vertex at sx>255 | 27 | 47 |
| polygons spanning sx=0..≥256 | **0** | **39** |
| screen extent | x 0..256, y 0..192 | x 0..256, y 0..192 |

**ProjMatrix is bit-identical between the two renderers**:
`1024 0 0 0 | 0 1365 0 0 | 0 0 -128 0 | -614400 -521462 0 131072`, and the
viewport is identical (`x0 0, y0 0, w 256, h 192`; melonDS `Viewport[6]` =
0,191,255,0,256,192 with y0 in [3]). So the clip-space *frame* agrees; what
disagrees is the polygon set: melonDS's polygons in the band's rows carry
|clip.x| ≈ 1.7e4..2.4e4 (sx ≈ 104..112), while our band triangles carry
|clip.x| up to 6.1e5 and are therefore clipped into full-width slivers.

## 4. Where the defect therefore is (leading hypothesis, with evidence)

The full-width slivers are triangle-strip polygons (`mode=2`) that mix one vertex
from a completely different place with a local mesh:

```
vtx  clip-space
local (0,1637,6082)     (150904, 48164,20658)   -> sx ~ 275
local (5245,6665,2601)  (153759, 57804,20921)
detached (-614400,177418,32768)                  -> sx 0 after clip
```

That detached vertex's clip coordinates **do not occur anywhere in the frame's
2517-vertex stream** (the per-vertex dump prints every transformed vertex), so
the polygon consumed a `temp_vtx[]` slot that was transformed under a
*different* clip matrix — i.e. a retained strip vertex — while its two
companions were transformed with the current one. melonDS keeps the same
retained `TempVertexBuffer`, but additionally re-uses the *clipped* vertices of
the previous polygon only when `!Vertices[i]->Clipped`, and re-clips from the
raw vertices otherwise (`GPU3D.cpp:1330-1412`); our `submit_polygon()` always
re-clips the raw retained vertices and has no `LastStripPolygon` equivalent, so
a strip that crosses a matrix or polygon boundary keeps a stale clip-space
vertex that melonDS would have replaced. The 39 full-width slivers are exactly
this population, and they are what paints the blue band.

This is a *lead*, not a proven fix: closing it needs a per-polygon comparison of
the PosMatrix against melonDS's at the same model, which the savestate can only
give for the instant it was written (one matrix, scale 4096, translation
(640,320,-1023)).

## 5. Gates (this tree, `wt/t_4fe58333`)

| gate | result |
|---|---|
| `cargo test --release` (nnktool) | **65 passed, 0 failed** |
| `tools/verify_phase2a.sh` | see run log below |
| frame 3375 captures | unchanged by the diagnostics (`NNK_GX_VD` is env-gated) |

## 6. Artifacts (gitignored `work/`, not committed)

```
work/p5bt_mel_bot_480.png / _256.png   extracted melonDS bottom screen
work/p5bt_sbs.png, p5bt_diff.png       ours | reference side by side + diff
work/p5bt_ours_top3x.png, p5bt_mel_top3x.png   rows 0..130 at 3x
work/p5bt/vd3375d.log                  full clip/vertex dump of frame 3375
work/p5bt/mln3.txt                     melonDS polygon list from the savestate
work/p5bt_mln3.py, p5bt_vd.py, p5bt_cmp.py, p5bt_metrics.py   the probes
```
