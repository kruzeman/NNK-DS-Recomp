/* gx_gl2d.h — DS 2D engine + compositor on the GPU (OpenGL 4.3 core).
 *
 * A per-pixel GLSL port of the validated software model (src/gx_2d.c,
 * itself a 1:1 port of melonDS GPU2D_Soft.cpp): the same candidate stack
 * semantics (draw_pixel push-down, priority-ordered draw, top-two pixels +
 * color_composite), the same texture/address arithmetic (text / affine /
 * extended / large BGs, normal + rotscale sprites incl. bitmap and OBJ
 * window sprites, mosaic latches, window x/y latches), all integer.
 *
 * Two passes per engine per frame:
 *   pass 1: sprite layer  -- per pixel, iterate the 128 OAM entries in the
 *           software's order and replicate draw_sprite_pixel's competition
 *           (objline winner + objwindow), writing an OBJ-line image
 *   pass 2: compositor    -- per pixel: window mask (closed-form x-latch),
 *           backdrop, BG3..BG0 / 3D layer at each priority with the sprite
 *           winner interleaved after the BGs of its priority (exact
 *           draw_scanline_bgobj order), then color_composite
 *
 * The per-scanline latch state (ref counters, mosaic rows, window y/x
 * latches) is computed on the CPU with the same PreDraw/PostDraw/
 * UpdateWindows model as the software path (gx_gl2d.c) and uploaded as a
 * per-line data texture.
 *
 * Output: one engine's composed screen, post display-mode dispatch, master
 * brightness and 6->8 bit expansion (the gx2d_render_screen tail), in the
 * melonDS Framebuffer layout (256*192 u32 BGRA8), byte-comparable with
 * f%05u.scr and with the software path's output.
 */
#ifndef GX_GL2D_H
#define GX_GL2D_H

#include <stdint.h>
#include "gx_2d.h"

typedef struct GxGl2d GxGl2d;

GxGl2d *gx_gl2d_open(void);
void gx_gl2d_close(GxGl2d *g);
const char *gx_gl2d_renderer(GxGl2d *g);
const char *gx_gl2d_version(GxGl2d *g);

/* Render one engine's screen through the GPU compositor.  fb3d is the
 * melonDS 3D layer (engine A BG0, may be NULL); dst receives 256*192 u32
 * BGRA8 like gx2d_render_screen().  Returns 0 on success. */
int gx_gl2d_render_screen(GxGl2d *g, const Gx2DState *st, int engine,
                          const uint32_t *fb3d, uint32_t *dst);

typedef struct {
    long frames, sprite_passes;
} GxGl2dStats;
void gx_gl2d_get_stats(GxGl2d *g, GxGl2dStats *out);

#endif /* GX_GL2D_H */
