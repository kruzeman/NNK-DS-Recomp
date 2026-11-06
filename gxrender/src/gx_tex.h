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

/* Phase-5BY: per-pixel texel fetch with the TEXIMAGE_PARAM decode hoisted out
 * of the fill loop.  The software rasteriser fetches ~100 000 texels per frame
 * and re-deriving width/height/format/wrap flags for every one of them was a
 * third of the texel cost.  gx_tex_lookup() in gx_tex.c is defined as
 * prep + fetch, so the hoisted path and the reference path cannot diverge. */
typedef struct {
    const GxTexMem *mem;
    uint32_t vramaddr;      /* (texparam & 0xFFFF) << 3 */
    int32_t width, height;  /* texels */
    uint32_t fmt;           /* (texparam >> 26) & 7 */
    uint32_t palbase;       /* texpal << 4 */
    int srepeat, sflip, trepeat, tflip;
    uint8_t alpha0;         /* alpha of texel value 0 (TEXIMAGE_PARAM bit 29) */
    /* Phase-5BY: two flags that turn the per-texel branches into arithmetic
     * and let the per-texel palette bounds check go away. */
    int wrap_fast;          /* repeat S/T, no flip: mask the coords */
    int pal_safe;           /* paletted index provably inside palette RAM */
} GxTexCtx;

static inline uint16_t gx_tex_pal(const GxTexMem *mem, uint32_t byte_off)
{
    uint32_t idx = byte_off >> 1;
    if (idx >= mem->pal_count) return 0;
    return mem->pal[idx];
}

static inline void gx_tex_prep(GxTexCtx *c, const GxTexMem *mem,
                               uint32_t texparam, uint32_t texpal)
{
    c->mem = mem;
    c->vramaddr = (texparam & 0xFFFFu) << 3;
    c->width = 8 << ((texparam >> 20) & 0x7);
    c->height = 8 << ((texparam >> 23) & 0x7);
    c->fmt = (texparam >> 26) & 0x7;
    c->palbase = texpal << 4;
    c->srepeat = (int)((texparam >> 16) & 1);
    c->sflip = (int)((texparam >> 18) & 1);
    c->trepeat = (int)((texparam >> 17) & 1);
    c->tflip = (int)((texparam >> 19) & 1);
    c->alpha0 = (texparam & (1u << 29)) ? 0 : 31;
    c->wrap_fast = c->srepeat && c->trepeat && !c->sflip && !c->tflip;
    c->pal_safe = ((uint32_t)(c->palbase >> 1) + 256u <= mem->pal_count);
}

/* Same arithmetic as the reference gx_tex_lookup(); returns 1 and writes
 * *color (BGR555) / *alpha (0..31) when the texel is visible, 0 when it is
 * fully transparent (color/alpha are then irrelevant). */
static inline int gx_tex_fetch(const GxTexCtx *c, int32_t s, int32_t t,
                               uint16_t *color, uint8_t *alpha)
{
    const GxTexMem *mem = c->mem;
    int32_t width = c->width, height = c->height;
    uint32_t vramaddr = c->vramaddr;
    uint8_t pixel;

    s >>= 4; /* 12.4 -> texel */
    t >>= 4;

    /* wrapping: bit16 S repeat, bit18 S flip, bit17 T repeat, bit19 T flip.
     * The common DS texture state (repeat both, no flip) is two masks. */
    if (c->wrap_fast) {
        s &= width - 1;
        t &= height - 1;
    } else {
    if (c->srepeat) {
        if (c->sflip) {
            if (s & width) s = (width - 1) - (s & (width - 1));
            else           s = (s & (width - 1));
        } else {
            s &= width - 1;
        }
    } else {
        if (s < 0) s = 0;
        else if (s >= width) s = width - 1;
    }
    if (c->trepeat) {
        if (c->tflip) {
            if (t & height) t = (height - 1) - (t & (height - 1));
            else            t = (t & (height - 1));
        } else {
            t &= height - 1;
        }
    } else {
        if (t < 0) t = 0;
        else if (t >= height) t = height - 1;
    }
    }

    switch (c->fmt) {
    case 0: /* no texture */
        return 0;

    case 1: /* A3I5 */
        vramaddr += (uint32_t)(t * width + s);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        *color = c->pal_safe ? mem->pal[(c->palbase >> 1) + (uint32_t)(pixel & 0x1F)] : gx_tex_pal(mem, c->palbase + ((uint32_t)(pixel & 0x1F) << 1));
        /* 3-bit alpha, replicated up (melonDS formula) */
        *alpha = (uint8_t)(((pixel >> 3) & 0x1C) + (pixel >> 6));
        return 1;

    case 2: /* 4-color (2bpp) */
        vramaddr += (uint32_t)((t * width + s) >> 2);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = (uint8_t)(mem->vram[vramaddr] >> ((s & 0x3) << 1));
        pixel &= 0x3;
        *color = c->pal_safe ? mem->pal[(c->palbase >> 1) + (uint32_t)pixel] : gx_tex_pal(mem, c->palbase + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? c->alpha0 : 31;
        return 1;

    case 3: /* 16-color (4bpp) */
        vramaddr += (uint32_t)((t * width + s) >> 1);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        if (s & 0x1) pixel >>= 4;
        else         pixel &= 0xF;
        *color = c->pal_safe ? mem->pal[(c->palbase >> 1) + (uint32_t)pixel] : gx_tex_pal(mem, c->palbase + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? c->alpha0 : 31;
        return 1;

    case 4: /* 256-color (8bpp) */
        vramaddr += (uint32_t)(t * width + s);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        *color = c->pal_safe ? mem->pal[(c->palbase >> 1) + (uint32_t)pixel] : gx_tex_pal(mem, c->palbase + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? c->alpha0 : 31;
        return 1;

    case 5: { /* 4x4-texel compressed (melonDS TextureLookup case 5) */
        uint32_t blockaddr, slot1addr, paloffset, palinfo;
        uint32_t r, g, b;
        uint8_t val, mode;
        uint16_t color0, color1;

        blockaddr = vramaddr + (uint32_t)(t & 0x3FC) * (uint32_t)(width >> 2) +
                    (uint32_t)(s & 0x3FC) + (uint32_t)(t & 0x3);
        blockaddr &= 0x7FFFF; /* address wraps after slot 3 (melonDS) */

        slot1addr = 0x20000 + ((blockaddr & 0x1FFFC) >> 1);
        if (blockaddr >= 0x40000)
            slot1addr += 0x10000;

        if (blockaddr >= 0x20000 && blockaddr < 0x40000) {
            val = 0; /* reading slot 1 for texels always reads 0 (melonDS) */
        } else {
            if (blockaddr >= mem->vram_size) return 0;
            val = (uint8_t)(mem->vram[blockaddr] >> (2 * (s & 0x3)));
        }

        if (slot1addr + 1 >= mem->vram_size) return 0;
        palinfo = (uint32_t)mem->vram[slot1addr] |
                  ((uint32_t)mem->vram[slot1addr + 1] << 8);
        paloffset = (palinfo & 0x3FFF) << 2;
        mode = (uint8_t)(palinfo >> 14);

        switch (val & 0x3) {
        case 0:
            *color = gx_tex_pal(mem, c->palbase + paloffset);
            *alpha = 31;
            return 1;
        case 1:
            *color = gx_tex_pal(mem, c->palbase + paloffset + 2);
            *alpha = 31;
            return 1;
        case 2:
            if (mode == 1 || mode == 3) {
                color0 = gx_tex_pal(mem, c->palbase + paloffset);
                color1 = gx_tex_pal(mem, c->palbase + paloffset + 2);
                if (mode == 1) {
                    r = ((color0 & 0x001F) + (color1 & 0x001F)) >> 1;
                    g = (((color0 & 0x03E0) + (color1 & 0x03E0)) >> 1) & 0x03E0;
                    b = (((color0 & 0x7C00) + (color1 & 0x7C00)) >> 1) & 0x7C00;
                } else {
                    r = ((color0 & 0x001F) * 5 + (color1 & 0x001F) * 3) >> 3;
                    g = (((color0 & 0x03E0) * 5 + (color1 & 0x03E0) * 3) >> 3) & 0x03E0;
                    b = (((color0 & 0x7C00) * 5 + (color1 & 0x7C00) * 3) >> 3) & 0x7C00;
                }
                *color = (uint16_t)(r | g | b);
            } else {
                *color = gx_tex_pal(mem, c->palbase + paloffset + 4);
            }
            *alpha = 31;
            return 1;
        case 3:
            if (mode == 2) {
                *color = gx_tex_pal(mem, c->palbase + paloffset + 6);
                *alpha = 31;
            } else if (mode == 3) {
                color0 = gx_tex_pal(mem, c->palbase + paloffset);
                color1 = gx_tex_pal(mem, c->palbase + paloffset + 2);
                r = ((color0 & 0x001F) * 3 + (color1 & 0x001F) * 5) >> 3;
                g = (((color0 & 0x03E0) * 3 + (color1 & 0x03E0) * 5) >> 3) & 0x03E0;
                b = (((color0 & 0x7C00) * 3 + (color1 & 0x7C00) * 5) >> 3) & 0x7C00;
                *color = (uint16_t)(r | g | b);
                *alpha = 31;
            } else {
                *color = 0;
                *alpha = 0;
            }
            return 1;
        }
        return 0;
    }

    case 6: /* A5I3 */
        vramaddr += (uint32_t)(t * width + s);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        *color = c->pal_safe ? mem->pal[(c->palbase >> 1) + (uint32_t)(pixel & 0x7)] : gx_tex_pal(mem, c->palbase + ((uint32_t)(pixel & 0x7) << 1));
        *alpha = (uint8_t)(pixel >> 3);
        return 1;

    case 7: /* direct 15bpp */
        vramaddr += (uint32_t)((t * width + s) << 1);
        if (vramaddr + 1 >= mem->vram_size) return 0;
        *color = (uint16_t)(mem->vram[vramaddr] | (mem->vram[vramaddr + 1] << 8));
        *alpha = (*color & 0x8000) ? 31 : 0;
        return 1;
    }
    return 0;
}

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
