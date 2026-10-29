/* gx_raster.h — deterministic software rasterizer for DS GX polygons.
 *
 * The rasterizer mirrors melonDS's scanline model (GPU3D_Soft.h Slope/
 * Interpolator classes, GPU3D_Soft.cpp RenderPolygonScanline) rather than
 * a generic barycentric fill, because the DS interpolates attributes with
 * an 8/9-bit perspective factor and a per-edge one-row offset for X-major
 * edges (GPU3D_Soft.h:320-321) — visible 1-row texture shifts on wide
 * polygons.  Vertex positions are integer screen pixels (melonDS
 * FinalPosition), like the hardware.  Integer-only: identical runs produce
 * identical bytes.
 *
 * Frame post-processing (edge marking + fog + antialiasing) mirrors melonDS
 * GPU3D_Soft.cpp ScanlineFinalPass; see gx_post_pass() below.
 */
#ifndef GX_RASTER_H
#define GX_RASTER_H

#include <stdint.h>
#include "gx_types.h"
#include "gx_tex.h"

typedef struct {
    int32_t sx, sy;   /* screen position, INTEGER pixels, origin top-left
                       * (melonDS FinalPosition: posX&0x1FF, posY&0xFF) */
    int32_t depth;    /* melonDS FinalZ scale (GPU3D.cpp:1537-1547):
                       *   Z-buffer mode: clamp(((z*0x4000)/w + 0x3FFF) * 0x200,
                       *                       0, 0xFFFFFF) -- 24-bit depth
                       *   W-buffer mode: W truncated to its top 16 bits
                       *                  (per-polygon normalization) */
    int32_t wnorm;    /* melonDS FinalW: W normalized so the polygon's W
                       * values fit in 16 bits (GPU3D.cpp:1516-1535) */
    int32_t s, t;     /* texture coords, s16 (12.4 fixed) */
    int32_t cr, cg, cb; /* FinalColor (GPU3D.cpp:1463-1468): 5-bit color
                         * expanded to 9 bits as (c<<4)+0xF (0 stays 0) */
} GxRasVertex;

typedef struct {
    GxRasVertex v[10];
    int nv;           /* 3..10 vertices (post-clip polygon ring) */
    uint32_t attr;    /* POLYGON_ATTR */
    uint32_t texparam;
    uint32_t texpal;
    uint8_t wbuffer;     /* 1 = W-buffered (SWAP_BUFFERS param bit 1) */
    uint8_t facing_view; /* melonDS FacingView: clip-space (x,y,w) dot <= 0 */
} GxPolygon;

typedef struct {
    uint32_t *color;  /* w*h packed pixels: r | g<<8 | b<<16 | a<<24 */
    uint32_t *depth;  /* w*h, GxRasVertex.depth scale */
    uint32_t *attr;   /* w*h optional (may be NULL): melonDS attribute buffer
                       * (GPU3D_Soft.h:479-486):
                       *   bit0-3  edge flags (left/right/top/bottom)
                       *   bit4    backfacing flag
                       *   bit8-12 antialiasing coverage
                       *   bit15   fog enable
                       *   bit16-21 polygon ID for translucent pixels
                       *   bit22   translucent flag
                       *   bit24-29 polygon ID for opaque pixels */
    uint32_t *color2; /* optional AA second layer (may be NULL) */
    uint32_t *depth2;
    uint32_t *attr2;
    int w, h;
} GxSurface;

typedef struct {
    const GxTexMem *tex;
    const uint16_t *toon;  /* 32 BGR555 entries (palette 0..31) */
    int tex_enable;        /* DISP3DCNT bit 0 */
    int highlight;         /* DISP3DCNT bit 1 (1 = highlight, 0 = toon) */
    int alpha_blend;       /* DISP3DCNT bit 3 */
    int aa_enable;         /* DISP3DCNT bit 4 (antialiasing) */
    int edge_enable;       /* DISP3DCNT bit 5 (edge marking; affects the
                            * hardware edge fill rules, GPU3D_Soft.cpp:1023) */
    int alpha_ref;         /* pixels with alpha <= ref are discarded */
} GxRasterEnv;

/* Latched render env for the frame post pass (edge marking + fog + AA),
 * mirroring melonDS RenderDispCnt/RenderEdgeTable/RenderFog*
 * (GPU3D.cpp:2690-2701). */
typedef struct {
    const uint16_t *edge;  /* 8 BGR555 edge colors (EDGE_COLOR table) */
    uint32_t clear_attr;   /* clear pixel attr (attr-buffer value) */
    uint32_t clear_depth;  /* clear depth in GxRasVertex.depth scale */
    /* fog (only used when fog_enable) */
    uint32_t fog_color;    /* FOG_COLOR reg: BGR555 in bits 0-14, alpha 16-20 */
    uint32_t fog_offset;   /* FOG_OFFSET reg * 0x200 (melonDS latch) */
    uint8_t fog_density[34]; /* RenderFogDensityTable layout: [0]=reg[0],
                              * [1..32]=reg[0..31], [33]=reg[31] */
    int fog_shift;         /* DISP3DCNT bits 8-11 */
    int edge_enable;       /* DISP3DCNT bit 5 */
    int fog_enable;        /* DISP3DCNT bit 7 */
    int fog_alpha_only;    /* DISP3DCNT bit 6 (1: fog affects alpha only) */
    int aa_enable;         /* DISP3DCNT bit 4 */
} GxPostEnv;

void gx_surface_clear(GxSurface *sf, uint32_t color6, uint32_t depth,
                      uint32_t attr0);

/* Signed double-area of v0,v1,v2 in screen space (integer pixels).
 * > 0 = front-facing (clockwise on screen, DS convention), < 0 = back. */
int64_t gx_poly_area2(const GxPolygon *poly);

void gx_raster_polygon(GxSurface *sf, const GxPolygon *poly,
                       const GxRasterEnv *env);

uint32_t gx_shade_pixel(const GxPolygon *poly, uint8_t vr, uint8_t vg,
                        uint8_t vb, int32_t s, int32_t t,
                        const GxRasterEnv *env);

/* Frame post pass: edge marking, fog, then antialiasing (melonDS
 * ScanlineFinalPass, GPU3D_Soft.cpp:1453-1621).  Requires sf->attr to be
 * maintained by the rasterizer; color-only surfaces are left untouched. */
void gx_post_pass(GxSurface *sf, const GxPostEnv *env);

/* Fog density lookup (melonDS CalculateFogDensity, GPU3D_Soft.cpp:1414-1451).
 * Exported for unit tests. */
uint32_t gx_fog_density(const uint8_t density[34], uint32_t fog_offset,
                        int fog_shift, uint32_t z);

/* Binary PPM (P6) dump of the surface; 6-bit channels expanded to 8-bit. */
int gx_write_ppm(const char *path, const GxSurface *sf);

#endif /* GX_RASTER_H */
