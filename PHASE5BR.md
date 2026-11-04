# PHASE-5BR — "379 polygons drawn but the 3D colour buffer is still black"

**Card t_f3d5ee9a; follows Phase-5BQ (t_7732ccd7, commit 75cd100).**

## Answer

There is **no drawn-but-invisible polygon** behind the black frame. The 379
polygons the 5BQ build reports at frame 3351 are *submitted* into the GX
retained set, but the guest has **not yet issued `SWAP_BUFFERS` (0x50)** at
that frame, and melonDS's `GPU3D::VBlank` renders only a **flushed** list
(`src/GPU3D.cpp:2756-2835`: `if (FlushRequest) { ... RenderNumPolygons =
NumPolygons; }`, followed by the bank flip that clears the list). Frames 3351
and 3352 are therefore pre-flush frames and legitimately show the *previous*
retained set — which is empty in this run, because the operator's savestate was
written by a **pre-5BQ build** that dropped every polygon (the zero projection),
so its serialised `current`/`pending` lists are empty.

At the first presentation after the guest's flush (frame **3353**) all 379
polygons are rendered in one pass: **79 308 colour-buffer stores**, 330 of 379
polygons wrote pixels, 49 wrote none (fully occluded/degenerate singletons),
and the 3D colour buffer is **100 % non-black** — the Morton Street shop scene.

## 1. Per-polygon accounting (card item 1)

New diagnostic, env-gated, statics only (no `Gxd`/savestate change):

* `gx_raster.c/h`: `gx_ras_pixels_total` (cumulative colour stores) and
  `gx_ras_last_pixels` (stores made by the last `gx_raster_polygon()`), set at
  all four store sites (3 span phases + `plot_translucent`).
* `gx_display.c`: `NNK_GX_RASTDBG=1` prints, for every polygon the rasteriser
  reports 0 stores for, `<frame> 3dslot=<i>/<n> submitted to fill_ras_vertex
  but wrote 0 pixels: attr=... nv=... wbuf=... facing=...` with the
  viewport-mapped coordinates, depth, W and FinalColor of the first vertices,
  plus a per-frame `polys= zero_px= pixels=` summary.

Measured on `work/repro_5bo/operator_save.nnks` (fixed 5BQ image + this
diagnostic; the 3D PPM is **byte-identical** to the unmodified 5BQ build, so
the diagnostic changes no behaviour):

```
f=3351  polys=0   zero_px=0   pixels=0      fb3d_nonblack=no    (pending=379, current=0)
f=3352  polys=0   zero_px=0   pixels=0      fb3d_nonblack=no    (pending=379, current=0)
f=3353  polys=379 zero_px=49  pixels=79308  fb3d_nonblack=yes   (pending=0,   current=379)
```

The first zero-store polygons are all explained by the picture, not by a
raster bug: `3dslot=43` is the 65x31 backdrop quad at z=8388096 (the farthest
surface, depth-rejected by every nearer polygon), `3dslot=34/36/58/59/60` are
1-3 px slivers.

## 2. Retained-set / flush ordering (card item 2)

`NNK_GX_GXLOG=3350:3356` on the fixed build (`dropclip/dropdegen/cullf/cullb`
columns added by this card):

```
f=3350 words=0     swaps=1 pending=0   current=0   (flush with an empty list)
f=3351 words=23753 swaps=0 pending=379 current=0   polys=1375 drawn=379
f=3352 words=0     swaps=0 pending=379 current=0
f=3353 words=0     swaps=1 pending=0   current=379  <- latch: render 379
f=3354 words=23753 swaps=0 pending=378 current=379  polys=1375 drawn=378 dropclip=529 dropdegen=2 cullf=466
f=3355 words=0     swaps=0 pending=378 current=379
f=3356 words=0     swaps=1 pending=0   current=378
```

Per cycle the guest submits 1375 polygons and 379 survive:
`drawn 378 + cull-front 466 + clip 529 + degenerate 2 = 1375`. The retained
set is rendered **exactly once per flush**, against the env latched at that
flush (`disp3dcnt=0x3b`, viewport `0,0,256,192`, `wbuf=0`,
`latched_sort = last_swap_param & 1`), and the rendered buffer is retained and
re-rendered for the following pre-flush frames, exactly like melonDS's
`RenderFrameIdentical` path.

`gx_display.c`'s swap already implements GPU3D::VBlank; the only deviation
noticed is cosmetic for this scene: a flush with an empty list keeps the old
`current` (`if (g->pending.n > 0)`) whereas melonDS would set
`RenderNumPolygons = 0` (empty screen). Not touched here (it would flash an
empty frame at the 3350-style flushes; a separate card should decide it).

## 3. Attribute / alpha / depth (card item 3)

The 379 polygons carry `POLYGON_ATTR` `001f00c0`, `1d1f0080`, `1e1f0080`,
`201f0080`, `341f0080`, ... : alpha `(attr>>16)&0x1F` = **31 → opaque**, no
translucent path, no wireframe; bits 6/7 set → both faces kept (no culling).
`DISP3DCNT=0x3b` → texture on, highlight (not toon), alpha-blend on, AA on,
edge marking on, fog off. Depth is the Z path (`wbuf=0`), clear depth
`0x7fff*0x200+0x1ff`; polygons are depth-tested against it and the nearest
survive (backdrop `3dslot=43` is the one visibly rejected). 100 % of the
composed 3D area is covered, so depth/alpha are not suppressing the scene.

## 4. Independent check: the rendered state persists

Save the scene state after a flush
(`NNK_SAVESTATE_SAVE_AT=1878000000`, frame 3359, `current=376`), then load it:
on the **first** frame after the load (f=3359) the runtime renders
`current=376`, writes **78 107** pixels and the dumped buffer is non-black.
So once the retained set is non-empty, every frame has a picture; the black
frame was the empty restored set plus the guest's two-frame flush latency.

## 5. What still differs from the reference screenshot

`work/repro_5bo/melonds_071932.png` and our frame 3353 are **not the same
camera moment**: the melonDS savestate `melonds_oliver.mln` carries
`PosMatrix = diag(4096) / (2621440, 1310720, -4190208)` (1.0, translate
640,320,-1023) while the live guest state at frame 3351 is
`diag(65536) / (1572864, 786432, -4190208)` (16.0, translate 384,192,-1023),
written by the mode-2 `MTX_SCALE/TRANSLATE` burst at cmdbg #15-#18 (the same
commands write `PosMatrix` in melonDS, `GPU3D.cpp:2455-2510`). The scene is
consequently framed/zoomed differently; the 2D UI (dialogue box, "Mark"
nameplate, portrait, text) matches the reference at 84-92 % of pixels within
12 levels, the 3D area only at 30 % (best global shift (-2,-2): mean |d| 44.8).
The 3D layer is correct in *content* (yellow tiled pavement, red KITCHEN
shopfront with blue/white awning, grey road, car, portrait) but the reference
pixel comparison is not a like-for-like frame.

## Repro

```
NNK_SAVESTATE=work/repro_5bo/operator_save.nnks NNK_SAVESTATE_IGNORE_BUILD=1 \
NNK_STEP_LIMIT=1890000000 NNK_GX=headless NNK_GX_RASTDBG=1 NNK_GX_GXLOG=3350:3356 \
NNK_GX_FB3D=/tmp/fb3353.ppm NNK_GX_FB3D_FRAME=3353 NNK_GX_SHOT=/tmp/shot \
./out/p5br/nnk_runner "Ni No Kuni.nds"
```

## Numbers

* submitted polygons at frame 3351: 426 228 cumulative (1 375 in that frame),
  `drawn=379`; clip 392 457, degenerate 32 926, cull-front 466, cull-back 0.
* per flushed frame: submitted 1 375, drawn 378, cull-front 466, clipped 529,
  degenerate 2, colour stores 79 308, polygons with 0 stores 49/379.
* 3D buffer at 3353: 49 152/49 152 non-black pixels.
