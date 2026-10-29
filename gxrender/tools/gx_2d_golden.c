/* gx_2d_golden.c -- deterministic 2D composite scene for the golden test.
 *
 * Builds a fixed synthetic GX2D state (text BGs 16/256-color, sprites incl.
 * semitransparent, alpha blending, window, master brightness) and renders
 * one frame with gx_2d.  Output must be byte-identical across runs and
 * matches the committed tests/golden/scene2d.ppm.
 *
 * Usage: gx_2d_golden --out <path.ppm>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_2d.h"

static uint8_t *img;

static void put16(uint32_t off, uint16_t v) { memcpy(img + off, &v, 2); }
static void put32(uint32_t off, uint32_t v) { memcpy(img + off, &v, 4); }

int main(int argc, char **argv)
{
    const char *out = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    if (!out)
    {
        fprintf(stderr, "usage: gx_2d_golden --out <path.ppm>\n");
        return 1;
    }

    img = calloc(1, GX2D_STATE_SIZE);
    if (!img) return 1;

    /* --- engine A (state-image layout: bgcnt at +0x04, bgx at +0x0C,
     * bgy at +0x14, win0coords at +0x4C = X1,X2,Y1,Y2) --- */
    put32(0x000, (1u << 16) | (1u << 12) | 0x300 | (1u << 13)); /* mode 0,
        OBJ on, BG0+BG1, WIN0 on */
    /* BG0: 256-color, charbase 0, screenbase 1 */
    put16(0x004, 0x0180);
    /* BG1: 16-color, charbase 0, screenbase 2 (0x1000), prio 1 */
    put16(0x006, 0x0201);
    put16(0x00C, 4); /* BG0 scroll x */
    put16(0x014, 2); /* BG0 scroll y */
    /* WIN0: x 32..224 y 16..176, BG0/BG1/OBJ inside, BG0 outside */
    img[0x04C] = 32; img[0x04D] = 224;
    img[0x04E] = 16; img[0x04F] = 176;
    img[0x054] = 0x17; img[0x056] = 0x01;
    /* BLDCNT: alpha 1st BG0|OBJ, 2nd BG1|OBJ */
    put16(0x05C, (1 << 6) | 0x11 | (0x12 << 8));
    img[0x060] = 10; img[0x061] = 6; img[0x062] = 0;

    /* BG0 tiles: 256-color ramp tile 0 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            img[0x1400 + y * 8 + x] = (uint8_t)(x + y);
    /* BG0 screen (base 1): tile 0 everywhere, alternating flips */
    for (int i = 0; i < 32 * 32; i++)
        put16(0x1400 + 0x800 + i * 2,
              (uint16_t)(((i / 32) & 1) ? (1 << 10) : 0));
    /* BG1 tiles: 16-color stripes tile 1 (32 bytes) */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x += 2)
            img[0x1400 + 32 + y * 4 + x / 2] = (uint8_t)(0x21);
    /* BG1 screen (base 2): tile 1, palette bank 2 */
    for (int i = 0; i < 32 * 32; i++)
        put16(0x1400 + 0x1000 + i * 2, (uint16_t)(1 | (2 << 12)));

    /* palettes */
    for (int i = 0; i < 256; i++)
        put16(0x400 + i * 2, (uint16_t)((i * 37) & 0x7FFF));
    for (int i = 0; i < 16; i++)
        put16(0x400 + 32 * 2 + i * 2, (uint16_t)(0x2A5 + i * 0x411));

    /* OBJ: two sprites, 16-color (OAM at 0xC00, OBJ VRAM at 0xA1400) */
    /* sprite 0: 16x16 at (60, 40), tile 0, prio 0 */
    put16(0xC00 + 0, 40);
    put16(0xC00 + 2, 60);
    put16(0xC00 + 4, 0);
    /* sprite 1: 8x8 at (100, 90), tile 16, semitransparent, prio 1 */
    put16(0xC00 + 8, (1u << 10) | 90);
    put16(0xC00 + 10, 100);
    put16(0xC00 + 12, 16 | (1 << 10) /* prio 1 */);
    /* OBJ tiles at OBJ VRAM (0xA1400): 16-color */
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x += 2)
            img[0xA1400 + y * 8 + x / 2] = (uint8_t)(0x31);
    for (int i = 0; i < 32; i++)
        img[0xA1400 + 16 * 16 + i] = 0x44;
    /* OBJ palettes (A OBJ at 0x600) */
    for (int i = 0; i < 16; i++)
        put16(0x600 + i * 2, (uint16_t)(0x0C1F + i * 0x217));

    /* engine B: solid color screen via text BG */
    put32(0x100, (1u << 16) | (1u << 8));
    put16(0x104, 0x0100);
    memset(img + 0x81400, 0x11, 32);
    put16(0x81400 + 0x800, 0);
    put16(0x400 + 0x400, 0x1234); /* B backdrop */
    put16(0x400 + 0x402, 0x2D6B); /* B color 1 */

    /* master brightness */
    put16(0x200, 0x4002);        /* A up factor 2 */
    put16(0x202, 0x0000);        /* B none */

    img[0x204] = 1; /* screens enabled */
    img[0x205] = 1; /* screen swap */
    img[0x206] = 1;
    img[0x207] = 1;

    Gx2DState st;
    gx2d_state_load(&st, img);
    static uint32_t s0[256 * 192], s1[256 * 192];
    gx2d_render_frame(&st, NULL, s0, s1);

    /* write both screens side by side (512x192) as the golden image */
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    fprintf(f, "P6\n512 192\n255\n");
    for (int y = 0; y < 192; y++)
    {
        for (int half = 0; half < 2; half++)
        {
            const uint32_t *buf = half ? s1 : s0;
            for (int x = 0; x < 256; x++)
            {
                uint32_t v = buf[y * 256 + x];
                uint8_t rgb[3] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                                   (uint8_t)((v >> 16) & 0xFF) };
                fwrite(rgb, 1, 3, f);
            }
        }
    }
    fclose(f);
    free(img);
    return 0;
}
