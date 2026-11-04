# PHASE-5BS — "a flat blue band covers the sky"

**Card t_6894b178.** Question: at the operator's moment (frame 3375) is the
blue strip across rows ~19..100 a mis-decoded clear colour, a mis-textured
polygon, or the scene's real background?

## Answer (one paragraph)

It is **neither the clear colour nor a rasteriser defect**: the band is the
game's **blue elevated road / overpass with its dashed lane markings**, drawn
by ordinary opaque textured quads that span the full 256-px width. The
operator's own 256x192 capture of the same moment (an independent encode, not
a re-encode of our frame — see §3) contains the *same* band, and the band's
left/right boundary agrees with ours to **1-2 px on every row 19..56**. What
looks like "a model dragging a smear to the left edge" is geometry that
legitimately crosses the left frustum plane: its vertices are **clipped to
exactly x=0** by the `-w` plane, so the span starts at column 0 because the
polygon really does run off the left edge there. The rasteriser clamps spans
to the polygon's own bbox and to [0,255]; it cannot start a span below x=0
(`sx` is masked to 0..511 and `slope_xval()` clamps to the vertex X range),
and the archive frame is byte-identical to the pre-change build.

## 1. Which primitive paints the band (values)

New env-gated diagnostic `NNK_GX_POLYDBG=<frame>[:<frame>]` (gx_display.c:
per-polygon dump of nv/facing/attr/texparam/wbuf/pixels-written/vertex list,
plus a `wmap` "which polygon last wrote this pixel" row map). At frame 3375
the bottom screen's 3D layer renders **376 polygons / 78 419 pixels**; the
band rows 18..56 are written by these (all `wbuf=0`, opaque `alpha=31`):

```
i=113 nv=4 facing=1 attr=201f0080 tex=312379b2 px=494  v=(256,47)(256,46)(0,18)(0,19)
i=114 nv=4 facing=1 attr=201f0080 tex=312379b2 px=620  v=(256,48)(256,46)(0,18)(0,19)
i=119 nv=4 facing=1 attr=201f0080 tex=312379b2 px=619  v=(256,48)(0,19)(0,20)(256,50)
i=144 nv=4 facing=1 attr=201f0080 tex=31237932 px=1024 v=(0,27)(0,30)(256,64)(256,61)
i=146 nv=4 facing=1 attr=201f0080 tex=312379b2 px=1792 v=(256,65)(256,58)(0,25)(0,30)
```

`wmap` for the left third (writer ids per pixel) shows the band pixels at
x=0 are written by i=113..169 (`z` 9.6-10.0 M, `w=8192`), i.e. **geometry at a
constant depth plane**, not the background: the clear colour is
`clear_attr1=001f0000` -> `r=(ca<<1)&0x3E=0`, `g=(ca>>4)&0x3E=0`,
`b=(ca>>9)&0x3E=0` -> **black**, and every band pixel is a non-black 6-bit
colour (e.g. `3c8ecf`, `4d96d7`). So the "clear colour decoded wrongly"
hypothesis is dead: 0 % of the band is clear colour.

The mauve field the operator measured at x=0..8, rows 40..46
(`866d86/a696a6/ae9eae/967d96`) is written by i=293
(`attr=001f00c0 tex=15bf1d50`, `v=(0,94)(40,94)(40,30)(0,30)`), i.e. the
40x64 textured **side wall tile** left of the KITCHEN front — a legitimately
shadowed brick wall, not a stretched model.

## 2. Why the spans reach x=0 (the operator's four points)

Measured over the 376 rendered polygons of frame 3375:

| measurement | value |
|---|---|
| polygons with a vertex at `sx==0` | 120, writing 32 964 px (42.0 %) |
| ... of those, with **exactly two** vertices at x=0 | 120 (all of them) |
| polygons with a vertex at `sx>255` | 47 (writing 25 575 px) |
| overall `sx` range in screen space | 0..256 |

Exactly-two-vertices-at-x=0 is the signature of **quad clipped by the `x=-w`
plane** (clip_segment() writes `clip[0] = plane*clip[3]` for the new verts,
and `fill_ras_vertex()` then computes `posX = clip[0]+w = 0`). The DS really
draws those slivers, and melonDS's `RenderPolygonScanline` does the same: the
span is the intersection of the polygon with the screen, so a wall that
crosses the left plane fills from column 0.

Code check of the four suggested causes, in the order given:

1. **Span clamping** — `render_polygon_scanline()` starts `x = xstart`,
   `interp_setup(xstart, xend+1)`, then `if (x < 0) x = 0` (identical to
   GPU3D_Soft.cpp:1000-1010); each of the three phases limits with
   `xlimit = min(xstart+l_edgelen | xend-r_edgelen+1 | xend+1, 256)`.
   `xstart/xend` come from `slope_setup()->slope_xval()`, which clamps to
   `[xmin,xmax]` built from the two vertex `sx` values, and `sx` is
   `posX & 0x1FF` (9-bit, 0..511) — **no span can begin below 0**, so a
   missing clamp cannot be the mechanism. Confirmed empirically: every one of
   the 120 left-reaching polygons has its vertices at x=0 from clipping, and
   none of them writes outside its own bbox.
2. **Edge equations** — `slope_setup()` / `slope_step()` are a line-for-line
   port of GPU3D_Soft.h:230-431 and are already covered by the byte-exact
   `verify_frame` class tests; no negative `XL/XR` was observed for any of
   the 376 polygons.
3. **Swapped / degenerate spans** — `xstart > xend` takes the swapped branch
   (melonDS 1016-1035) and the loops use the swapped `xlimit`s; the zero-store
   polygons at this frame are the same 49 the 5BR card accounted for
   (1-3 px slivers + the depth-rejected backdrop).
4. **w<=0 vertices** — `fill_ras_vertex()` has the `w==0 -> (0,0)` guard and
   the polygon is dropped before that by the `W==0 -> Degenerate` path; after
   the z/y/x `+/-w` clip no vertex can be behind the camera, and the 0..256
   range above shows no wrapped/oversized coordinate.

Depth/attribute path: the band polygons store `z` (9.6-10.0 M) into the depth
buffer along with colour, so it is not a colour-only smear.

## 3. Like-for-like comparison at the same moment

`work/repro_5bs/operator_scene.jpg` is a 256x192 JFIF capture of the same
moment (dialogue, portrait, nameplate identical). It is **not** a re-encode of
our frame: re-encoding our PPM at q40..95 and comparing against it peaks at
**34.5 dB PSNR**, far below the >45 dB a same-source JPEG round trip gives.

| metric (frame 3375 bottom screen) | value |
|---|---|
| mean abs diff, whole frame | 10.50 |
| pixels differing by >40 | 1 297 / 49 152 (**2.6 %**) |
| mean signed diff, 3D area rows 19..100 | +0.17 / -0.04 / +0.07 (no bias) |
| mean abs diff, 2D dialogue box | 1.87 / 2.02 / 4.08 |
| band left/right edge per row 19..56 | **agrees within 1-2 px on every row** |

Band-edge trace (blue = B>110, B-R>40, B-G>25), ours vs the capture:

```
row   ours        ref            row   ours        ref
 19   1..255   0..255            33  24..136   23..134
 21   0..255   0..255            36  68..161   68..167
 26   1..88    1..80             42  91..178   90..177
 30   3..109   3..115            45 113..177  111..177
```

The residual 2.6 % is concentrated on high-contrast 3-D edges and is blue-
channel dominated (mean |d| 8.6/4.9/11.8 = r/g/b), the signature of the
JPEG's 4:2:0 chroma subsampling, plus one 1-row AA edge discrepancy on the
band's top edge (row 23) and diffuse ±10 level texture differences. There is
no region where our frame invents a flat field the capture does not have.

## 4. Verdict

At frame 3375 there is **nothing to remove**: the band is the blue overpass,
present in the operator's own capture with the same boundaries; the left-edge
"constant family" is the shop's shadowed side wall, also present in the
capture. Forcing the left third of rows 26..100 to "the shop/street" would
mean deleting the guest's real polygons — i.e. faking guest behaviour, which
this project forbids. What the 5BR card called "a different camera moment" is
also not the explanation here: the frame content matches the capture, so the
camera for *this* moment agrees.

If the operator still sees a wrong band, the useful artefact is the same
**pair of captures at the frame they consider wrong** (a 256x192 PNG/JPG from
their renderer + the frame number) — the band-edge trace above is the
comparison that settles it per row.

## 5. Gates (this tree, `wt/t_6894b178`)

| gate | result |
|---|---|
| `cargo test --release` (nnktool) | **65 passed, 0 failed** |
| `tools/verify_phase2a.sh` | **1000/1000 melonDS PCs (100.0 %)** |
| savestate 5 M -> 6 M, `NNK_SAVESTATE_STRICT=1` | `fnv=0x8AF95E72 crc=0xEFC35E8F` both runs, identity OK |
| scene at frame 3353 | `polys=379 zero_px=49 pixels=79308 fb3d_nonblack=yes` (330/379 write pixels) |
| frame 3375 captures | **byte-identical** to `work/repro_5bs/ours_f3375.{top,bot}.ppm` (md5 `0f6c5ef0…`, `49ed1c6b…`) -> diagnostics change no behaviour |
| 200 M steps, ROM-only, headless | 9.45 s / 10.18 s (baseline 9.1-9.31 s; best +3.8 %) |
