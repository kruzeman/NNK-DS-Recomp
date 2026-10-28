/* gx_tex.h — DS texture VRAM decode (texel formats used by LEVEL-5 titles).
 *
 * Format codes live in TEXIMAGE_PARAM bits 26-28 (GBATEK / melonDS
 * TextureLookup semantics):
 *   0 none, 1 A3I5, 2 4-color (2bpp paletted), 3 16-color (4bpp paletted),
 *   4 256-color (8bpp paletted), 5 4x4-texel compressed (STUB),
 *   6 A5I3, 7 direct 15bpp (color + 1-bit alpha).
 */
#ifndef GX_TEX_H
#define GX_TEX_H

#include <stdint.h>

typedef struct {
    const uint8_t *vram;    /* texture VRAM base */
    uint32_t vram_size;     /* bytes */
    const uint16_t *pal;    /* palette RAM (BGR555 entries) */
    uint32_t pal_count;     /* palette entries */
} GxTexMem;

/* Look up one texel.  s,t are 12.4 fixed-point texture coordinates (the
 * caller interpolates them; this function truncates to texels and applies
 * TEXIMAGE_PARAM wrapping).  Returns 1 and writes *color (BGR555) and
 * *alpha (0..31) if the texel is visible, 0 if it is fully transparent
 * (color is then irrelevant).  Mirrors melonDS SoftRenderer3D::TextureLookup
 * arithmetic; the 4x4-compressed format is a stub (see GX_COVERAGE.md). */
int gx_tex_lookup(const GxTexMem *mem, uint32_t texparam, uint32_t texpal,
                  int32_t s, int32_t t, uint16_t *color, uint8_t *alpha);

/* Decode a whole texture into an RGBA8888 buffer (row-major, width from
 * texparam).  Used by the GL backend skeleton and by tests.  Returns the
 * texel width, or 0 on bad parameters. */
int gx_tex_decode_rgba(const GxTexMem *mem, uint32_t texparam, uint32_t texpal,
                       uint8_t *rgba /* width*height*4 */);

#endif /* GX_TEX_H */
