# GX_DISPLAY.md — runtime display & input integration (Phase-3B4)

`gx_display` is the bridge between the recompiled guest runtime and the
gxrender graphics stack.  The runtime's IO layer forwards display-relevant
MMIO traffic into it and calls one function per emulated frame; guest input
(KEYINPUT / touch) is read back through the same IO surface.  Everything is
in `src/gx_display.h` (API) + `src/gx_display.c` (implementation).

Presentations modes:

* **windowed** — one SDL2 window with an OpenGL context shows BOTH DS
  screens (256x192 each), side-by-side or stacked, letterboxed, integer
  scaled.  SDL2 keyboard/mouse events drive the guest key/touch state.
* **headless** — no SDL at all; screens are kept in memory and can be
  dumped as PPM.  Byte-deterministic; used by `make check` and the replay
  tools.

The 3D layer is rendered by the software rasterizer (byte-exact vs melonDS
captures) or, windowed only, by the GL backend (`gl_3d` config flag;
approximate shading, see GX_COVERAGE.md "GL backend").  The 2D compositor
(`gx_2d`) always runs in software and produces both LCD outputs; the window
present itself is GL (two textured quads + letterbox).

## 1. Wiring recipe for the coder track

Link `libgxrender.a` + `gx_display.o` + `gx_gl.o` (SDL2 + GL), include
`gx_display.h`, and create one instance at boot:

```c
#include "gx_display.h"

static Gxd *gxd;

void display_init(void) {
    GxdConfig cfg = {
        .windowed = 1,            /* 0 for headless runs */
        .gl_3d    = 1,            /* 0 = software 3D (exact) */
        .scale    = 2,
        .layout   = GXD_LAYOUT_SIDE,
        .title    = "Ni no Kuni DS",
    };
    gxd = gxd_open(&cfg);         /* NULL -> run headless or fail fast */
}
```

In the IO dispatch (the flat-window MMIO hook from the IO-model phase),
forward the display ranges with the SAME width the guest used:

```c
void io_write(unsigned addr, unsigned val, int width) {
    switch (addr & 0xFF000000) {
    case 0x04000000: /* ... timers/DMA/IPC handling ... */
        if (display_reg(addr)) { gxd_io_write(gxd, addr, val, width); return; }
        /* ... */
    case 0x05000000: /* palette RAM */
    case 0x06000000: /* VRAM windows (0x06000000-0x0689FFFF) */
    case 0x07000000: /* OAM */
        gxd_io_write(gxd, addr, val, width);
        return;
    }
}

unsigned io_read(unsigned addr, int width) {
    if (addr == 0x04000130 || addr == 0x04000132 || addr == 0x0400024E)
        return gxd_io_read(gxd, addr, width);
    /* ... DISPSTAT/VCOUNT/timers stay in the runtime ... */
}
```

`display_reg(addr)` is true exactly for the ranges listed in
`gx_display.h` (2D engine A/B register blocks, VRAMCNT, POWCNT1, KEYCNT,
the 3D register block incl. DISP3DCNT, and the GX FIFO / direct ports —
the latter can also be routed to `gxd_gx_fifo_write` / `gxd_gx_cmd`
directly).  `gxd_io_write` ignores anything it does not own, so
over-forwarding is safe; check `gxd_unhandled_io()` in debug builds.

At each emulated frame boundary (the VBlank the runtime already paces on):

```c
void vblank(void) {
    gxd_frame(gxd);           /* render + composite + present + pump events */
    if (gxd_should_quit(gxd)) runtime_quit();
}
```

Input: nothing to wire for the keypad — the guest's reads of KEYINPUT
(0x04000130) return the live SDL keyboard state mapped to real DS key
bits (see below).  For touch, the runtime polls the state once per frame
(or on its existing touch-IPC path):

```c
int down, x, y;
gxd_get_touch(gxd, &down, &x, &y);   /* bottom-screen px 0..255, 0..191 */
```

and feeds `x, y` / release into whatever the game reads (the ARM7 firmware
TSC path on real hardware; melonDS `NDS::TouchScreen(x, y)` /
`NDS::ReleaseScreen()` semantics — coordinates are bottom-screen pixels).

## 2. Keyboard map (windowed)

| SDL key | DS key | KEYINPUT bit |
|---|---|---|
| X | A | 0 |
| Z | B | 1 |
| Right Shift | Select | 2 |
| Return | Start | 3 |
| Arrow Right/Left/Up/Down | D-pad | 4/5/6/7 |
| S | R | 8 |
| A | L | 9 |
| mouse button 1 on the sub screen | touch | — |

Tests and tools can bypass SDL with `gxd_set_keymask` / `gxd_key` /
`gxd_touch` (headless input injection).

## 3. Register semantics (what the implementation mirrors)

All register decode follows melonDS exactly (`src/GPU2D.cpp` Write8/16,
`src/GPU3D.cpp` Write8/16/32, `src/GPU.cpp` MapVRAM_A..I):

* 2D engine A/B register blocks -> the `Gx2DEngine` fields of the
  `gx_2d` state image (DISPCNT incl. the engine-B mask 0xC0B1FFF7, BGxCNT,
  BGxOFS, BGxPA-PD, BG2/3X-Y with the 28-bit sign extension and the
  ref+reload double write, WIN0/1H-V byte order (x1 high byte), WININ/
  WINOUT, MOSAIC, BLDCNT, BLDALPHA (EVA/EVB clamped to 16), BLDY,
  MASTER_BRIGHT, DISPCAPCNT).
* POWCNT1 (0x04000304): bit0 screens enabled, bit1 engine A, bit9 engine
  B, bit15 screen swap (melonDS `GPU::SetPowerCnt`).
* VRAM writes are window-decoded (BG/OBJ windows of both engines + the
  LCDC window) and routed into the raw banks through the current VRAMCNT
  mapping; the flat views the compositor samples (BG/OBJ/ext-palette VRAM
  of both engines, texture VRAM + texture palette) are rebuilt from the
  raw banks whenever VRAMCNT changes (and are always coherent at frame
  time).  Mapping tables mirror `GPU::MapVRAM_A..I`, including the
  bank-F/G "second half at +32K" quirk and the engine-B bank offsets.
* GX FIFO words and direct ports feed the same decoder as `gx_replay`
  (`gx_fifo.c`); the frame model (render env latched at SWAP_BUFFERS,
  retained polygon set replaced only when a flush submits polygons,
  Y-sorted opaque render order) is the melonDS model documented in
  `GX_TRACES.md` §3.
* Touch/keys: KEYINPUT is the real active-low register (0 = pressed);
  `GXD_KEY_*` are bit positions.

## 4. Frame flow

```
guest MMIO/GX traffic                VBlank
        |                              |
 gxd_io_write / gxd_gx_fifo_write      |
        |  (2D state image patched,    |
        |   polys retained at SWAP)    |
        v                              v
                     gxd_frame():
                       3D layer: clear -> Y-sort retained set -> raster
                                 -> edge/fog/AA post pass   (or GL)
                       2D: gx2d_render_frame(state, fb3d) -> screen0/1
                       present: two textured quads + letterbox (windowed)
                       pump SDL events -> key/touch state
```

The 3D layer handed to the compositor is in the melonDS `f%05u.fb` layout,
so headless output is directly comparable with capture dumps.

## 5. Determinism & testing

* Headless mode is byte-deterministic (integer pipeline everywhere).
  `make check` runs `tests/test_display.c`: keypad bits through
  `gxd_io_read` change with injected presses (headless), the register/
  VRAM write path produces the same compositor output as the bulk
  `gxd_load_2d_state` path for an equivalent state, and double runs are
  byte-identical.
* `tools/gx_present` replays captured streams (canon2d / canonB, see
  GX_TRACES.md) through this API — the same calls the runtime will make:

      build/gx_present <trace_dir> <out_dir> [--units A-B] [--window]
          [--layout side|stack] [--scale N] [--shots-every N]
          [--feed3d gx|fb] [--csv FILE] [--quiet]

  `--feed3d gx` renders the 3D layer from the captured `.gx` stream
  through the frame model (full pipeline); `--feed3d fb` uses the captured
  `.fb` layer (isolates the compositor + present path).  Headless runs
  print the same diff totals as `gx_2dreplay` and are compared against the
  established canonical numbers in the acceptance run.

## 6. Documented limitations

* Single-snapshot 2D state (same as the `gx_2d` model): mid-frame register
  /palette/VRAM writes are not represented; the state seen at the frame
  call is used for the whole frame.  This is the known unit-14444 class
  from GX_TRACES.md §7.
* Display FIFO mode (DISPCNT 3) and main-memory display (DISPCNT 1) are
  not fed by the live path (the display FIFO buffer and VRAM display mode
  work from the state image); Ni no Kuni DS does not use them in the
  captured windows.
* Main-RAM textures are not sampled (texture space is the 512K texture
  VRAM flat, matching melonDS `VRAMFlat_Texture`); the game's captured
  content resolves inside texture VRAM.
* GX hit-test commands (BOX/POS/VEC_TEST) stay stubs (GX_COVERAGE.md).
* The GL 3D path is an approximation (no fog/toon/edge/AA fidelity); the
  software 3D path is the byte-exact one and is what the headless
  canonical numbers are measured with.
