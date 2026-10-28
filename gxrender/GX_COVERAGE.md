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

Implemented (software path, deterministic):
- barycentric scanline fill with the top-left rule; pixel centers in 1/16 px
  (on-edge pixels count on top edges — horizontal, running right — and left
  edges — any up-running edge, incl. diagonal — so shared edges are gapless)
- perspective-correct attributes (1/w) for color and texture coords
- backface culling in clip space (melonDS (x,y,w)-normal dot test, before
  clipping); POLYGON_ATTR bits 6/7 render front/back faces
- depth: Z-buffer (z/w, 12.12) or W-buffer (clip w) per POLYGON_ATTR bit 14;
  pass if new depth < old (first-submitted wins ties)
- shading modes (melonDS RenderPixel arithmetic): modulation, decal,
  toon (palette 0..31 table), highlight (toon added after texturing, all
  channels set to the red component first)
- alpha: per-polygon alpha (bits 16-20), texture alpha modulation
  ((ta+1)*(pa+1)-1)>>5, 1-bit alpha test (alpha <= ref discards, ref = 0),
  melonDS AlphaBlend for translucent pixels (DISP3DCNT bit 3 gate)
- lighting: ambient+diffuse+specular with shininess table, 4 lights,
  integer arithmetic identical to melonDS CalculateLighting()

Partial / staged:
- clipping/culling: mirrors melonDS GPU3D.cpp SubmitPolygon/ClipPolygon —
  culling in clip space on the (x, y, w) normal BEFORE clipping (dot < 0 =
  back face / POLYGON_ATTR bit 7, dot > 0 = front / bit 6, dot == 0 draws);
  Sutherland-Hodgman against z=±w, y=±w, x=±w with the far-plane reject
  (z > w drops the polygon unless POLYGON_ATTR bit 12); attributes
  interpolate with hardware trunc-toward-zero arithmetic; W truncated to
  24 bits and any w == 0 vertex makes the polygon degenerate (not
  rendered).  Unimplemented: the ZeroDotW check (0x04000610 is captured)
- wireframe polygons (alpha = 0) are drawn filled at forced alpha 31;
  1px-edge wireframe rendering is staged
- translucent polygons blend in submission order (no depth-sorted
  translucent queue)
- viewport: x1,y1 = top-left, x2,y2 = bottom-right (GBATEK reading); the
  melonDS source stores viewport Y flipped (191-y) because of its internal
  bottom-up buffers — our surface is top-down PPM so y0 is the top row

Stubbed (ignored, documented):
- fog (0x3D/0x3E/0x3F registers never reach the renderer)
- edge marking, antialiasing, shadow volumes/polygon-ID shadow masking
- FIFO timing, stalls, IRQ behavior (out of scope for the render base)

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

Unit tests (tests/test_matrix.c, tests/test_tex.c, tests/test_shade.c) pin
matrix ops, the transform/viewport mapping, culling, lighting, FIFO command
packing, texcoord transforms, texture decode and shading modes against
hand-computed constants (arithmetic shown in the test comments).

## Lighting hand-computation (T8 reference)

vec = identity; light 0 dir = (0,0,-512) raw -> hardware negates ->
light_dir = (0,0,+512); spec_recip = 2^18/((512+512)) = 256.
normal = (0,0,1023) -> nt = (0,0,1023); dot = (512*1023)>>9 = 1022.
diffuse = 0; specular dot = 1022+1023 = 2045 -> 11-bit wrap = -3 ->
(-3*-3)>>10 = 0 -> shinelevel = -512 -> 0.  ambient 4, light color 31:
vtxbuff = (0 + 4<<9)*31 = 63488 -> 63488>>14 = 3.  Expected vertex color
(3,3,3) — asserted by tests/test_matrix.c.
