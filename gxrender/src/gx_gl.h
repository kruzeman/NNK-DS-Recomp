/* gx_gl.h — OpenGL backend skeleton (SDL2 window + GL context).
 *
 * Consumes the SAME GxPolygon stream the software rasterizer gets (guest
 * GX commands decoded by gx_fifo.c), so a draw here proves guest-derived
 * geometry reaches hardware GL.  Fidelity is intentionally lower than the
 * software path (vertex color x texture modulation, approximated depth);
 * see GX_COVERAGE.md.
 */
#ifndef GX_GL_H
#define GX_GL_H

#include "gx_fifo.h"

typedef struct GxGl GxGl;

/* Open a window (w*scale x h*scale) + GL context.  NULL on failure. */
GxGl *gx_gl_open(int scale);
void gx_gl_close(GxGl *g);

/* GL_VENDOR / GL_RENDERER / GL_VERSION strings (for hardware-vs-software
 * rasterizer attribution in test logs). */
const char *gx_gl_vendor(GxGl *g);
const char *gx_gl_renderer(GxGl *g);
const char *gx_gl_version(GxGl *g);

void gx_gl_begin_frame(GxGl *g, uint32_t clear_color6);
void gx_gl_draw_polygon(GxGl *g, const GxPolygon *poly, const GxTexMem *tex);
void gx_gl_end_frame(GxGl *g);

/* Read back the GL framebuffer (row 0 = top) and write a binary PPM. */
int gx_gl_read_ppm(GxGl *g, const char *path);
/* Read back raw RGBA (row 0 = top); caller provides w*h*4 bytes. */
int gx_gl_read_rgba(GxGl *g, uint8_t *rgba);

#endif /* GX_GL_H */
