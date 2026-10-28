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

## 4. First diff report (exploratory — NOT the final numbers)

Slice units 43540–43545 (real 3D street scene) replayed starting mid-stream
at unit 43440 (pipeline state warm-from-wrong-point):

    A=traces/slice3d units=6
    B=traces/slice3d_out
    compared=294912 differing_pixels=294382 (99.820%) max_channel_delta=63
    exact_match_avg=0.18%
    VERDICT: DIFFERENT (99.82% of pixels differ)

Per frame: melonDS coverage 100% (full-screen background), replay coverage
40.8%.

Visual triptych analysis (traces diff PNGs, unit 43540) CONFIRMS two
renderer gaps — the transform/vertex/raster pipeline itself works (street
geometry, a character model and building shells come out in the right
screen positions):
1. **4x4-texel compressed textures render as the solid magenta stub** — the
   scene's large surfaces (wall/floor/awning polygons) are texture format 5;
   this is the single biggest color diff and matches the task's predicted
   gap list.  NOT yet implemented (needs gx_tex.c format-5 decode mirrored
   from melonDS TextureLookup + the palette-swap table at texpal 0x20000).
2. **Screen-covering background geometry is dropped** (black where melonDS
   shows street+sky): consistent with the near-plane-only clip + ±512 px
   coordinate clamp in gx_fifo.c fill_ras_vertex on very large quads; needs
   full frustum clipping or clamp-free raster bounds.

Also unmeasured-but-captured: fog, edge marking, shadow/polyID masking
(gxrender has no attr buffer) — quantified only after the two gaps above are
fixed, since they dominate the pixel diff today.

## 5. Gaps found / fixed / remaining

Fixed in this phase:
- capture fidelity: raw GX FIFO words alone are NOT enough — this game uses
  the direct command ports (04000440h+) for most single-param commands and
  splits multi-param commands across ports and FIFO words; the trace records
  both plus all render-relevant MMIO writes (two-pass latch model).
- gxrender API: gx_cmd_entry() for direct-port replay (src/gx_fifo.c).

Open (pending diff evidence on full-stream replay):
- 4x4-texel compressed textures (gxrender stub = magenta marker)
- fog (regs captured; renderer stub)
- edge marking, antialiasing, shadow volumes / polygon-ID masking
  (no attr/polyid buffer in gxrender's rasterizer — captured traces will
  quantify the pixel cost)
- near-clip precision and off-screen coordinate handling (see hypothesis 2)

## 6. Remaining work (next run)

1. Canonical capture with MELONDS_GXLOG_PRESS schedule
   (`14500:touch:128,70;14530:touch:up;16000:touch:128,70;16030:touch:up;
   17000:touch:128,70;17030:touch:up`), FRAMES≈48000, TEXFROM≈33000,
   TEXEVERY=1 over the 3D window.
2. Replay FROM UNIT 0 and diff the 3D window (root-cause §4 hypotheses).
3. Close the gaps the diffs reveal (4x4 tex / fog / edge / toon) or
   document precisely why not; re-diff to show deltas.
4. Update GX_COVERAGE.md to match measured reality.
