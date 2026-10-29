/* gx_display.h — runtime display & input bridge for the Ni no Kuni DS
 * static recompilation.
 *
 * This is the small C API the recompiled guest runtime calls from its IO
 * layer: GX FIFO traffic, 2D/display register writes, VRAM writes, and
 * keypad/touch state reads.  Everything else (timers, DISPSTAT/VCOUNT,
 * DMA, IPC) stays in the runtime; this module only owns the graphics and
 * input surface.
 *
 * Implementation (gx_display.c) is backed by the validated gxrender
 * engines: gx_fifo (3D GX decode + frame model), gx_raster (software 3D),
 * gx_gl (OpenGL 3D + SDL2 window present), gx_2d (2D compositor).
 * Headless mode is byte-deterministic and used by the tests; windowed mode
 * presents both DS screens in one SDL2/GL window and samples SDL input.
 *
 * Memory ownership: gxd_open allocates everything; all buffers stay valid
 * until gxd_close.  No globals; multiple instances are legal (tests).
 *
 * See GX_DISPLAY.md for the register map, the wiring recipe for the coder
 * track, and the documented limitations.
 */
#ifndef GX_DISPLAY_H
#define GX_DISPLAY_H

#include <stdint.h>

typedef struct Gxd Gxd;

/* ---- configuration ---- */

#define GXD_LAYOUT_SIDE  0   /* main screen left, sub screen right */
#define GXD_LAYOUT_STACK 1   /* main screen top, sub screen bottom */

typedef struct {
    int windowed;        /* 0 = headless (no SDL), 1 = SDL2 window */
    int gl_3d;           /* 1 = render the 3D layer through the GL backend
                          *     (windowed only; approximate shading, see
                          *     GX_COVERAGE.md), 0 = software 3D (exact,
                          *     byte-deterministic) */
    int scale;           /* window pixels per DS pixel (1..4) */
    int layout;          /* GXD_LAYOUT_SIDE or GXD_LAYOUT_STACK */
    int win_w, win_h;    /* initial window size in host pixels (0 = natural
                          * layout size; the window is resizable and the
                          * screens letterbox inside it) */
    const char *title;   /* window title (NULL = "gxrender") */
} GxdConfig;

/* Open a display instance.  Returns NULL on failure (e.g. windowed without
 * a usable SDL2/GL display).  cfg may be NULL for the headless default. */
Gxd *gxd_open(const GxdConfig *cfg);
void gxd_close(Gxd *g);

/* ---- guest IO surface (called from the runtime's IO layer) ---- */

/* MMIO write.  width = 1, 2 or 4 bytes; val is the little-endian value
 * (byte writes use the low 8 bits).  Handled ranges:
 *
 *   0x04000000-0x0400006F  2D engine A registers (DISPCNT, BGxCNT, BGxOFS,
 *                          BGxPA-PD, BGxX/Y, WIN, MOSAIC, BLDCNT/ALPHA/Y,
 *                          MASTER_BRIGHT, DISPCAPCNT)
 *   0x04001000-0x0400106F  2D engine B registers (same layout)
 *   0x04000240-0x04000248  VRAMCNT_A..I (bank mapping; rebuilds flat views)
 *   0x04000132              KEYCNT (stored, readback only)
 *   0x04000304              POWCNT1 (screen enable/swap, engine enables)
 *   0x04000060              DISP3DCNT
 *   0x04000330-0x040003FF  3D regs (edge colors, alpha ref, clear color/
 *                          depth, fog regs, fog table, toon table)
 *   0x04000400-0x0400043F  GXFIFO (same as gxd_gx_fifo_write)
 *   0x04000440-0x040005CB  GX direct command ports (cmd = (addr&0x1FC)>>2,
 *                          melonDS GPU3D::Write32)
 *   0x04000600              GXSTAT (stack-reset side effect; value ignored)
 *   0x04000610              ZeroDotWLimit
 *   0x05000000-0x050007FF  palette RAM
 *   0x06000000-0x061FFFFF  VRAM window: engine A BG (routed per VRAMCNT)
 *   0x06200000-0x0621FFFF  VRAM window: engine B BG
 *   0x06400000-0x0643FFFF  VRAM window: engine A OBJ
 *   0x06600000-0x0661FFFF  VRAM window: engine B OBJ
 *   0x06800000-0x0689FFFF  VRAM LCDC window (raw banks A..I)
 *   0x07000000-0x070007FF  OAM
 *
 * Unhandled addresses are ignored and counted (gxd_unhandled_io). */
void gxd_io_write(Gxd *g, uint32_t addr, uint32_t val, int width);

/* MMIO read.  Handled:
 *   0x04000130  KEYINPUT  (active-low DS key bits, see GXD_KEY_*; bits
 *               10-15 read 0; bits 16-17 mirror the lid/debug mask bits)
 *   0x04000132  KEYCNT    (last written value)
 *   0x0400024E  VRAMSTAT  (bit0 = VRAM_C mapped to ARM7, bit1 = VRAM_D)
 * Everything else returns 0. */
uint32_t gxd_io_read(Gxd *g, uint32_t addr, int width);

/* ---- GX stream (also reachable via io_write; see above) ---- */

/* One raw GXFIFO word (up to 4 packed command bytes + parameter words). */
void gxd_gx_fifo_write(Gxd *g, uint32_t word);
/* One direct command-port entry (cmd + one parameter). */
void gxd_gx_cmd(Gxd *g, uint8_t cmd, uint32_t param);

/* ---- frame boundary ---- */

/* Render + present one frame.  The runtime calls this once per emulated
 * frame (VBlank) after that frame's GX traffic.  Semantics follow the
 * melonDS frame model: SWAP_BUFFERS latches the render env and swaps the
 * retained polygon set; this call renders the retained set (clear, Y-sort,
 * raster, edge/fog/AA post pass), composites both 2D screens from the live
 * register/VRAM state, presents them (windowed) and pumps SDL events. */
void gxd_frame(Gxd *g);

/* Advance the frame model one unit (SWAP_BUFFERS env latch + retained-set
 * swap) WITHOUT rendering.  Replay tools feed long captures and render
 * only selected units (mid-stream skips do not accumulate state); the
 * runtime never needs this. */
void gxd_advance(Gxd *g);

/* Nonzero once the user asked to close the window (or SDL quit). */
int gxd_should_quit(const Gxd *g);

/* ---- input state (real DS registers) ---- */

/* KEYINPUT bit positions (value is ACTIVE-LOW in the register: 0 = pressed). */
#define GXD_KEY_A      0
#define GXD_KEY_B      1
#define GXD_KEY_SELECT 2
#define GXD_KEY_START  3
#define GXD_KEY_RIGHT  4
#define GXD_KEY_LEFT   5
#define GXD_KEY_UP     6
#define GXD_KEY_DOWN   7
#define GXD_KEY_R      8
#define GXD_KEY_L      9
#define GXD_KEY_LID    10  /* bit 16 of KEYINPUT (melonDS SetKeyMask) */

/* Set the whole key state from a KEYINPUT-shaped active-low mask (as used
 * by the melonDS capture schedules: 0 = pressed). */
void gxd_set_keymask(Gxd *g, uint32_t active_low_mask);
/* Set one key: keybit = GXD_KEY_*, down = 1 presses it. */
void gxd_key(Gxd *g, int keybit, int down);
/* Current KEYINPUT-shaped mask (active-low). */
uint32_t gxd_keymask(const Gxd *g);

/* Touch state (bottom screen).  x,y are bottom-screen pixels (0..255,
 * 0..191), like melonDS NDS::TouchScreen(x, y); down=0 releases.  In
 * windowed mode SDL mouse events drive this automatically. */
void gxd_touch(Gxd *g, int down, int x, int y);
void gxd_get_touch(const Gxd *g, int *down, int *x, int *y);

/* ---- output access (tests, tools, headless screenshots) ---- */

/* Last composed screen, 256*192 u32, 8-bit BGRA (melonDS Framebuffer
 * layout).  which: 0 = main LCD (top), 1 = sub LCD (bottom). */
const uint32_t *gxd_screen(const Gxd *g, int which);
/* Last 3D layer, 256*192 u32 in the f%05u.fb layout (6-bit rgb + 5-bit
 * alpha). */
const uint32_t *gxd_fb3d(const Gxd *g);

/* Binary PPM (P6) of one composed screen.  0 on success. */
int gxd_write_screen_ppm(const Gxd *g, int which, const char *path);
/* Binary PPM of the whole window (both screens incl. letterbox, windowed
 * mode only).  0 on success. */
int gxd_write_window_ppm(Gxd *g, const char *path);

/* ---- trace / test hooks (not needed by the runtime) ---- */

/* Bulk-load a full GX2D state image (GX2D_STATE_SIZE bytes, the layout of
 * the melonDS f%05u.2d keyframe payload) as the live 2D state.  Subsequent
 * gxd_io_write calls patch it like live register writes would. */
void gxd_load_2d_state(Gxd *g, const uint8_t *image);
/* Bulk-load texture memory snapshots (melonDS VRAMFlat_Texture /
 * VRAMFlat_TexPal, the f%05u.texvram / f%05u.texpal layout).  Live texture
 * content is maintained from VRAM writes; this overrides it for replay. */
void gxd_load_tex(Gxd *g, const uint8_t *texvram, uint32_t vram_size,
                  const uint16_t *texpal, uint32_t pal_count);
/* Override the 3D layer fed to the compositor with a captured f%05u.fb
 * buffer (256*192 u32, fb layout); NULL restores the rendered layer. */
void gxd_load_fb3d(Gxd *g, const uint32_t *fb);

/* Diagnostics. */
uint32_t gxd_unhandled_io(const Gxd *g);
unsigned gxd_frame_count(const Gxd *g);

#endif /* GX_DISPLAY_H */
