/* test_shade.c — DS pixel-shading unit tests (modulate / decal / toon /
 * highlight), hand-computed from the GPU3D_Soft.cpp formulas. */
#include <stdio.h>
#include <string.h>
#include "gx_raster.h"

static int failures;

static void check32(const char *what, int32_t got, int32_t want)
{
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %-36s got %d (0x%X) want %d (0x%X)\n",
                what, got, (uint32_t)got, want, (uint32_t)want);
    } else {
        fprintf(stderr, "ok   %-36s = %d (0x%X)\n", what, got, (uint32_t)got);
    }
}

static uint8_t vram[256];
static uint16_t pal[16];
static uint16_t toon[32];
static GxTexMem texmem;
static GxRasterEnv env;

static uint32_t tp_fmt3(void)
{
    /* 8x8, 16-color, addr 0 */
    return (0u) | (0u << 20) | (0u << 23) | (3u << 26);
}

int main(void)
{
    int i;
    GxPolygon poly;
    uint32_t pix;

    memset(&poly, 0, sizeof(poly));
    memset(vram, 0, sizeof(vram));
    texmem.vram = vram;
    texmem.vram_size = sizeof(vram);
    texmem.pal = pal;
    texmem.pal_count = 16;

    for (i = 0; i < 16; i++) pal[i] = 0;
    pal[1] = 0x7FFF; /* white */
    pal[2] = 0x001F; /* red */

    for (i = 0; i < 32; i++)
        toon[i] = (uint16_t)(i | (i << 5) | (i << 10)); /* gray ramp */

    env.tex = &texmem;
    env.toon = toon;
    env.tex_enable = 1;
    env.highlight = 0;
    env.alpha_blend = 1;
    env.alpha_ref = 0;

    vram[0] = 0x11; /* texel(0,0)=1 white, (1,0)=1 white */
    vram[1] = 0x22; /* texel(2,0)=2 red, (3,0)=2 red */

    /* S1: modulate (mode 0), white texel, white vertex color:
     * r = ((63+1)*(63+1)-1)>>6 = 63, a = ((31+1)*(31+1)-1)>>5 = 31 */
    poly.attr = (31u << 16); /* modulate, alpha 31 */
    poly.texparam = tp_fmt3();
    poly.texpal = 0;
    pix = gx_shade_pixel(&poly, 63, 63, 63, 0, 0, &env);
    check32("S1 modulate white r", pix & 0x3F, 63);
    check32("S1 modulate white g", (pix >> 8) & 0x3F, 63);
    check32("S1 modulate white b", (pix >> 16) & 0x3F, 63);
    check32("S1 modulate white a", pix >> 24, 31);

    /* S2: modulate, red texel (tcolor=0x001F -> tr: (0x001F<<1)&0x3E = 62,
     * +1 -> 63; tg = (0x001F>>4)&0x3E = 0), vertex c5 gray 16 -> c6 33:
     * r = ((63+1)*(33+1)-1)>>6 = (64*34-1)>>6 = 2175>>6 = 33
     * g = ((0+1)*(33+1)-1)>>6 = 33>>6 = 0
     * a = ((31+1)*(31+1)-1)>>5 = 31 */
    pix = gx_shade_pixel(&poly, 33, 33, 33, 2 * 16, 0, &env);
    check32("S2 modulate red r", pix & 0x3F, 33);
    check32("S2 modulate red g", (pix >> 8) & 0x3F, 0); /* (1*34-1)>>6 = 0 */
    check32("S2 modulate red a", pix >> 24, 31);

    /* S3: decal (mode 1), texel alpha 0 -> vertex color shows:
     * vertex c5=5 -> c6 = 11 */
    poly.attr = (1u << 4) | (31u << 16);
    vram[0] = 0x00; /* idx0 -> alpha 0 when bit29 set */
    poly.texparam = tp_fmt3() | (1u << 29);
    pix = gx_shade_pixel(&poly, 11, 11, 11, 0, 0, &env);
    check32("S3 decal talpha0 r", pix & 0x3F, 11);
    check32("S3 decal talpha0 a", pix >> 24, 31);

    /* S4: decal, full-alpha white texel replaces color */
    vram[0] = 0x11;
    poly.texparam = tp_fmt3();
    pix = gx_shade_pixel(&poly, 11, 11, 11, 0, 0, &env);
    check32("S4 decal opaque r", pix & 0x3F, 63);
    check32("S4 decal opaque a", pix >> 24, 31);

    /* S5: toon (mode 2, highlight off), texture disabled:
     * vr=62 -> toon[31] = 0x7FFF -> r=g=b = expand(31) = 63 */
    poly.attr = (2u << 4) | (31u << 16);
    env.tex_enable = 0;
    pix = gx_shade_pixel(&poly, 62, 62, 62, 0, 0, &env);
    check32("S5 toon r", pix & 0x3F, 63);
    check32("S5 toon g", (pix >> 8) & 0x3F, 63);
    check32("S5 toon a", pix >> 24, 31);

    /* S6: highlight (mode 2, highlight on), texture disabled:
     * r = vr + expand(toon[vr>>1]); vr=32 -> toon[16]=16 -> expand=33
     * r = 32+33 = 65 -> clamped 63 */
    env.highlight = 1;
    pix = gx_shade_pixel(&poly, 32, 32, 32, 0, 0, &env);
    check32("S6 highlight r", pix & 0x3F, 63);
    env.highlight = 0;

    /* S7: wireframe (alpha 0) forces final alpha to 31 */
    poly.attr = (31u << 16) & 0; /* alpha 0, modulate */
    pix = gx_shade_pixel(&poly, 33, 33, 33, 0, 0, &env);
    check32("S7 wireframe a", pix >> 24, 31);

    if (failures) {
        fprintf(stderr, "test_shade: %d FAILURES\n", failures);
        return 1;
    }
    fprintf(stderr, "test_shade: all checks passed\n");
    return 0;
}
