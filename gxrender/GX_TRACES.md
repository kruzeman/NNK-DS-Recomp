# GX_TRACES.md — real-game GX capture & frame-diff methodology (Phase-1B)

Scope: validate/extend gxrender against REAL Dragon Quest VIII-era DS game
rendering — here Ni no Kuni DS (game code B2KJ family, fan-translated ROM
`/home/hermes/NiNoKuni/Ni No Kuni.nds`) — instead of synthetic scenes.
All capture/diff code lives in gxrender/; trace binaries are gitignored.

## 1. Capture: melonDS patch

`ref/melonds-gxfifo-log.patch` (against melonDS 1e654443, sources at
/home/hermes/tools/melonDS) adds an env-gated capture facility to the
melonDS core only (src/GPU3D.h, src/GPU3D.cpp, src/GPU3D_Soft.cpp; no
frontend/CMake changes).  Re-apply on a fresh checkout:

    cd /home/hermes/tools/melonDS && git checkout -- src
    git apply --check gxrender/ref/melonds-gxfifo-log.patch   # verified OK
    git apply gxrender/ref/melonds-gxfifo-log.patch

### Environment knobs (all optional; unset MELONDS_GXLOG_DIR = patch inactive)

| Var | Meaning |
|-----|---------|
| MELONDS_GXLOG_DIR | output directory; enables capture |
| MELONDS_GXLOG_FRAMES | exit(0) after N units (0 = run until killed) |
| MELONDS_GXLOG_FBDUMP | 1 = dump f%05u.fb per unit (default 1) |
| MELONDS_GXLOG_FBEVERY | dump the framebuffer every Nth unit (default 1) |
| MELONDS_GXLOG_TEXEVERY | dump texture memory every Nth unit (default 1) |
| MELONDS_GXLOG_TEXFROM | first unit to dump texture memory for (default 0) |
| MELONDS_GXLOG_2DEVERY | dump 2D compositor state + final screens every Nth unit (0/unset = no 2D dumps; patch v2d) |
| MELONDS_GXLOG_2DFROM | first unit to dump 2D state for (default 0) |
| MELONDS_GXLOG_RANGES | "a-b;c-d" inclusive unit windows: when set, ALL per-unit dumps (.fb/.texvram/.texpal/.2d/.scr) are restricted to units in any range (multi-window captures stay small); .gx streams always record |
| MELONDS_GXLOG_PACE | throttle to N units/second (0 = unthrottled) |
| MELONDS_GXLOG_PRESS | deterministic input schedule (below) |

MELONDS_GXLOG_PRESS entries are `unit:kind:arg`, semicolon-separated,
applied at the VBlank that starts that unit (exact, emulated-frame
deterministic, bypasses the frontend key map):

    14500:touch:128,70   touch bottom screen at unit 14500 (NDS::TouchScreen)
    14530:touch:up       release (NDS::ReleaseScreen)
    1200:keys:fffb       KEYINPUT mask (hex, active-low; NDS::SetKeyMask)

### Trace file format

A "unit" is one VBlank-delimited slice of GX traffic (GPU3D::VBlank closes
the unit).  Per unit N:

| File | Content |
|------|---------|
| f%05u.gx | record stream, little-endian: `01 <u32 word>` raw GX FIFO word; `02 <u8 width> <u32 addr> <u32 val>` 3D MMIO write (04000060h DISP3DCNT, 04000330h-040003FEh edge/fog/alpha-ref/clear/toon); `03 <u8 cmd> <u32 param>` direct command-port write (04000440h-040005CCh) |
| f%05u.fb | melonDS 3D layer after rendering unit N: 256*192 u32, `r \| g<<8 \| b<<16 \| a<<24`, 6-bit rgb + 5-bit alpha (SoftRenderer3D::ColorBuffer visible window) |
| f%05u.texvram | 512 KiB flat texture VRAM at render time (GPU::VRAMFlat_Texture) |
| f%05u.texpal | 128 KiB flat texture palette RAM (GPU::VRAMFlat_TexPal) |
| f%05u.2d | GX2D compositor state at display start (scanline 0) of the frame whose 3D layer is f%05u.fb (patch v2d, see §7): container "GX2D" `<u32 ver=1> <u32 unit> <u32 flags> <u32 size=0x195400>`, flags bit0 = keyframe; keyframe payload = raw state image, delta payload = `<u32 nruns> { <u32 off> <u32 len> <bytes> }*` vs the previous dumped unit.  State image layout documented in gx_2d.h (engine regs x2, global regs, palette, OAM, flat BG/OBJ/ext-palette VRAM views, raw VRAM_A..D) |
| f%05u.scr | final composed screens at display end (scanline 191) of that frame: 2 x 256*192 u32, 8-bit BGRA post master-brightness (melonDS Framebuffer[BackBuffer][0] then [1]) |

Note: with FRAMES=N the last recorded unit has no .fb (the process exits at
the closing VBlank before that unit's render).  Unit i's .fb is produced at
the NEXT frame's Finish3DRendering; .fb/.texvram are snapshots at render
(VCount 215) of the same unit.

## 2. Capture session (boot to title + 3D scene)

Command (real BIOS at /home/hermes/NiNoKuni/bios, configured in
~/.config/melonDS/melonDS.toml [DS] BIOS9Path/BIOS7Path/FirmwarePath +
[Emu] ExternalBIOSEnable; run under Xvfb):

    SDL_AUDIODRIVER=dummy DISPLAY=:99 \
    MELONDS_GXLOG_DIR=traces/<name> MELONDS_GXLOG_FRAMES=N ... \
    xvfb-run -a /home/hermes/tools/melonDS/build/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"

Timeline established from capture + window screenshots (melonDS 1.1,
software 3D renderer, threaded):

| Unit (approx) | Screen | GX traffic |
|---|---|---|
| 0–10500 | BIOS/boot logos, LEVEL-5 splash (2D) | none |
| 10750–14250 | TITLE screen (「NI NO KUNI 漆黒の魔導士」, ©2010 LEVEL-5) — 2D artwork | 52-entry idle loop/frame (matrix setup + SWAP, no vertices) |
| 14500 | tap "New Game" (bottom screen touch at 128,70) | transition burst |
| 15000–25000 | fan-translation notice dialog (2D) | none |
| 25750–43300 | opening cutscene, "MotorCity HOUROIT" billboard (2D) | none |
| 43300+ | **real 3D street scene** (Motorville: textured shops, lit character models, vehicles, shadows) | ~12.5k entries/frame: VTX_16, TEXCOORD, NORMAL, LIGHT_*, MTX_*, TEXIMAGE_PARAM |

Important honesty note: the title screen does NOT exercise the 3D engine
beyond clear-color frames.  Real geometry validation needs the in-game 3D
scene, which needs input; that is why MELONDS_GXLOG_PRESS exists.

Sets kept (gitignored): traces/boot60s (boot→title, 3600 units, full fbs),
traces/slice3d (units 43440-43560 of a live-interaction session: real 3D
frames with fbs + 3 texture snapshots), traces/smoke1, traces/schedA
(schedule-verification run).

## 3. Replay & diff

    make
    ./build/gx_replay <trace_dir> <out_dir> [--start N] [--ppm] [--all]
    python3 tools/gx_tracediff.py <trace_dir> <out_dir> [--units A-B] \
            [--png <dir>] [--csv <file>]

`gx_replay` feeds the captured streams through the gxrender software path
and reproduces melonDS's frame model as documented in the melonDS source:
render-env registers latch at SWAP flushes (GPU3D::VBlank), submitted
polygons are retained and re-rendered each frame from the last flushed set
(RenderPolygonRAM/RenderNumPolygons — a flush with no polygons keeps the
previous set), textures come from the unit's snapshot, the clear follows
SoftRenderer3D::ClearBuffers (solid + clear-image mode), render order is
melonDS YSort (opaque by SortKey `(ybot<<8)|ytop`, translucent after in
submission order).  Output .raw has the .fb layout (byte-comparable).

`gx_tracediff` reports exact-match %, differing pixels, per-channel max
delta, alpha-vs-color mismatch split and coverage on both sides (DQ8
compare_frames.py conventions extended).

## 4. Canonical capture and FINAL diff report

Canonical deterministic capture (input-fix build, patch v2), kept as
traces/canonB (gitignored):

    SDL_AUDIODRIVER=dummy DISPLAY=:99 \
    MELONDS_GXLOG_DIR=traces/canonB MELONDS_GXLOG_FRAMES=48000 \
    MELONDS_GXLOG_FBDUMP=1 MELONDS_GXLOG_FBEVERY=1 \
    MELONDS_GXLOG_TEXEVERY=1 MELONDS_GXLOG_TEXFROM=33000 \
    MELONDS_GXLOG_PACE=60 \
    MELONDS_GXLOG_PRESS='14500:touch:128,70;14530:touch:up;14700:touch:128,85;
    14730:touch:up;14900:touch:128,100;14930:touch:up;16000:touch:128,85;
    16030:touch:up;16500:touch:220,175;16530:touch:up;17000:touch:130,175;
    17030:touch:up' \
    xvfb-run -a /home/hermes/tools/melonDS/build/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"

Determinism: units 0-14499 (before the first injected input) are
byte-identical to the earlier traces/schedA run (md5-verified at 12 sampled
units).  Timeline: title screen from ~unit 101, "New Game" tapped at 14500,
notice/dialog taps 16000-17000, real 3D (Morton Street: textured shops,
characters, car) from unit 19589 through 48000 (vtx/fmt5 traffic in every
other unit, [geometry unit][SWAP unit] cadence).

Two capture fixes were required (root causes found this phase):

1. **Input injection was being clobbered**: melonDS's frontend EmuThread
   re-applies its own input state every emu-loop iteration
   (qt_sdl/EmuThread.cpp SetKeyMask/TouchScreen/ReleaseScreen), undoing
   MELONDS_GXLOG_PRESS events within the same frame (evidence: traces/schedA
   — identical attract cycle before/after the taps, zero VTX in 48000
   units).  Patch v2 locks input to the schedule while one is active
   (GXLog::InputLocked gate in NDS::SetKeyMask/TouchScreen/ReleaseScreen).
2. **Tap coordinates**: the "New Game" button occupies DS (52,72)-(208,97)
   (measured from window screenshots); the designed tap (128,70) misses its
   top edge by ~2px.  The canonical schedule taps (128,85) with fallbacks.

### FINAL diff (replay FROM UNIT 0, tools/gx_replay + tools/gx_tracediff)

    ./build/gx_replay traces/canonB traces/canonB_out \
        --render 11000-11005 --render 12000-12005 --render 13000-13005 \
        --render 14000-14005 --render 33000-33009 --render 36000-36004 \
        --render 40000-40004 --render 44000-44004
    python3 tools/gx_tracediff.py traces/canonB traces/canonB_out --units 10000-14999
    python3 tools/gx_tracediff.py traces/canonB traces/canonB_out --units 33000-47999

| Set | Units | exact | differ | max delta | cov mel / rep | verdict |
|-----|-------|-------|--------|-----------|---------------|---------|
| title screen (2D-only 3D layer) | 11000-14005 (24) | **100.00%** | 0.000% | 0 | 0.0% / 0.0% | IDENTICAL |
| real 3D slice (Phase-1B-2, BEFORE) | 33000-33009, 36000-36004, 40000-40004, 44000-44004 (25) | **90.29%** | 9.71% | 56 | 100.0% / 100.0% | DIFFERENT |
| real 3D slice (Phase-3B, AFTER) | same 25 units | **98.70%** | **1.30%** | 63 | 100.0% / 100.0% | DIFFERENT |
| real 3D slice (Phase-3B rev2, review fixes) | same 25 units | **98.70%** | **1.297%** | 63 | 100.0% / 100.0% | DIFFERENT |
| real 3D slice (AA second-layer fix, t_23498a08) | same 25 units | **99.92%** | **0.084%** | 46 | 100.0% / 100.0% | DIFFERENT |

Phase-3B after-numbers (traces/phase3b_out2, replay FROM UNIT 0 through the
melonDS scanline rasterizer port): per-unit exact 98.67%..98.73%, diff pixels
622..656 of 49152 per unit (15981 of 1228800 total = 1.301%), mean channel
delta 14.4..15.0, max channel delta 63.  Title set still byte-identical
(24/24 units, 0 diff px).  Residual dropped 9.71% -> 1.30% (87% of the
remaining pixels closed); full CSV via `--csv`, per-unit table below.

Phase-3B rev2 after-numbers (traces/phase3b_out3, review-round-2 fixes: edge
marking forces AA coverage to 16 per GPU3D_Soft.cpp:1486-1487, fog applied to
the AA second layer per 1500-1503/1527-1570 — the latter cannot move trace
numbers, fog is never enabled in this capture): per-unit exact
98.67%..98.74%, diff pixels 620..655 of 49152 per unit (15935 of 1228800
total = 1.297%), max channel delta 63.  46 fewer diff px than rev1
(15981 -> 15935), all attributable to edge-marked partial-coverage pixels now
blending 17/32 like melonDS.  Title set still byte-identical (24/24).

AA-second-layer-fix after-numbers (traces/fix_out, t_23498a08: second-layer
addressing unified onto GxSurface's separate per-layer arrays + render-order
translucent sub-sort fix, see §5): per-unit exact 99.91%..99.92%, diff
pixels 39..43 of 49152 per unit (1028 of 1228800 total = 0.084%), max
channel delta 46.  The rev2 before-numbers above were re-measured in the
same run from the untouched rev2 binary (traces/base_rev2): 15935 px /
1.297% reproduced exactly.  Title set still byte-identical (24/24).  The
whole "alpha 31 in melonDS vs 14/0 in replay" class is gone (every one of
the 1028 remaining px is a color-only delta with alpha 31 on both sides),
and the two translucent shadow quads converged (quad-2 region: 0 diff px in
all 25 units; quad-1 region: only 2 px, adjacent to the feet band below).

Per-unit 3D exact% (honest numbers, stable across the window):
rev2: 33000: 98.73, 36000: 98.67, 40000: 98.69, 44000: 98.71 (min 98.67,
max 98.73).  AA-fix: 33000: 99.92, 36000: 99.91, 40000: 99.92, 44000:
99.92 (min 99.91, max 99.92; per-unit diff px 39..43).  The full 25-unit
table is reproducible with the commands above (`--csv` for machine form).

What the OLD residual 9.7% actually was (Phase-3B deep-dive, corrected):
the task's original "top band = fog / contours = edge marking" attribution
was WRONG.  A full-trace register scan shows DISP3DCNT is always 0x3B
effective (texture | highlight | alpha-blend | AA(bit4) | edge-marking(bit5))
— fog (bit7) is NEVER enabled in the whole 48000-unit capture (fog regs are
written but never turned on).  The top band (rows 0-27, ~85% of the residual)
was a RIGID 1-row attribute shift: our row y == melonDS row y+1 for all 256 x
(byte-exact), caused by melonDS's X-major edge interpolation offset
(GPU3D_Soft.h:320-321) combined with integer FinalPosition rasterization.
The fix was porting melonDS's actual scanline rasterizer wholesale (see
GX_COVERAGE.md "Rasterizer"), not a fog implementation.

What the rev2 residual 1.297% was (measured, unit 44000, 636 diff px;
closed by the AA-second-layer fix below, kept for history):
1. **AA edge pixels of dense character meshes** — the bulk.  Character
   models are many small triangles; at shared-edge pixels our AA second
   layer / coverage blend diverges from melonDS (e.g. same pixel color with
   alpha 31 in melonDS vs 14 in replay = blend against a transparent second
   layer; some character edge pixels 1px narrower).  Interiors of all large
   triangles (buildings, ground, car) match byte-exact.  Leading hypothesis
   (found during rev2, left to t_23498a08): the scanline fill's
   depth-fail->second-layer path (gx_raster.c:737-744, 813-819 and the
   translucent twin-plot at 789-790/932-933) indexes the second layer as
   `sf->attr/depth/color[pixeladdr + w*h]` (melonDS unified-buffer
   convention) while the push-down writes, the AA blend and gx_replay use
   separate per-layer w*h arrays (color2/attr2/depth2 at base addr) — those
   accesses land in the wrong array/out of bounds on the current GxSurface
   layout, plausibly producing exactly the "transparent second layer where
   melonDS has an opaque one" symptom.  Needs convention unification,
   ASan-verified.
2. **two translucent A5I3 quads** (alpha 26, modulate, polyid 43/23,
   x=114-142 y=104-118 and x=53-92 y=112-132 = the characters' soft ground
   shadows) — blended results differ around/under the characters.  There are
   NO shadow-volume/mask polygons in the scene (POLYGON_ATTR bits 4-5 check
   over all 418 retained polys at unit 44000: zero), so the classic DS
   stencil shadow path is not exercised by this capture.
   Classification of the 636 diff px: 394 both-foreground color+alpha,
   239 both-foreground color-only, 2 mel=fg/rep=bg, 1 both-bg alpha-only;
   melonDS side is always opaque (alpha 31) where we differ.

4x4-compressed textures decode correctly on real scene data (the earlier
magenta stub is gone; storefront/awning textures render in place).

What remains after the AA-second-layer fix (0.084% = 1028 px of 1228800,
measured across all 25 units of the slice): ONE 8x9-pixel cluster at the
character's foot/shin seam, x=125..132 y=100..108 (47 unique pixel
positions, repeated across the near-identical frames of each window;
nothing else in any frame differs by a single pixel).  All 1028 are
color-only deltas between two opaque pixels (alpha 31 both sides), max
channel delta 46.  It straddles the top edge of shadow quad 1 (y104-118)
— y100..103 is above the quad entirely, so at least part is pure
character-mesh seam shading; where the quad overlaps (y104-108) a
quad-over-feet blend nuance cannot be fully excluded but has the identical
signature.  Class: scattered both-fg color diffs at dense small-triangle
seams (old class (c)); the old classes (a) alpha-graded AA blends and (b)
1px-narrower silhouettes are fully closed, and the two translucent quads
converge everywhere except the 2 margin pixels of the feet band.  The
per-pixel write tracer (GX_TRACE_PIX, see §5) confirms the write sequences
at the former diff pixels (128,84), (130,85), (68,87) now mirror
RenderPolygonScanline exactly (push-down before top-layer replacement,
depth-fail writes into the second layer, coverage-17 aablend at
edge-marked pixels) and those pixels now match melonDS byte-exactly.

## 5. Gaps found / fixed / remaining

Fixed in this phase:
- **capture/input**: PRESS schedule now owns emulated input while active
  (see §4); 0x04000610 (ZeroDotWLimit) is captured as well.
- **4x4-texel compressed textures (fmt 5)**: implemented in gx_tex.c,
  mirroring melonDS TextureLookup case 5 exactly (slot-1 palette-info table
  at vram 0x20000+(addr&0x1FFFC)/2, all four interpolation modes, slot-1
  texel-read-0 quirk, texpal<<4 base).  17 hand-computed unit checks
  (tests/test_tex.c T8).
- **coverage gap (40.8% -> 100%)** — three compounding root causes:
  1. mid-stream replay starts zero the latched env regs: the slice replay
     had clear_attr2=0 -> clearz=511, depth-failing every polygon with
     z/w >= 0.125 in our (z/w<<12) depth scale.  Replaying FROM UNIT 0
     restores the real clear (clear_attr2=0x7FFF, clearz=0xFFFFFF);
  2. culling/clipping semantics: we culled in SCREEN space after clamping
     unclipped coordinates to +/-512px and clipped only w>=1/64.  Now
     mirroring melonDS GPU3D.cpp SubmitPolygon/ClipPolygon exactly:
     culling in CLIP space on (x,y,w) before clipping, full
     Sutherland-Hodgman against z=±w, y=±w, x=±w, far-plane reject
     (POLYGON_ATTR bit 12), W truncated to 24 bits with the w==0
     degenerate rule;
  3. the top-left fill rule only counted axis-aligned edges as
     top/left, so diagonal shared edges (this isometric scene is full of
     them) dropped on-edge pixels on both sides — 1px black seams and
     "glitched" small models.  Now any up-running edge is a left edge.

Fixed in Phase-3B (this phase):
- **fog** (0x3D FOG_OFFSET, 0x3E FOG_COLOR, 0x3F FOG_TABLE + DISP3DCNT
  bits 6/7/8-11): implemented per melonDS CalculateFogDensity
  (GPU3D_Soft.cpp:1414-1451) and ScanlineFinalPass (1492-1570), with the
  34-entry RenderFogDensityTable latch (GPU3D.cpp:2697-2701), alpha-fog and
  (rev2) fog on the AA second layer — the "lower pixel", gated on the top
  pixel's edge flags and the lower pixel's own fog flag, density from the
  lower pixel's depth (1500-1503, 1527-1570).  Verified by 13 named unit
  checks (tests/test_post.c F1-F13).  NOTE: fog is never
  enabled in the canonical capture (DISP3DCNT bit7 unset in all 48000
  units), so it contributes 0 to the trace diff — correctness rests on the
  unit tests and line-referenced semantics, not on trace pixels.
- **edge marking** (DISP3DCNT bit5): per ScanlineFinalPass
  (GPU3D_Soft.cpp:1459-1490) — candidates attr&0xF, polyid=attr>>24, mark
  when any 4-neighbor has a different polyid AND is behind (z < nz),
  color = EdgeTable[polyid>>3], alpha kept; (rev2) every marked pixel also
  breaks antialiasing coverage — attr coverage forced to 16
  ((attr & 0xFFFFE0FF) | 0x00001000, 1486-1487) so it blends 17/32 in the
  AA pass.  Verified by 6 named unit checks (tests/test_post.c E1-E6) and
  active in the trace diff (bit5 set).
- **toon/highlight** (RenderPixel GPU3D_Soft.cpp:457-562): already matched
  before this phase (index = vr>>1, 32-entry table; highlight adds the
  toon color after texturing).  Pinned by test_shade; the canonical scene
  runs in HIGHLIGHT mode (DISP3DCNT bit1), toon mode only unit-tested.
- **melonDS scanline rasterizer** (the dominant fix): Slope + Interpolator
  (GPU3D_Soft.h:74-431), span fill with edge-fill rules / AA coverage /
  second pixel layer (GPU3D_Soft.cpp:940-1395), PlotTranslucentPixel
  (565-601), integer FinalPosition incl. the W>0xFFFF divider quirk
  (GPU3D.cpp:1317-1343), 12.20 color clip scale + post-clip rounding
  (1108-1116), FinalColor 9-bit expansion (1463-1468), FinalW
  per-polygon W normalization (1516-1551), ZeroDotW check (1357-1390),
  SortKey Y-sort (1511), depth-test variants (385-425).  This replaced the
  barycentric fill and closed the 1-row shift class.

Fixed in the AA-second-layer phase (t_23498a08):
- **second-layer addressing unified** (the 1.297% residual's bulk, old
  class (a)): the scanline fill's depth-fail->second-layer path indexed the
  lower pixel melonDS-style as `pixeladdr + w*h` in unified 2*w*h buffers
  (melonDS GPU3D_Soft.h:475-477, GPU3D_Soft.cpp:1142-1147/1236-1244/
  1328-1336) while GxSurface + gx_replay keep separate per-layer w*h arrays
  (color2/depth2/attr2 at base addr) — those accesses landed in the wrong
  static arrays/out of bounds (e.g. color[addr+w*h] aliased depthbuf[addr]
  in gx_replay's .bss layout), leaving the second layer transparent where
  melonDS has an opaque one.  All second-layer accesses now route through
  layer plane pointers at the BASE addr (gx_raster.c render_polygon_scanline
  parts 1-3), `plot_translucent` takes explicit plane pointers (= melonDS
  PlotTranslucentPixel 565-601 semantics, unchanged), the AA push-down
  writes are gated on the top layer only (`pixeladdr < BufferSize` in
  melonDS, 1181-1187/1270-1276/1369-1375), and the translucent twin-plot
  blends into color2/depth2/attr2 (1199-1201/1288-1290/1387-1389).
  ASan-clean (test_post/test_shade/gx_cube_test with -fsanitize=address,
  incl. the E6/F13 second-layer checks).
- **render-order translucent sub-sort** (build_render_order,
  tools/gx_replay.c): the mode-0 sub-sort started at opaque_n+1 with an
  unbounded j, so translucent polygons could bubble INTO the opaque region
  (our poly_sortkey lacks melonDS's SortKey |= 0x10000 translucent bit,
  GPU3D.cpp:1511-1512) and the first translucent was never inserted.  Now
  a region-bounded stable insertion sort (j >= opaque_n), which reproduces
  melonDS's whole-list std::stable_sort (GPU3D.cpp:2657-2671) exactly given
  the translucent-bit key separation.  This is what makes the two
  translucent shadow quads render strictly after all opaque geometry.
- **per-pixel write tracer** (debug aid, env-gated): GX_TRACE_PIX="x,y;x,y"
  logs every framebuffer write at the listed pixels (tags
  opq1/opq2/opq3/push/trans/aacopy/aablend, L1=top layer, L2=second
  layer); GX_TRACE_LOG picks the output file (default stderr).

Open (measured, what the 0.084% residual is made of):
- one 8x9-px cluster at the character foot/shin seam (x125-132 y100-108,
  see §4 "What remains"): color-only deltas between opaque pixels at dense
  small-triangle seams.  Candidates: per-triangle interp setup at tiny
  triangles (Slope Interp roundings), texture s/t seam rounding, or a
  quad-over-feet blend nuance where shadow quad 1 overlaps the feet band
  (y104-108).  Not yet traced at those pixels (tracer ready, see above).
- shadow volumes / polygon-ID shadow masking + stencil
  (RenderShadowMaskScanline GPU3D_Soft.cpp:1122-1132, PlotTranslucentPixel
  shadow branch 570-583, IsShadow/IsShadowMask GPU3D.cpp:1425-1426): NOT
  implemented and NOT exercised by this capture (zero shadow polys in the
  canonical scene); needed when a shadow-using scene shows up.

## 6. Remaining work (next run)

1. The 0.084% residual cluster (x125-132 y100-108, character foot/shin
   seams, see §4/§5): trace writes at (128,104)/(127,105)/(129,104) with
   GX_TRACE_PIX and compare against melonDS per-triangle interpolation
   (Slope Interp roundings on tiny triangles) and texture s/t seam
   rounding; decide seam-interp vs quad-over-feet blend first (the pixels
   at y100-103 are above the quad and isolate the seam class).
2. Shadow volumes + stencil (IsShadow/IsShadowMask, RenderShadowMaskScanline)
   — implement when a capture using them appears (the canonical scene has
   none; do not stub-claim coverage).
3. Extend the final table with a walk-around slice (needs a PRESS
   d-pad schedule).

## 7. 2D compositor capture & validation (Phase-3B2)

Patch v2d (ref/melonds-2d-log.patch, layered on top of
melonds-gxfifo-log.patch; re-apply order: checkout -- src, apply v1, apply
v2d) extends the capture with two per-unit dumps, both indexed by the same
PendingUnit() slot as f%05u.fb so a unit's inputs and golden output are
consistently grouped:

* f%05u.2d — the full 2D compositor state at display START (scanline 0) of
  the display frame: both engines' register blocks (DISPCNT/BGxCNT/BGxOFS/
  BGxPA-PD/BGxX-Y ref+reload/WIN/MOSAIC/BLD/EVA-EVY/win-active latches),
  global regs (master brightness, POWCNT-derived enables + screen swap,
  VRAMCNT, DISPCAPCNT), palette RAM, OAM, all flat BG/OBJ/ext-palette VRAM
  views (coherency forced before dump), raw VRAM_A..D (VRAM display /
  capture sources) and the display FIFO buffer.  1659904 bytes per keyframe,
  delta-encoded vs the previous dumped unit (one run per changed region;
  auto-keyframe when the delta exceeds 3/4 of the size).  Snapshot point =
  end of DrawScanline(0), i.e. after the VBlank IRQ's register writes and
  before PostDraw(0)'s ref increments — the exact state the frame's 192
  scanlines start from (BGXRef == BGXRefReload at this point).
* f%05u.scr — the final composed screens at display END (scanline 191,
  post ExpandColor): Framebuffer[BackBuffer][0] + [1], the exact bytes the
  frontend displays.  This is the golden reference.

Replay: tools/gx_2dreplay.c renders each unit through gx_2d (scanline loop
replicating melonDS's PreDraw/DrawScanline/DrawSprites/PostDraw sequencing
incl. the line-262 frame-start hblank) with f%05u.fb as the 3D layer input,
then per-pixel diffs against f%05u.scr.  IMPORTANT: the .2d delta chain is
relative to the previously DUMPED unit, so the tool must be run over a
contiguous unit range in one invocation (isolated sub-ranges start from a
zero state and produce garbage).

### Canon2d capture + honest numbers

    MELONDS_GXLOG_DIR=traces/canon2d MELONDS_GXLOG_FRAMES=22000 \
    MELONDS_GXLOG_2DEVERY=1 MELONDS_GXLOG_2DFROM=0 \
    MELONDS_GXLOG_RANGES='1200-1250;3500-3550;6000-6050;9000-9050;\
    12000-12050;14250-14510;15100-15150;16200-16250;19590-19740;21000-21050' \
    MELONDS_GXLOG_PRESS='<canonB schedule>' ... melonDS "Ni No Kuni.nds"

    ./build/gx_2dreplay traces/canon2d traces/canon2d_out \
        --units 1200-21050 --quiet --csv traces/canon2d_out/all.csv
    python3 tools/summarize_2d.py traces/canon2d_out/all.csv

Timeline windows (canonB timeline: title from ~101, New Game tap 14500,
Morton Street 3D from 19589).  820 units captured (.2d + .scr + .fb each):

| Window | Content | Units | diff px | max delta | exact |
|--------|---------|-------|---------|-----------|-------|
| 1200-1250 | title screen (2D) | 51 | 0 | 0 | 100.0000% |
| 3500-3550 | title screen (2D) | 51 | 0 | 0 | 100.0000% |
| 6000-6050 | title screen (2D) | 51 | 0 | 0 | 100.0000% |
| 9000-9050 | title screen (2D) | 51 | 0 | 0 | 100.0000% |
| 12000-12050 | title screen (2D) | 51 | 0 | 0 | 100.0000% |
| 14250-14510 | title -> New Game transition (fade + tap at 14500) | 261 | 65024 | 207 | 99.7466% |
| 15100-15150 | notice dialog (2D) | 51 | 0 | 0 | 100.0000% |
| 16200-16250 | dialog (2D) | 51 | 0 | 0 | 100.0000% |
| 19590-19740 | **mixed 2D+3D street scene** (3D layer + 2D) | 151 | 0 | 0 | 100.0000% |
| 21000-21050 | **mixed 2D+3D street scene** | 51 | 0 | 0 | 100.0000% |
| TOTAL | | 820 | 65024 | 207 | **99.9193%** (80544256 / 80609280 px) |

819 of 820 units are byte-identical (0 diff pixels).  The 65024 differing
pixels are all in ONE unit (14444) — root cause traced to the documented
single-snapshot model limit: unit 14444's only state change is the master
brightness white-fade ramp (0x4001 -> 0x4003 both engines, verified via
tools/dumpdelta.py on the .2d container), but melonDS's golden frame shows
the transition's PALETTE fade (backdrop -> white 0x7FFF) written MID-FRAME —
rows 0-9 of buffer 0 (pre-write scanlines) match byte-exact, rows 10-191
use the mid-frame palette (251 vs 44); buffer 1's write lands at row 120.
Per-row analysis: tools/rowdiff.py.  Fixing this requires capturing
mid-frame write timing (a per-scanline palette/OAM/VRAM write log in the
patch) — future work; every other captured unit, including all pure-2D
title/dialog frames and all mixed 2D+3D frames, is byte-exact.

Capture determinism: two unthrottled runs of the boot window (120 units,
598 files) are byte-identical (cmp-verified); gx_2dreplay over the same
chain twice produces identical CSVs.

3D regression: the canonB 3D replay/diff was re-run after the 2D work
(traces/reg3d_out, replay from unit 0 through the same --render windows as
§4): title set 24 units IDENTICAL (0 diff px), real 3D set 25 units
**1028 differing pixels (0.084%), max channel delta 46** — byte-for-byte the
Phase-3B accepted baseline, no regression (logs traces/reg3d_diff_*.log).

Trace tooling added: tools/gx_2dreplay.c (replay + diff), tools/summarize_2d.py
(per-window summary), tools/dumpdelta.py (inspect .2d delta runs),
tools/probe_frame.py / tools/rowdiff.py (pixel forensics), tools/ppm2png.py.
