/* gx_tex.c — DS texture VRAM decode.  Arithmetic follows melonDS
 * src/GPU3D_Soft.cpp TextureLookup(); palette entries are BGR555 with the
 * color-0 transparency rule supplied by TEXIMAGE_PARAM bit 29 ("alpha 0").
 */
#include "gx_tex.h"
#include "gx_types.h"

static uint16_t pal_fetch(const GxTexMem *mem, uint32_t byte_off)
{
    uint32_t idx = byte_off >> 1;
    if (idx >= mem->pal_count) return 0;
    return mem->pal[idx];
}

int gx_tex_lookup(const GxTexMem *mem, uint32_t texparam, uint32_t texpal,
                  int32_t s, int32_t t, uint16_t *color, uint8_t *alpha)
{
    uint32_t vramaddr = (texparam & 0xFFFF) << 3;
    int32_t width = 8 << ((texparam >> 20) & 0x7);
    int32_t height = 8 << ((texparam >> 23) & 0x7);
    uint8_t alpha0, pixel;
    uint32_t fmt = (texparam >> 26) & 0x7;

    s >>= 4; /* 12.4 -> texel */
    t >>= 4;

    /* wrapping: bit16 S repeat, bit18 S flip, bit17 T repeat, bit19 T flip */
    if (texparam & (1u << 16)) {
        if (texparam & (1u << 18)) {
            if (s & width) s = (width - 1) - (s & (width - 1));
            else           s = (s & (width - 1));
        } else {
            s &= width - 1;
        }
    } else {
        if (s < 0) s = 0;
        else if (s >= width) s = width - 1;
    }
    if (texparam & (1u << 17)) {
        if (texparam & (1u << 19)) {
            if (t & height) t = (height - 1) - (t & (height - 1));
            else            t = (t & (height - 1));
        } else {
            t &= height - 1;
        }
    } else {
        if (t < 0) t = 0;
        else if (t >= height) t = height - 1;
    }

    alpha0 = (texparam & (1u << 29)) ? 0 : 31;

    switch (fmt) {
    case 0: /* no texture */
        return 0;

    case 1: /* A3I5 */
        vramaddr += (uint32_t)(t * width + s);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        *color = pal_fetch(mem, (texpal << 4) + ((uint32_t)(pixel & 0x1F) << 1));
        /* 3-bit alpha, replicated up (melonDS formula) */
        *alpha = (uint8_t)(((pixel >> 3) & 0x1C) + (pixel >> 6));
        return 1;

    case 2: /* 4-color (2bpp) */
        vramaddr += (uint32_t)((t * width + s) >> 2);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = (uint8_t)(mem->vram[vramaddr] >> ((s & 0x3) << 1));
        pixel &= 0x3;
        *color = pal_fetch(mem, (texpal << 3) + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? alpha0 : 31;
        return 1;

    case 3: /* 16-color (4bpp) */
        vramaddr += (uint32_t)((t * width + s) >> 1);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        if (s & 0x1) pixel >>= 4;
        else         pixel &= 0xF;
        *color = pal_fetch(mem, (texpal << 4) + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? alpha0 : 31;
        return 1;

    case 4: /* 256-color (8bpp) */
        vramaddr += (uint32_t)(t * width + s);
        if (vramaddr >= mem->vram_size) return 0;
        pixel = mem->vram[vramaddr];
        *color = pal_fetch(mem, (texpal << 4) + ((uint32_t)pixel << 1));
        *alpha = (pixel == 0) ? alpha0 : 31;
        return 1;

    case 5: { /* 4x4-texel compressed (melonDS TextureLookup case 5).
               * Layout: 4 bytes of 2-bit texel indices per 4x4 block in
               * slot 0/2/3, one 16-bit palette-info word per block in the
               * companion "slot 1" table region (addr/2 + 0x20000 base). */
        uint32_t blockaddr, slot1addr, palbase, paloffset, palinfo;
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
        palbase = texpal << 4;
        mode = (uint8_t)(palinfo >> 14);

        switch (val & 0x3) {
        case 0:
            *color = pal_fetch(mem, palbase + paloffset);
            *alpha = 31;
            return 1;
        case 1:
            *color = pal_fetch(mem, palbase + paloffset + 2);
            *alpha = 31;
            return 1;
        case 2:
            if (mode == 1 || mode == 3) {
                color0 = pal_fetch(mem, palbase + paloffset);
                color1 = pal_fetch(mem, palbase + paloffset + 2);
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
                *color = pal_fetch(mem, palbase + paloffset + 4);
            }
            *alpha = 31;
            return 1;
        case 3:
            if (mode == 2) {
                *color = pal_fetch(mem, palbase + paloffset + 6);
                *alpha = 31;
            } else if (mode == 3) {
                color0 = pal_fetch(mem, palbase + paloffset);
                color1 = pal_fetch(mem, palbase + paloffset + 2);
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
        *color = pal_fetch(mem, (texpal << 4) + ((uint32_t)(pixel & 0x7) << 1));
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
