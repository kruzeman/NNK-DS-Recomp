/* gx_2d.h — DS 2D engine (BG layers, sprites, windows, color effects) and
 * screen compositor, for the Ni no Kuni DS static recompilation.
 *
 * Semantics follow the Nintendo DS hardware (GBATEK) and are validated
 * against the melonDS software 2D renderer (src/GPU2D_Soft.cpp DrawBG_x /
 * DrawSprite_x, src/GPU2D.cpp window/register latching, src/GPU_Soft.cpp
 * display path) the same way gx_raster.c mirrors the 3D rasterizer: the
 * scanline loop, latch timing (UpdateRegistersPreDraw/PostDraw), pixel flag
 * layout and color math are ported 1:1 so output is byte-comparable with
 * melonDS framebuffers.  Integer-only: identical runs produce identical
 * bytes.
 *
 * Input state is the per-unit GX2D state image dumped by
 * ref/melonds-2d-log.patch (f%05u.2d keyframe/delta container, layout
 * below), plus the melonDS 3D layer (f%05u.fb).  A single state image is
 * latched at display start (scanline 0): mid-frame register changes are not
 * represented (documented limitation, GX_TRACES.md).
 */
#ifndef GX_2D_H
#define GX_2D_H

#include <stdint.h>

/* ---- GX2D state image layout (little-endian; melonDS GPU_Soft.cpp) ----
 *   0x00000  256  engine A regs block
 *   0x00100  256  engine B regs block
 *   0x00200  512  global regs block
 *   0x00400 2048  palette RAM
 *   0x00C00 2048  OAM
 *   0x01400 512K  VRAMFlat_ABG       0x81400 128K VRAMFlat_BBG
 *   0xA1400 256K  VRAMFlat_AOBJ      0xE1400 128K VRAMFlat_BOBJ
 *   0x101400  32K VRAMFlat_ABGExtPal 0x109400 32K VRAMFlat_BBGExtPal
 *   0x111400   8K VRAMFlat_AOBJExtPal 0x113400 8K VRAMFlat_BOBJExtPal
 *   0x115400 512K VRAM_A..D raw banks
 * Engine regs block: +0x00 dispcnt u32, +0x04 bgcnt[4] u16, +0x0C bgxpos[4],
 * +0x14 bgypos[4], +0x1C bgxref[2] s32, +0x24 bgyref[2], +0x2C bgxrefreload[2],
 * +0x34 bgyrefreload[2], +0x3C bgrota[2] s16, +0x40 bgrotb[2], +0x44 bgrotc[2],
 * +0x48 bgrotd[2], +0x4C win0coords[4] u8, +0x50 win1coords[4], +0x54 wincnt[4],
 * +0x58 bgmosaicsize[2], +0x5A objmosaicsize[2], +0x5C blendcnt u16,
 * +0x5E blendalpha u16, +0x60 eva/evb/evy u8, +0x63 win0active, +0x64 win1active
 * (win0/1active = latched Y-active bits as of scanline 0, melonDS Win0Active).
 * Global block: +0x00 masterbright A/B u16, +0x04 screens_enabled,
 * +0x05 screen_swap, +0x06 engA_enabled, +0x07 engB_enabled, +0x08 VRAMCNT[9],
 * +0x11 VRAMSTAT, +0x12 capture_enable, +0x14 capturecnt u32,
 * +0x18 dispfifobuffer[256] u16.
 */
#define GX2D_STATE_SIZE 0x195400u

/* per-engine register set (decoded view of one 256-byte regs block) */
typedef struct {
    uint32_t dispcnt;
    uint16_t bgcnt[4];
    uint16_t bgxpos[4], bgypos[4];
    int32_t  bgxref[2], bgyref[2];        /* running ref as of scanline 0 */
    int32_t  bgxrefreload[2], bgyrefreload[2];
    int16_t  bgrota[2], bgrotb[2], bgrotc[2], bgrotd[2];
    uint8_t  win0coords[4], win1coords[4], wincnt[4];
    uint8_t  bgmosaic[2], objmosaic[2];   /* [0]=X size-1, [1]=Y size-1 */
    uint16_t blendcnt, blendalpha;
    uint8_t  eva, evb, evy;
    uint8_t  win0active, win1active;
} Gx2DEngine;

typedef struct {
    Gx2DEngine eng[2];
    uint16_t masterbright[2];
    uint8_t screens_enabled, screen_swap;
    uint8_t eng_enabled[2];
    uint8_t vramcnt[9];
    uint8_t vramstat;
    uint8_t capture_enable;
    uint32_t capturecnt;
    const uint8_t *palette;     /* 2048: engine A BG 0x000, A OBJ 0x200,
                                 * engine B BG 0x400, B OBJ 0x600 (melonDS) */
    const uint8_t *oam;         /* 2048: A 0x000, B 0x400 */
    const uint8_t *bgvram[2];   /* [0]=ABG 512K (mask 0x7FFFF),
                                 * [1]=BBG 128K (mask 0x1FFFF) */
    const uint8_t *objvram[2];  /* [0]=AOBJ 256K (mask 0x3FFFF),
                                 * [1]=BOBJ 128K (mask 0x1FFFF) */
    const uint8_t *bgextpal[2]; /* 32K each (4 slots x 16 palettes x 256 x 2) */
    const uint8_t *objextpal[2];/* 8K each */
    const uint8_t *vram_raw;    /* 512K: VRAM_A..D (VRAM display mode) */
    const uint16_t *dispfifo;   /* 256 entries (display FIFO mode) */
} Gx2DState;

/* Decode a raw GX2D state image (GX2D_STATE_SIZE bytes) into st.  All
 * pointers in st reference the image; the image must outlive the state. */
void gx2d_state_load(Gx2DState *st, const uint8_t *image);

/* Render one engine's composed screen for a whole frame: 256*192 u32 pixels,
 * post master-brightness 8-bit BGRA (melonDS Framebuffer layout, i.e. what
 * ExpandColor produces).  fb3d is the melonDS 3D layer for engine A BG0
 * (256*192 u32, r|g<<8|b<<16|a<<24 with 6-bit rgb and 5-bit alpha, the
 * f%05u.fb layout); NULL = transparent 3D layer.  engine: 0 = A, 1 = B. */
void gx2d_render_screen(const Gx2DState *st, int engine,
                        const uint32_t *fb3d, uint32_t *dst);

/* Render both engines and assemble the two screen buffers exactly like
 * melonDS SoftRenderer::DrawScanline (dstA/dstB routed through
 * screen_swap): out0/out1 correspond to melonDS Framebuffer[BackBuffer][0]
 * and [1], comparable byte-exact with f%05u.scr.  If screens_enabled is 0
 * both buffers are black (POWCNT1 LCD disable). */
void gx2d_render_frame(const Gx2DState *st, const uint32_t *fb3d,
                       uint32_t *out0, uint32_t *out1);

/* Binary PPM (P6) dump of a 256*192 u32 BGRA buffer (for tests/diffs). */
int gx2d_write_ppm(const char *path, const uint32_t *buf);

/* Phase-5BO: print the resolved 2D layer model for this frame -- per engine and
 * per BG: enable, text/affine/bitmap, priority, char/screen base, colour depth,
 * size, BGxCNT bit-13 ext-palette slot, the VRAM banks backing the engine's BG
 * window with absolute LCDC addresses, the first tilemap words, the scroll
 * registers, and the pixels the layer wrote in this frame (0 = it draws
 * nothing).  Armed from gx_display.c by NNK_GX_2DINFO=<frame>[,<frame>]. */
void gx2d_diag_dump(const Gx2DState *st, unsigned frame);

#endif /* GX_2D_H */
