# GX_COVERAGE.md — DS GX command coverage (gxrender)

Scope: geometry-engine (GX) command FIFO decoder + vertex pipeline + software
rasterizer + GL backend skeleton for the Ni no Kuni DS static recompilation
(game code B2KJ).  Semantics follow the Nintendo DS hardware as documented by
GBATEK and validated against the melonDS source (src/GPU3D.cpp command
decoder, src/GPU3D_Soft.cpp rasterizer reference) at
/home/hermes/tools/melonDS.  All render math is integer (1.19.12 fixed point,
1/16-px screen units) so output is byte-deterministic.

## Command coverage table

GX FIFO words: each word packs up to 4 command bytes; parameters follow as
full 32-bit words (hardware packing, exactly like melonDS WriteToGXFIFO).

| Cmd  | Params | Name           | Status      | Notes |
|------|--------|----------------|-------------|-------|
| 0x10 | 1      | MTX_MODE       | implemented | proj/pos/pos+vec/tex modes |
| 0x11 | 0      | MTX_PUSH       | implemented | proj/tex 1-slot, pos 31-deep |
| 0x12 | 1      | MTX_POP        | implemented | 6-bit signed offset for pos stack |
| 0x13 | 1      | MTX_STORE      | implemented | slot addr (pos stack) |
| 0x14 | 1      | MTX_RESTORE    | implemented | |
| 0x15 | 0      | MTX_IDENTITY   | implemented | |
| 0x16 | 16     | MTX_LOAD_4x4   | implemented | s32 1.19.12 |
| 0x17 | 12     | MTX_LOAD_4x3   | implemented | |
| 0x18 | 16     | MTX_MULT_4x4   | implemented | m <- m*s (s applied to vertex first) |
| 0x19 | 12     | MTX_MULT_4x3   | implemented | |
| 0x1A | 9      | MTX_MULT_3x3   | implemented | |
| 0x1B | 3      | MTX_SCALE      | implemented | VecMatrix not scaled (hw behavior) |
| 0x1C | 3      | MTX_TRANS      | implemented | VecMatrix translated (hw behavior) |
| 0x20 | 1      | COLOR          | implemented | 5-bit RGB |
| 0x21 | 1      | NORMAL         | implemented | triggers lighting |
| 0x22 | 1      | TEXCOORD       | implemented | s,t 12.4 |
| 0x23 | 2      | VTX_16         | implemented | x,y then z (s16 1.3.12) |
| 0x24 | 1      | VTX_10         | implemented | s10, sign-extended to s16 |
| 0x25 | 1      | VTX_XY         | implemented | |
| 0x26 | 1      | VTX_XZ         | implemented | |
| 0x27 | 1      | VTX_YZ         | implemented | |
| 0x28 | 1      | VTX_DIFF       | implemented | 10-bit signed deltas |
| 0x29 | 1      | POLYGON_ATTR   | implemented | see attribute notes |
| 0x2A | 1      | TEXIMAGE_PARAM | implemented | see texture notes |
| 0x2B | 1      | PLTT_BASE      | implemented | 13-bit palette offset |
| 0x30 | 1      | DIF_AMB        | implemented | incl. vertex-color-from-diffuse bit |
| 0x31 | 1      | SPE_EMI        | implemented | incl. shininess-table enable bit |
| 0x32 | 1      | LIGHT_VECTOR   | implemented | direction + specular reciprocal |
| 0x33 | 1      | LIGHT_COLOR    | implemented | |
| 0x34 | 32     | SHININESS      | implemented | 128-byte table |
| 0x40 | 1      | BEGIN_VTXS     | implemented | tri/quad/tri-strip/quad-strip |
| 0x41 | 0      | END_VTXS       | implemented | |
| 0x50 | 1      | SWAP_BUFFERS   | implemented | immediate rendering; no queue |
| 0x60 | 1      | VIEWPORT       | implemented | see viewport notes |
| 0x70 | 3      | BOX_TEST       | stub        | accepted, result register = 0 |
| 0x71 | 2      | POS_TEST       | stub        | accepted, result register = 0 |
| 0x72 | 1      | VEC_TEST       | stub        | accepted, result register = 0 |
| other| —      | unknown/NOP    | ignored     | counted in n_unknown_cmds |

**Coverage: 35/38 geometry commands implemented (92%); the 3 stubs are the
hit-test commands (BOX/POS/VEC_TEST), which do not affect rendering.**

## Texture formats (TEXIMAGE_PARAM bits 26-28)

| Fmt | Format                      | Status      | Notes |
|-----|-----------------------------|-------------|-------|
| 0   | none                        | implemented | vertex color only |
| 1   | A3I5                        | implemented | alpha = ((p>>3)&0x1C)+(p>>6) |
| 2   | 4-color (2bpp paletted)     | implemented | palette base = texpal<<3 |
| 3   | 16-color (4bpp paletted)    | implemented | used by golden test |
| 4   | 256-color (8bpp paletted)   | implemented | |
| 5   | 4x4-texel compressed        | implemented | melonDS TextureLookup case-5 mirror: block table at vram 0x20000+(addr&0x1FFFC)/2, modes 0-3, slot-1 texel-0 quirk (tests T8) |
| 6   | A5I3                        | implemented | |
| 7   | direct 15bpp (BGR555+A)     | implemented | bit15 = alpha (0/31) |

Wrapping (repeat/flip per axis, bits 16-19), color-0 transparency (bit 29),
and palette-RAM lookups follow melonDS TextureLookup() arithmetic exactly.
**Coverage: 8/8 formats (100%).**

## Rasterizer / shading status

Implemented (software path, deterministic; faithful port of the melonDS
scanline rasterizer, replacing the earlier barycentric fill):
- Slope + Interpolator with 8/9-bit perspective-factor attribute
  interpolation and the X-major edge interpolation-offset quirk
  (GPU3D_Soft.h:74-431); integer screen positions (melonDS FinalPosition,
  GPU3D.cpp:1317-1343 incl. the W>0xFFFF divider quirk)
- span fill: left edge / interior / right edge with the hardware edge-fill
  rules (edges always filled when AA or edge marking is on), AA coverage
  stepping per edge, and the AA second pixel layer
  (GPU3D_Soft.cpp:940-1395); opaque plot with push-down (top layer only,
  melonDS `pixeladdr < BufferSize`), translucent
  PlotTranslucentPixel with the translucent-polygon-ID skip rule
  (GPU3D_Soft.cpp:565-601) and the underneath-depth-test fallback.
  Layout: melonDS keeps the second layer at `pixeladdr + BufferSize` in
  unified 2*w*h buffers (GPU3D_Soft.h:475-477); GxSurface keeps separate
  per-layer w*h arrays (color2/depth2/attr2 at base addr) and the raster
  code routes layer writes through plane pointers accordingly
  (t_23498a08 — before that the depth-fail path used unified indexing
  against the separate arrays, corrupting the wrong buffer)
- depth: melonDS FinalZ ((z*0x4000)/w + 0x3FFF)*0x200 clamped 24-bit
  (GPU3D.cpp:1537-1547) or W-buffer via SWAP_BUFFERS param bit 1 with
  per-polygon 16-bit-normalized W (1516-1551); depth tests
  Equal_Z / Equal_W (POLYGON_ATTR bit 14) and LessThan /
  LessThan_FrontFacing tie rule (GPU3D_Soft.cpp:385-425); bit 11 =
  translucent depth write
- backface culling in clip space (melonDS (x,y,w)-normal dot test, before
  clipping); POLYGON_ATTR bits 6/7 render front/back faces; ZeroDotW
  check (GPU3D.cpp:1357-1390)
- shading modes (melonDS RenderPixel arithmetic, 457-562): modulation,
  decal, toon (32-entry table, index = vr>>1), highlight (channels set to
  red first, toon added after texturing); FinalColor 9-bit expansion
  (1463-1468) and 12.20 clip scale + post-clip rounding (1108-1116)
- alpha: per-polygon alpha (bits 16-20), texture alpha modulation
  ((ta+1)*(pa+1)-1)>>5, alpha test (alpha <= ref discards;
  RenderPixel->span-fill placement, GPU3D_Soft.cpp:1157-1161),
  melonDS AlphaBlend for translucent pixels (DISP3DCNT bit 3 gate)
- render order: melonDS YSort, SortKey = (ybot<<8)|ytop with translucent
  flag 0x10000, stable sort of the opaque set (or all, per SWAP sort mode
  bit 0) with translucent in submission order (GPU3D.cpp:1511-1512,
  2632-2671)
- lighting: ambient+diffuse+specular with shininess table, 4 lights,
  integer arithmetic identical to melonDS CalculateLighting()
- clipping/culling: mirrors melonDS GPU3D.cpp SubmitPolygon/ClipPolygon —
  culling in clip space on the (x, y, w) normal BEFORE clipping (dot < 0 =
  back face / POLYGON_ATTR bit 7, dot > 0 = front / bit 6, dot == 0 draws);
  Sutherland-Hodgman against z=±w, y=±w, x=±w with the far-plane reject
  (z > w drops the polygon unless POLYGON_ATTR bit 12); attributes
  interpolate with hardware trunc-toward-zero arithmetic; W truncated to
  24 bits and any w == 0 vertex makes the polygon degenerate (not
  rendered)

Frame post pass (gx_post_pass, ScanlineFinalPass GPU3D_Soft.cpp:1453-1621):
- edge marking (DISP3DCNT bit5): 4-neighbor polyid/depth contour test,
  EdgeTable[polyid>>3] color, alpha kept (1459-1490); every marked pixel
  also breaks antialiasing coverage — the attr coverage field is forced to
  16 (`(attr & 0xFFFFE0FF) | 0x00001000`) before the AA pass, so it blends
  17/32 against the second layer (1486-1487)
- fog (DISP3DCNT bit7): per-pixel density from the depth buffer via the
  34-entry table + offset/shift (CalculateFogDensity 1414-1451), color
  blend unless bit6 (alpha-only fog), alpha always blended (1492-1570);
  fog is applied to the topmost TWO pixels — the AA second layer is fogged
  too, gated on the top pixel's edge flags (attr & 0xF) and the lower
  pixel's own fog flag (attr bit15), with density from the lower pixel's
  depth (1500-1503, 1527-1570)
- antialiasing (DISP3DCNT bit4): coverage blend with the second layer,
  alpha always blended (1570-1620)

Partial / staged:
- wireframe polygons (alpha = 0) are drawn filled at forced alpha 31;
  1px-edge wireframe rendering is staged
- translucent polygons blend in submission order (no depth-sorted
  translucent queue)
- viewport: x1,y1 = top-left, x2,y2 = bottom-right (GBATEK reading); the
  melonDS source stores viewport Y flipped (191-y) because of its internal
  bottom-up buffers — our surface is top-down PPM so y0 is the top row

Stubbed (ignored, documented):
- shadow volumes / polygon-ID shadow masking + stencil buffer
  (IsShadow/IsShadowMask GPU3D.cpp:1425-1426, RenderShadowMaskScanline +
  StencilBuffer GPU3D_Soft.cpp:1122-1132/1218-1228, PlotTranslucentPixel
  shadow branch 570-583): mode-3 polygons render as ordinary translucent
  polygons.  NOT exercised by the canonical capture (0 shadow polys).
- FIFO timing, stalls, IRQ behavior (out of scope for the render base)

Known residual vs melonDS on the canonical 3D capture: 1.30% of pixels
(see GX_TRACES.md §4-§5) — AA second-layer/coverage divergence on dense
character meshes and two translucent shadow quads.

## GL backend skeleton (src/gx_gl.c)

Status: SKELETON.  SDL2 window + GL compatibility context; draws the same
decoded GxPolygon stream the software rasterizer receives (proven by
tools/gx_gl_smoke.c: the GX stream's polygons reach the GL draw callback and
cover exactly the same 8970 pixels as the software rasterizer).  Fidelity is
lower by design: vertex color x texture modulation (no toon/highlight
emulation), approximate depth normalization.  Under Xvfb the GL renderer is
llvmpipe (software GL); on a display with GPU drivers the same code runs on
real hardware.

## Determinism

All pipeline math is integer; identical input streams produce byte-identical
color/depth buffers and PPM output.  Verified:
- 3 in-process renders (tools/gx_cube_test --runs 3): identical FNV hashes
  and byte-compare of color+depth buffers
- 3 separate processes (tests/check_determinism.sh): byte-identical PPM,
  matching the committed golden tests/golden/cube.ppm

## Test evidence

    make            # build library, tests, tools
    make test       # unit tests + golden determinism (3 + 3 runs)
    make golden     # compare against tests/golden/cube.ppm
    make gl-smoke   # SDL2+GL smoke test under xvfb-run
    make check      # all of the above

Real-game validation (melonDS trace capture + replay + per-pixel diff) is
documented in GX_TRACES.md; the tooling is tools/gx_replay.c and
tools/gx_tracediff.py and the capture patch is ref/melonds-gxfifo-log.patch.

Unit tests pin matrix ops, the transform/viewport mapping, culling, lighting,
FIFO command packing, texcoord transforms, texture decode and shading modes
against hand-computed constants (arithmetic shown in the test comments), and
the post passes (fog density/blend/alpha-fog/second-layer fog, edge marking
incl. the break-AA-coverage write, AA final pass):
tests/test_matrix.c (51 checks), tests/test_tex.c (99), tests/test_shade.c
(16), tests/test_post.c (46) — 212 checks total.

Golden note (Phase-3B): tests/golden/cube.ppm was regenerated (1176 pixels
changed) because the rasterizer model changed from barycentric fill to the
melonDS scanline port; the change is justified line-referenced above and the
golden is re-pinned by the 3+3 determinism check.

## 2D engine coverage (Phase-3B2, src/gx_2d.c + src/gx_2d.h)

The DS 2D engine and screen compositor are a faithful port of the melonDS
software 2D renderer's scanline model (GPU2D_Soft.cpp DrawBG_x/DrawSprite_x,
GPU2D.cpp window + register-latch timing, GPU_ColorOp.h color math,
GPU_Soft.cpp display path), the same way gx_raster.c mirrors the 3D
rasterizer.  Integer-only; output bytes match melonDS framebuffers exactly.
Input is the per-unit GX2D state image dumped by ref/melonds-2d-log.patch
(f%05u.2d, layout in gx_2d.h) plus the melonDS 3D layer (f%05u.fb).

| Feature | Status | Notes |
|---------|--------|-------|
| BG modes 0-5 dispatch | implemented | melonDS DrawScanlineBGMode<0..5> layer/priority order |
| BG modes 6/7 | implemented | Mode6 large-bitmap BG2 + 3D; Mode7 BG0/BG1 text + 3D |
| text BG | implemented | 16/256-color, h/v flip, 4 screen sizes + wide-area bit, char/screen bases, palette banks |
| affine BG | implemented | PA-PD, ref points w/ per-scanline B/D increments (UpdateRegistersPostDraw model), overflow bit, 4 sizes |
| extended BG | implemented | affine+text mixed (ext palette, flips) and bitmap modes (256-color palette + direct-color 15bpp) |
| large BG (mode 6) | implemented | 512x256..1024x512 bitmap, BG2 only |
| sprites | implemented | all 128 OAM entries, 12 sizes x normal/rotscale/double-size, 16/256-color, 1D/2D OBJ mapping (all 4 DISP3CNT variants), bitmap sprites (32/64-wide), x/y flip, semitransparent mode, OBJ-window sprites, priority interleave, sprite Y mosaic + X mosaic post-pass |
| palette decode | implemented | 15bpp BGR555 + bit15 bottom-green-bit (melonDS [Gericom] note), 4 BG banks, extended BG (4 slots) and OBJ palettes per engine |
| windowing | implemented | WIN0/WIN1/OBJ-window/outside masks, X/Y edge latch semantics (CalculateWindowMask/UpdateWindows verbatim, incl. wrap ranges) |
| color special effects | implemented | alpha blend (EVA/EVB), brighten/darken (EVY) on 18-bit values, sprite-blend and 3D-layer blend rules (flag bits 0x80/0x40, ColorBlend5 5-bit alpha) |
| mosaic | implemented | BG X (table) + Y (latched rows), OBJ Y + X post-pass latch rules |
| display output | implemented | engine A modes 0-3 (off/regular/VRAM/FIFO), engine B off/regular, master brightness up/down with clamp, screen-off fills, POWCNT screens-disabled black, screen-swap buffer routing |
| compositor | implemented | 2D layers + validated 3D layer (BG0, 5-bit alpha) + engine B, melonDS SoftRenderer frame model byte-for-byte |
| display capture (DISPCAPCNT) | absorbed | replay is snapshot-based: capture writes land in the next unit's VRAM snapshot, so DoCapture need not be replayed; register values are captured in the state image for completeness |
| mid-frame writes | limitation | one state snapshot per frame (scanline 0); mid-frame register/VRAM/palette/OAM writes within a frame are not represented. Measured impact on the real capture: 1 of 820 units (a transition frame with mid-frame palette writes; see GX_TRACES.md 7) |

Tests: tests/test_2d.c — 48 checks (text BG 16/256-color + flips + scroll,
affine identity, sprites normal/transparent/semitransparent, window 0
clipping, alpha blend/brighten/darken, master brightness + clamp, engine B
palette routing + screen-off fill, X mosaic, 3x in-process determinism).
Golden: tests/golden/scene2d.ppm (mixed scene: 256-color BG + 16-color BG +
sprites + window + blend + master brightness on both engines), re-pinned by
the 3+3 determinism check in tests/check_determinism.sh (`make golden2d`).

## Lighting hand-computation (T8 reference)

vec = identity; light 0 dir = (0,0,-512) raw -> hardware negates ->
light_dir = (0,0,+512); spec_recip = 2^18/((512+512)) = 256.
normal = (0,0,1023) -> nt = (0,0,1023); dot = (512*1023)>>9 = 1022.
diffuse = 0; specular dot = 1022+1023 = 2045 -> 11-bit wrap = -3 ->
(-3*-3)>>10 = 0 -> shinelevel = -512 -> 0.  ambient 4, light color 31:
vtxbuff = (0 + 4<<9)*31 = 63488 -> 63488>>14 = 3.  Expected vertex color
(3,3,3) — asserted by tests/test_matrix.c.
