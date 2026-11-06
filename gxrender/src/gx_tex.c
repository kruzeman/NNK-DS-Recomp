/* gx_tex.c — DS texture VRAM decode.  Arithmetic follows melonDS
 * src/GPU3D_Soft.cpp TextureLookup(); palette entries are BGR555 with the
 * color-0 transparency rule supplied by TEXIMAGE_PARAM bit 29 ("alpha 0").
 *
 * Phase-5BY: the texel fetch lives in gx_tex.h (gx_tex_prep/gx_tex_fetch) so
 * the software rasteriser can hoist the TEXIMAGE_PARAM decode out of its fill
 * loop.  gx_tex_lookup() is that same pair, one call at a time.
 */
#include "gx_tex.h"
#include "gx_types.h"

int gx_tex_lookup(const GxTexMem *mem, uint32_t texparam, uint32_t texpal,
                  int32_t s, int32_t t, uint16_t *color, uint8_t *alpha)
{
    GxTexCtx c;

    gx_tex_prep(&c, mem, texparam, texpal);
    return gx_tex_fetch(&c, s, t, color, alpha);
}

int gx_tex_decode_rgba(const GxTexMem *mem, uint32_t texparam, uint32_t texpal,
                       uint8_t *rgba)
{
    int32_t width = 8 << ((texparam >> 20) & 0x7);
    int32_t height = 8 << ((texparam >> 23) & 0x7);
    int32_t x, y;
    /* force wrap so the full texture decodes even with clamp addressing */
    uint32_t tp = texparam | (1u << 16) | (1u << 17);

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint16_t color;
            uint8_t alpha, r, g, b;
            int vis = gx_tex_lookup(mem, tp, texpal,
                                    (int32_t)((x << 4) | 8), (int32_t)((y << 4) | 8),
                                    &color, &alpha);
            if (!vis) { r = g = b = 0; alpha = 0; }
            else {
                r = gx_c5_to_c6((uint8_t)(color & 0x1F));
                g = gx_c5_to_c6((uint8_t)((color >> 5) & 0x1F));
                b = gx_c5_to_c6((uint8_t)((color >> 10) & 0x1F));
                if (alpha == 0) r = g = b = 0;
            }
            rgba[(y * width + x) * 4 + 0] = gx_c6_to_c8(r);
            rgba[(y * width + x) * 4 + 1] = gx_c6_to_c8(g);
            rgba[(y * width + x) * 4 + 2] = gx_c6_to_c8(b);
            rgba[(y * width + x) * 4 + 3] = (uint8_t)((alpha * 255) / 31);
        }
    }
    return width;
}
