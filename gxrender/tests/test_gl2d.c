/* test_gl2d.c -- differential test: GPU 2D compositor (gx_gl2d.c) vs the
 * software reference (gx_2d.c), byte-exact on synthetic states.
 *
 * Each case builds a synthetic GX2D state image (the melonDS capture
 * layout, see gx_2d.h), renders it through BOTH paths and compares every
 * pixel of both screen buffers.  The software path is the golden reference
 * (byte-validated vs melonDS), so GL must match it exactly: all 2D math is
 * integer and the GPU port is a per-pixel semantic clone.
 *
 * Build/run: build/test_gl2d  (needs a GL 4.3 context; run under
 * xvfb-run).  Exit 0 = all checks pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_2d.h"
#include "gx_gl2d.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
        failures++;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static uint8_t *img;

static void img_reset(void)
{
    memset(img, 0, GX2D_STATE_SIZE);
}

static void put16(uint32_t off, uint16_t v) { memcpy(img + off, &v, 2); }
static void put32(uint32_t off, uint32_t v) { memcpy(img + off, &v, 4); }

#define ENG(n)   ((n) ? 0x100u : 0x000u)
#define BGCNT(n, bg) (ENG(n) + 0x04u + (bg) * 2)
#define BGX(n, bg)   (ENG(n) + 0x0Cu + (bg) * 2)
#define BGY(n, bg)   (ENG(n) + 0x14u + (bg) * 2)
#define GBASE 0x200u
#define OAM_A 0xC00u
#define OAM_B 0x1000u

static void set_engine_defaults(int n)
{
    uint32_t b = ENG(n);
    put32(b + 0x00, 1u << 16);   /* display mode 1 (regular) */
    put16(b + 0x5C, 0);          /* BLDCNT */
    img[b + 0x60] = 16;          /* EVA */
    img[b + 0x61] = 0;           /* EVB */
    img[b + 0x62] = 0;           /* EVY */
}

static void set_global_defaults(void)
{
    img[GBASE + 0x04] = 1;       /* screens enabled */
    img[GBASE + 0x05] = 1;       /* screen swap: A -> buffer 0 */
    img[GBASE + 0x06] = 1;       /* engine A enabled */
    img[GBASE + 0x07] = 1;       /* engine B enabled */
}

static uint32_t sw0[256 * 192], sw1[256 * 192];
static uint32_t gl0[256 * 192], gl1[256 * 192];

static void render_both(GxGl2d *gl)
{
    Gx2DState st;
    gx2d_state_load(&st, img);
    gx2d_render_frame(&st, NULL, sw0, sw1);
    if (!st.screens_enabled) {
        for (int k = 0; k < 256 * 192; k++) { gl0[k] = 0xFF000000u; gl1[k] = 0xFF000000u; }
        return;
    }
    static uint32_t bufa[256 * 192], bufb[256 * 192];
    gx_gl2d_render_screen(gl, &st, 0, NULL, bufa);
    gx_gl2d_render_screen(gl, &st, 1, NULL, bufb);
    if (st.screen_swap) {
        memcpy(gl0, bufa, sizeof(bufa));
        memcpy(gl1, bufb, sizeof(bufb));
    } else {
        memcpy(gl0, bufb, sizeof(bufb));
        memcpy(gl1, bufa, sizeof(bufa));
    }
}

static int compare(const char *name, const uint32_t *a, const uint32_t *b)
{
    int diff = 0;
    for (int i = 0; i < 256 * 192; i++)
        if (a[i] != b[i])
            diff++;
    CHECK(diff == 0, "%s: %d differing pixels (first: %d got=%08X want=%08X)",
          name, diff,
          ({ int f = 0; while (f < 256 * 192 && a[f] == b[f]) f++; f; }),
          ({ int f = 0; while (f < 256 * 192 && a[f] == b[f]) f++; a[f]; }),
          ({ int f = 0; while (f < 256 * 192 && a[f] == b[f]) f++; b[f]; }));
    return diff;
}

/* ---- cases ---- */

/* C1: engine A text BG, 16-color (test_2d.c t1 shape) */
static void c1_text16(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;
    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8));
    memset(img + 0x1400, 0x11, 32);
    memset(img + 0x1400 + 32, 0x22, 32);
    img[0x1400 + 32] = 0x20;
    put16(0x1C00, 0x0000);
    put16(0x1C02, 0x0001);
    put16(0x1C04, 0x0001);
    put16(0x400 + 2, 0x001F);  /* pal 1 red */
    put16(0x400 + 4, 0x03E0);  /* pal 2 green */
    render_both(gl);
    compare("C1 buf0", gl0, sw0);
}

/* C2: engine B text BG 256-color with extended palette (title-screen
 * configuration: extpal on, BG1 256-color, slot 1) */
static void c2_engB_extpal(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(1);
    set_global_defaults();
    img[GBASE + 0x05] = 0; /* engine B -> buffer 1? swap=0: A->1, B->0 */
    img[GBASE + 0x05] = 1;
    /* engine B: dispcnt mode 0, BG1 on, extpal on, 1D OBJ mapping */
    put32(ENG(1) + 0x00, (1u << 16) | (1u << 9) | (1u << 30) | (1u << 12));
    put16(BGCNT(1, 1), 0x0087); /* prio 3, charbase 1, 256-color, screenbase 0 */
    /* tiles at charbase 1 = 0x4000 (engine B bgvram) */
    memset(img + 0x81400 + 0x4000, 0x05, 64); /* tile 0: index 5 */
    put16(0x81400 + 0, 0x0000); /* tilemap (0,0) = tile 0 */
    /* extpal slot 1 (bgextpal B at 0x109400): entry slot*8192 + color*2 */
    put16(0x109400 + 1 * 8192 + 5 * 2, 0x7C00); /* blue */
    render_both(gl);
    compare("C2 buf1", gl1, sw1);
}

/* C3: sprite over BG with priority interleave (engine A) */
static void c3_sprite(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;
    /* BG0 text 16-color full screen (color 1 = red) */
    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8));
    memset(img + 0x1400, 0x11, 32);
    put16(0x1C00, 0x0000);
    put16(0x400 + 2, 0x001F);
    /* sprite 0: 8x8 at (16,16), 16-color, prio 1, palette bank 1, tile 0 */
    put16(OAM_A + 0, (uint16_t)(16 & 0xFF));
    put16(OAM_A + 2, (uint16_t)(16 & 0x1FF));
    put16(OAM_A + 4, (uint16_t)(0x0000 | (1u << 12) | 0));
    /* OBJ VRAM A at 0xA1400: tile 0 = color 2 */
    memset(img + 0xA1400, 0x22, 32);
    /* OBJ palette: pal index = (bank<<4)|color -> image 0x600 + idx*2 */
    put16(0x600 + ((0x10 | 2) * 2), 0x03E0); /* bank 1, color 2 = green */
    render_both(gl);
    compare("C3 buf0", gl0, sw0);
}

/* C4: window clip + alpha blend + brightness on engine A */
static void c4_window_blend(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;
    put16(BGCNT(0, 0), 0x0100);
    put16(BGCNT(0, 1), 0x0201); /* BG1: prio 2, screenbase 2 */
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 9) | (1u << 8) | (1u << 13));
    memset(img + 0x1400, 0x11, 32);        /* BG0 tile0 color 1 */
    memset(img + 0x1400 + 32, 0x22, 32);   /* BG1 tile0 color 2 */
    put16(0x1C00, 0x0000); /* BG0 map 0x800 */
    put16(0x1400 + 0x1000, 0x0000); /* BG1 screenbase 2 = bgvram 0x1000 */
    put16(0x400 + 2, 0x001F);
    put16(0x400 + 4, 0x03E0);
    /* window 0: x 32..96, y 0..192; BG0 inside, nothing else */
    img[ENG(0) + 0x4C + 0] = 32;
    img[ENG(0) + 0x4C + 1] = 96;
    img[ENG(0) + 0x4C + 2] = 0;
    img[ENG(0) + 0x4C + 3] = 192 & 0xFF;
    img[ENG(0) + 0x54 + 0] = 0x01; /* win0: BG0 only */
    img[ENG(0) + 0x54 + 2] = 0x11; /* outside: BG0 + color effect */
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 9) | (1u << 8) | (1u << 13));
    /* brightness up outside windows */
    put16(ENG(0) + 0x5C, (uint16_t)((2u << 6) | 0x01)); /* mode 2 up, target BG0 */
    img[ENG(0) + 0x62] = 8;
    render_both(gl);
    compare("C4 buf0", gl0, sw0);
}

/* C5: mosaic + affine BG on engine A */
static void c5_affine_mosaic(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 9) | (1u << 8)); /* mode 1, BG2 */
    put16(BGCNT(0, 2), 0x0400 | (1u << 6) | (1u << 13)); /* affine, mosaic,
                                * screenbase 4 (map at bgvram 0x2000),
                                * overflow wrap */
    /* affine tiles: 8x8 tile 0 = color 1, tile 1 = color 2 */
    memset(img + 0x1400, 0x01, 64);
    memset(img + 0x1400 + 64, 0x01, 64);
    img[0x1400 + 64] = 0x02;
    /* map: rows of tile0/tile1 (map entries are 1 byte each for affine) */
    for (int i = 0; i < 64; i++)
        img[0x1400 + 0x2000 + i] = (uint8_t)((i & 1) ? 1 : 0);
    put16(0x400 + 2, 0x001F);
    put16(0x400 + 4, 0x03E0);
    /* rotation params: identity, ref (0,0) */
    put16(ENG(0) + 0x3C, 0x0100); /* rotA = 256 */
    put16(ENG(0) + 0x44, 0x0100); /* rotC = 256 */
    put32(ENG(0) + 0x1C, 0);      /* bgxref[0] */
    put32(ENG(0) + 0x24, 0);      /* bgyref[0] */
    put32(ENG(0) + 0x2C, 0);
    put32(ENG(0) + 0x34, 0);
    img[ENG(0) + 0x58] = 1; /* bg mosaic X size-1 = 1 */
    img[ENG(0) + 0x59] = 1;
    render_both(gl);
    compare("C5 buf0", gl0, sw0);
}

/* C6: engine-B mode 5 extended bitmap (direct color) -- the canon2d
 * unit-9020 register mix (dispcnt low bits 0x0435: mode 5 + OBJ 1D + BG2
 * only, bgcnt 0x4084 extended direct-color bitmap, size 1).  BG3 is
 * DISABLED but its ref registers hold a stale nonzero value: the software
 * latch model only advances the refs of enabled BGs, so bgxrefint[1] stays
 * stale while bgxrefint[0] advances per line.  A regression test for the
 * BG2-ref-latch bug (the shader used lat0.z/w -- BG3's latch -- for BG2).
 */
static void c6_mode5_bitmap(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(1);
    set_global_defaults();
    /* engine B dispcnt: display mode 1, mode 5, OBJ 1D, BG2 enable */
    put32(ENG(1) + 0x00, (1u << 16) | 5u | (1u << 4) | (1u << 10));
    put16(BGCNT(1, 2), 0x4084); /* extended bitmap, direct color, size 1 */
    /* BG2: identity rotation (A=256, D=256), refs (0,0) */
    put16(ENG(1) + 0x3C, 0x0100); /* bgrota[0] = 256 */
    put16(ENG(1) + 0x48, 0x0100); /* bgrotd[0] = 256 */
    put32(ENG(1) + 0x1C, 0);      /* bgxref[0] */
    put32(ENG(1) + 0x24, 0);      /* bgyref[0] */
    put32(ENG(1) + 0x2C, 0);      /* bgxrefreload[0] */
    put32(ENG(1) + 0x34, 0);      /* bgyrefreload[0] */
    /* BG3 refs stale + different (BG3 disabled keeps them frozen) */
    put32(ENG(1) + 0x20, 0x2000); /* bgxref[1] */
    put32(ENG(1) + 0x28, 0x1800); /* bgyref[1] */
    put32(ENG(1) + 0x30, 0x2000); /* bgxrefreload[1] */
    put32(ENG(1) + 0x38, 0x1800); /* bgyrefreload[1] */
    /* 256x256 15bpp bitmap at engine-B bgvram +0 (tilemapaddr = 0) */
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++)
        {
            uint16_t c = (uint16_t)((x & 31) | (((x >> 3) & 31) << 5) |
                                    (((y >> 3) & 31) << 10) | 0x8000);
            put16(0x81400u + (uint32_t)((y * 256 + x) * 2), c);
        }
    render_both(gl);
    compare("C6 buf1", gl1, sw1);
}

/* C7: engine-A 16-color text BG with palette bank >= 1 -- the canon2d
 * unit-21010 register class (bgcnt 0x2600: prio 0, screenbase 6, 16-color).
 * Regression test for the 16-color palette-bank offset bug (the shader
 * used bank*16 as a byte offset instead of bank*32). */
static void c7_text16_pbank(GxGl2d *gl)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;
    /* engine A dispcnt: display mode 1, mode 0, BG2 enable */
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 10));
    put16(BGCNT(0, 2), 0x2600); /* prio 0, 16-color, screenbase 6 (0x3000) */
    /* tileset 0: tile 0 = color 1, tile 1 = color 2 */
    memset(img + 0x1400, 0x11, 32);
    memset(img + 0x1400 + 32, 0x22, 32);
    /* tilemap at bgvram 0x3000: palette banks 2, 3, 15 */
    put16(0x1400 + 0x3000 + 0, (uint16_t)(0x2000 | 0));
    put16(0x1400 + 0x3000 + 2, (uint16_t)(0x3000 | 1));
    put16(0x1400 + 0x3000 + 4, (uint16_t)(0xF000 | 0));
    /* palette (engine A): bank 2 color 1 = red, bank 3 color 2 = green,
     * bank 15 color 1 = blue (with the melonDS bit-15 green quirk set) */
    put16(0x400 + ((2 * 16 + 1) * 2), 0x001F);
    put16(0x400 + ((3 * 16 + 2) * 2), 0x03E0);
    put16(0x400 + ((15 * 16 + 1) * 2), (uint16_t)(0x7C00 | 0x8000));
    render_both(gl);
    compare("C7 buf0", gl0, sw0);
}

int main(void)
{
    img = malloc(GX2D_STATE_SIZE);
    if (!img) return 1;

    GxGl2d *gl = gx_gl2d_open();
    if (!gl) {
        printf("SKIP: no GL context (run under xvfb-run)\n");
        free(img);
        return 1;
    }
    printf("test_gl2d: %s / %s\n", gx_gl2d_renderer(gl), gx_gl2d_version(gl));

    c1_text16(gl);
    c2_engB_extpal(gl);
    c3_sprite(gl);
    c4_window_blend(gl);
    c5_affine_mosaic(gl);
    c6_mode5_bitmap(gl);
    c7_text16_pbank(gl);

    printf("test_gl2d: %d checks, %d failures\n", checks, failures);
    gx_gl2d_close(gl);
    free(img);
    return failures ? 1 : 0;
}
