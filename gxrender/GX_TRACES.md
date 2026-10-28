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
| real 3D slice | 33000-33009, 36000-36004, 40000-40004, 44000-44004 (25) | **90.29%** | 9.71% | 56 | 100.0% / 100.0% | DIFFERENT |

Per-unit 3D exact% (honest numbers, stable across the window):
33000: 89.62->90.33 (see CSV), 36000: 90.25, 40000: 90.30, 44000: 90.30;
min 90.25%, max 90.33%, diff pixels 4751-4793 of 49152, max channel delta
55-56.  The full per-unit table is reproducible with the commands above
(CSV via --csv).  For comparison, the pre-fix exploratory number (mid-stream
slice start) was 0.18% exact / 99.82% differ / 40.8% replay coverage.

What the residual 9.7% is (triptych analysis of unit 44000):
1. **fog** — the dense fully-mismatched band along the top of the frame is
   the distant building/roofline area; the game enables fog (DISP3DCNT
   0x203b, fog regs captured) and gxrender still stubs it;
2. **edge marking** — red pixels form the OUTLINES of characters and scene
   objects (interiors match), i.e. melonDS's 1px edge-marking outlines;
3. minor edge/interpolation rounding (melonDS's 8/9-bit factor
   interpolation vs our exact barycentric, see GX_COVERAGE.md).

4x4-compressed textures decode correctly on real scene data (the earlier
magenta stub is gone; storefront/awning textures render in place).

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

Open (measured, dominating the residual diff):
- fog (regs captured; renderer stub) — top-band color deltas
- edge marking / antialiasing / shadow volumes, polygon-ID masking (no
  attr buffer) — outline deltas around characters and objects
- melonDS's 8/9-bit factor interpolation and depth-test tie/variant rules
  (DepthTest_Equal_Z/W for translucent-over-opaque) vs our exact barycentric
  interpolation — small edge-rounding deltas
- ZeroDotW check not implemented in the submit path (register now captured;
  default limit 0xFFFFFF makes it a no-op unless the game writes 0x610)

## 6. Remaining work (next run)

1. fog + edge marking implementation (the two named residual classes).
2. W-buffer depth alignment with melonDS's per-polygon 16-bit-normalized W
   if a W-buffered scene shows occlusion deltas (all sampled scene polygons
   are Z-buffered; ordering is monotonic-equivalent there).
3. Extend the final table with a walk-around slice (needs a PRESS
   d-pad schedule) once fog/edge marking land.
