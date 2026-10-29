/* gx_gl3d.h — OpenGL 4.3-core render engine for the DS GX pipeline.
 *
 * Architecture: GL rasterizes the GxPolygon triangles (hardware path), but
 * every per-pixel DS semantic runs in the fragment shader against image
 * load/store buffers (top layer + AA second layer, color/depth/attr), a 1:1
 * port of the software rasterizer's per-pixel body (gx_raster.c
 * depth_pass / plot_translucent / alpha_blend / gx_shade_pixel) and its
 * frame post pass (edge marking / fog / antialiasing).  Integer math where
 * the software reference is integer; the only float-vs-integer deltas are
 * triangle interpolation (GL float perspective-correct vs the DS 8/9-bit
 * perspective factor) and rasterization fill rules (GL pixel-center rule vs
 * the melonDS span fill).
 *
 * Buffer layout matches GxSurface: packed pixels r | g<<8 | b<<16 | a<<24
 * with 6-bit rgb and 5-bit alpha; depth in the melonDS FinalZ scale; attr as
 * documented in gx_raster.h.  The AA second layer persists across frames and
 * is never cleared (melonDS ClearBuffers semantics).
 *
 * Textures: decoded on the CPU through the shared gx_tex.c lookup (exact
 * integer semantics incl. 4x4-compressed), uploaded as RGBA8UI (r6,g6,b6,a5)
 * and sampled with DS-exact wrap math in the shader (TEXIMAGE_PARAM bits
 * 16-19).
 *
 * Requires ARB_shader_image_load_store (GL 4.2; validated on Mesa llvmpipe
 * 4.5 under Xvfb and intended for Mesa GL 4.3+ hardware drivers).
 */
#ifndef GX_GL3D_H
#define GX_GL3D_H

#include <stdint.h>
#include "gx_raster.h"

typedef struct GxGl3d GxGl3d;

/* Open a hidden-window GL 4.3 core context + offscreen buffers.  present
 * scale > 0 also creates a visible window for gx_gl3d_present(). */
GxGl3d *gx_gl3d_open(int present_scale);
void gx_gl3d_close(GxGl3d *g);

const char *gx_gl3d_vendor(GxGl3d *g);
const char *gx_gl3d_renderer(GxGl3d *g);
const char *gx_gl3d_version(GxGl3d *g);

/* Upload the frame-clear buffers (layer 0).  The buffers are the same
 * packed layout the software path clears (see rx_clear_frame): color/depth/
 * attr, w*h entries each.  Layer 1 is never cleared (persists like
 * melonDS's second layer). */
void gx_gl3d_load_clear(GxGl3d *g, const uint32_t *color, const uint32_t *depth,
                        const uint32_t *attr);

/* Latched render env for the frame (texture mem, DISP3DCNT-derived flags,
 * toon table) + the post-pass env (edge table, fog).  Must be called before
 * drawing polygons of a frame; resets the per-frame texture cache. */
void gx_gl3d_set_env(GxGl3d *g, const GxRasterEnv *env, const GxPostEnv *post);

/* Draw one post-clip polygon (triangle fan of poly->nv screen-space
 * vertices) with the exact DS per-pixel semantics. */
void gx_gl3d_draw_polygon(GxGl3d *g, const GxPolygon *poly);

/* Frame post pass on the GPU: edge marking, fog (incl. second-layer fog),
 * antialiasing final blend (gx_post_pass ports). */
void gx_gl3d_post_pass(GxGl3d *g);

/* Read back the final color buffer in the packed .raw/.fb layout
 * (w*h u32 LE, 6-bit rgb + 5-bit alpha). */
void gx_gl3d_read_color(GxGl3d *g, uint32_t *out);

/* Blit the final color buffer to the window and swap (interactive use). */
void gx_gl3d_present(GxGl3d *g);

/* Debug/stats: polygons drawn, textures decoded, flat polygons (which the
 * GL path rasterizes as 1px quads), degenerate polygons skipped. */
typedef struct {
    long polys_drawn, textures_decoded, flat_polys, degenerate_polys;
} GxGl3dStats;
void gx_gl3d_get_stats(GxGl3d *g, GxGl3dStats *out);

#endif /* GX_GL3D_H */
