/* gx_raster.h — deterministic software rasterizer for DS GX polygons.
 *
 * Scanline-free barycentric fill with the top-left rule, perspective-correct
 * attribute interpolation (1/w), Z- or W-buffer depth test, 1-bit alpha test
 * and the DS shading modes (modulate / decal / toon / highlight) ported from
 * melonDS GPU3D_Soft.cpp RenderPixel arithmetic.  Integer-only: identical
 * runs produce identical bytes.
 */
#ifndef GX_RASTER_H
#define GX_RASTER_H

#include <stdint.h>
#include "gx_types.h"
#include "gx_tex.h"

typedef struct {
    int32_t sx, sy;   /* screen position, 1/16 px, origin top-left */
    int32_t depth;    /* Z mode: z/w in 12.12;  W mode: clip w */
    int32_t invw;     /* (1<<30)/clip_w */
    int32_t s, t;     /* texture coords, 12.4 fixed */
    uint8_t r, g, b;  /* vertex color, 5-bit per channel */
} GxRasVertex;

typedef struct {
    GxRasVertex v[4];
    int nv;           /* 3 or 4 (quad = v0-v1-v2 + v0-v2-v3) */
    uint32_t attr;    /* POLYGON_ATTR */
    uint32_t texparam;
    uint32_t texpal;
} GxPolygon;

typedef struct {
    uint32_t *color;  /* w*h packed pixels: r | g<<8 | b<<16 | a<<24 */
    uint32_t *depth;  /* w*h */
    int w, h;
} GxSurface;

typedef struct {
    const GxTexMem *tex;
    const uint16_t *toon;  /* 32 BGR555 entries (palette 0..31) */
    int tex_enable;        /* DISP3DCNT bit 0 */
    int highlight;         /* DISP3DCNT bit 1 (1 = highlight, 0 = toon) */
    int alpha_blend;       /* DISP3DCNT bit 3 */
    int alpha_ref;         /* pixels with alpha <= ref are discarded */
} GxRasterEnv;

void gx_surface_clear(GxSurface *sf, uint32_t color6, uint32_t depth);

/* Signed double-area of v0,v1,v2 in screen space (1/16 px units).
 * > 0 = front-facing (clockwise on screen, DS convention), < 0 = back. */
int64_t gx_poly_area2(const GxPolygon *poly);

void gx_raster_polygon(GxSurface *sf, const GxPolygon *poly,
                       const GxRasterEnv *env);

uint32_t gx_shade_pixel(const GxPolygon *poly, uint8_t vr, uint8_t vg,
                        uint8_t vb, int32_t s, int32_t t,
                        const GxRasterEnv *env);

/* Binary PPM (P6) dump of the surface; 6-bit channels expanded to 8-bit. */
int gx_write_ppm(const char *path, const GxSurface *sf);

#endif /* GX_RASTER_H */
