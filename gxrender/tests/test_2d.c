/* test_2d.c -- unit checks for the DS 2D engine (gx_2d.c).
 *
 * Each check builds a synthetic GX2D state image in memory (the exact
 * layout the melonDS capture patch dumps, see gx_2d.h), renders a frame
 * through the public API and asserts exact output pixels (8-bit BGRA as
 * produced by melonDS ExpandColor: byte0=B byte1=G byte2=R byte3=0xFF;
 * 5-bit red 31 -> 6-bit 62 -> byte 251 = 0xFB).
 * Run: build/test_2d   (exit 0 = all checks pass)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_2d.h"

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

/* 8-bit output colors (B|G<<8|R<<16) */
#define RED    0xFFFB0000u  /* 5-bit 31 -> 62 -> 251 */
#define GREEN  0xFF00FB00u
#define BLUE   0xFF0000FBu
#define BLACK  0xFF000000u
#define WHITE  0xFFFFFFFFu

static uint8_t *img;

static void img_reset(void)
{
    memset(img, 0, GX2D_STATE_SIZE);
}

static void put16(uint32_t off, uint16_t v) { memcpy(img + off, &v, 2); }
static void put32(uint32_t off, uint32_t v) { memcpy(img + off, &v, 4); }

/* engine regs block base (state-image layout, NOT MMIO offsets):
 * +0x00 dispcnt, +0x04 bgcnt[4], +0x0C bgxpos[4], +0x14 bgypos[4],
 * +0x1C bgxref[2], +0x24 bgyref[2], +0x2C bgxrefreload[2],
 * +0x34 bgyrefreload[2], +0x3C bgrota[2], +0x40 bgrotb[2],
 * +0x44 bgrotc[2], +0x48 bgrotd[2], +0x4C win0coords[4],
 * +0x50 win1coords[4], +0x54 wincnt[4], +0x58 bgmosaic[2],
 * +0x5A objmosaic[2], +0x5C blendcnt, +0x5E blendalpha, +0x60 eva/evb/evy */
#define ENG(n)   ((n) ? 0x100u : 0x000u)
#define BGCNT(n, bg) (ENG(n) + 0x04u + (bg) * 2)
#define BGX(n, bg)   (ENG(n) + 0x0Cu + (bg) * 2)
#define BGY(n, bg)   (ENG(n) + 0x14u + (bg) * 2)
#define GBASE 0x200u
/* palette block at image 0x400: A BG 0x400, A OBJ 0x600, B BG 0x800,
 * B OBJ 0xA00; OBJ VRAM A at 0xA1400; BG VRAM A at 0x1400, B at 0x81400 */
#define PAL_A_BG(i) (0x400u + (i) * 2)
#define PAL_A_OBJ(i) (0x600u + (i) * 2)
#define PAL_B_BG(i) (0x800u + (i) * 2)

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

static uint32_t out0[256 * 192], out1[256 * 192];

static void render(void)
{
    Gx2DState st;
    gx2d_state_load(&st, img);
    gx2d_render_frame(&st, NULL, out0, out1);
}

/* screen_swap=1: engine A lands in out0, engine B in out1 */
static uint32_t px_a(int x, int y) { return out0[y * 256 + x]; }
static uint32_t px_b(int x, int y) { return out1[y * 256 + x]; }

static void check_px(const char *name, uint32_t got, uint32_t want)
{
    CHECK(got == want, "%s: got %08X want %08X", name, got, want);
}

/* ---- T1: text BG, 16-color, transparency -> backdrop ---- */
static void t1_text_bg_16(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    /* BG0 text: charbase 0, screenbase 1 (tilemap 0x800), 256x256 */
    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8)); /* mode 0, BG0 on */

    /* tile 0: all color 1; tile 1: color 2, but pixels 0,1 transparent */
    memset(img + 0x1400, 0x11, 32);
    memset(img + 0x1400 + 32, 0x22, 32);
    img[0x1400 + 32] = 0x20;
    /* entries: (0,0)=tile0 (1,0)=tile1 (2,0)=tile1 */
    put16(0x1C00, 0x0000);
    put16(0x1C02, 0x0001);
    put16(0x1C04, 0x0001);

    put16(PAL_A_BG(0), 0x0000);  /* backdrop black */
    put16(PAL_A_BG(1), 0x001F);  /* color 1 red */
    put16(PAL_A_BG(2), 0x03E0);  /* color 2 green */

    render();
    check_px("T1 tile0 red", px_a(0, 0), RED);
    check_px("T1 tile0 row7", px_a(0, 7), RED);
    check_px("T1 tile1 transparent", px_a(8, 0), BLACK);
    check_px("T1 tile1 green", px_a(9, 0), GREEN);
    check_px("T1 tile1b transparent", px_a(16, 0), BLACK);
    check_px("T1 tile1b green", px_a(17, 0), GREEN);
    check_px("T1 far entry", px_a(200, 100), RED); /* unwritten entry=tile0 */
}

/* ---- T2: text BG 256-color + flips + scroll ---- */
static void t2_text_bg_256_flip(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    put16(BGCNT(0, 0), 0x0180); /* 256-color, screenbase 1 */
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8));

    /* tile 0 (64 bytes): left half color 1, right half color 2 */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            img[0x1400 + y * 8 + x] = (x < 4) ? 1 : 2;

    put16(0x1C00, 0x0000);
    put16(0x1C02, (1 << 10));            /* (1,0) xflip */
    put16(0x1C00 + 64, (1 << 11));       /* (0,1) yflip */

    put16(PAL_A_BG(1), 0x001F);
    put16(PAL_A_BG(2), 0x03E0);

    render();
    check_px("T2 left", px_a(1, 1), RED);
    check_px("T2 right", px_a(6, 1), GREEN);
    check_px("T2 xflip left", px_a(9, 1), GREEN);
    check_px("T2 xflip right", px_a(14, 1), RED);
    check_px("T2 yflip row", px_a(1, 8), RED);
    /* scroll right 4 px: screen x=0 samples tile x=4 -> green */
    put16(BGX(0, 0), 4);
    render();
    check_px("T2 scrolled", px_a(0, 1), GREEN);
}

/* ---- T3: affine BG identity ---- */
static void t3_affine(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    /* mode 2: BG2 affine, screenbase 2 (tilemap 0x1000), size 128x128.
     * DISPCNT: BG mode in bits 0-2, display mode in bits 16-17 */
    put16(BGCNT(0, 2), 0x0200);
    put32(ENG(0) + 0x00, 2u | (1u << 16) | (1u << 10)); /* mode 2, BG2 on */

    /* affine tiles: 8x8, 1 byte/pixel, tile 0 = color 3 */
    memset(img + 0x1400, 3, 64);
    /* tile map 32x32 at 0x1400+0x1000: all tile 0 (zeroed) */

    /* PA=256 (1.0), PB=PC=0, PD=256, ref (0,0) */
    put16(ENG(0) + 0x3C, 256); /* BG2PA */
    put16(ENG(0) + 0x40, 0);   /* BG2PB */
    put16(ENG(0) + 0x44, 0);   /* BG2PC */
    put16(ENG(0) + 0x48, 256); /* BG2PD */
    put32(ENG(0) + 0x1C, 0);   /* BG2X ref */
    put32(ENG(0) + 0x24, 0);   /* BG2Y ref */
    put32(ENG(0) + 0x2C, 0);   /* BG2X reload */
    put32(ENG(0) + 0x34, 0);   /* BG2Y reload */

    put16(PAL_A_BG(3), 0x001F);

    render();
    check_px("T3 affine px", px_a(100, 50), RED);
    check_px("T3 affine edge", px_a(0, 0), RED);
}

/* ---- T4: sprites (normal, transparent, semitransparent) ---- */
static void t4_sprites(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    /* BG0 blue background + OBJ enabled */
    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8) | (1u << 12));
    memset(img + 0x1400, 0x11, 32);
    put16(0x1C00, 0);
    put16(PAL_A_BG(1), 0x7C00); /* blue */
    put16(PAL_A_BG(0), 0x0000);

    /* OBJ palette (A OBJ at 0x600): color 1 = red */
    put16(PAL_A_OBJ(1), 0x001F);

    /* OBJ tile 0 (16-color, 32 bytes at OBJ VRAM 0xA1400): all color 1,
     * pixels 0,1 transparent (byte 0 = 0x10) */
    memset(img + 0xA1400, 0x11, 32);
    img[0xA1400] = 0x10;

    /* sprite 0: normal 8x8 at (10, 20), tile 0, prio 0 */
    put16(0xC00 + 0, 20);      /* attr0: y */
    put16(0xC00 + 2, 10);      /* attr1: x */
    put16(0xC00 + 4, 0);       /* attr2: tile 0, prio 0, pal 0 */

    render();
    check_px("T4 bg behind", px_a(5, 25), BLUE);
    check_px("T4 sprite red", px_a(11, 21), RED);
    check_px("T4 sprite transparent", px_a(10, 20), BLUE);

    /* semitransparent sprite over blue BG, EVA=8 EVB=8 */
    put16(0xC00 + 0, (1 << 10) | 20);            /* mode 1 */
    put16(ENG(0) + 0x5C, (1 << 6) | 0x11 | 0x100); /* 1st BG0|OBJ, 2nd BG0 */
    img[ENG(0) + 0x60] = 8;
    img[ENG(0) + 0x61] = 8;
    render();
    /* red (62,0,0) over blue (0,0,62) at 8/8: per-channel
     * (62*8 + 0*8 + 8)>>4 = 31 -> expand 125 = 0x7D */
    uint32_t got = px_a(11, 21);
    CHECK(got == 0xFF7D007Du,
          "T4 semitrans: got %08X want 0xFF7D007D", got);
}

/* ---- T5: window 0 clipping ---- */
static void t5_window(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8) | (1u << 13)); /* WIN0 on */
    memset(img + 0x1400, 0x11, 32);
    put16(0x1C00, 0);
    put16(PAL_A_BG(1), 0x001F);
    put16(PAL_A_BG(0), 0x0000);

    /* win0coords: [0]=X1 left, [1]=X2 right, [2]=Y1 top, [3]=Y2 bottom */
    img[ENG(0) + 0x4C] = 10;
    img[ENG(0) + 0x4D] = 20;
    img[ENG(0) + 0x4E] = 5;
    img[ENG(0) + 0x4F] = 15;
    img[ENG(0) + 0x54] = 0x01; /* WIN0 inside: BG0 */
    img[ENG(0) + 0x56] = 0x00; /* outside: nothing */

    render();
    check_px("T5 inside", px_a(15, 10), RED);
    check_px("T5 outside", px_a(25, 10), BLACK);
    check_px("T5 below", px_a(15, 20), BLACK);
}

/* ---- T6: color effects (alpha blend, brighten, darken) ---- */
static void t6_effects(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    /* BG0 red prio 0, BG1 (palette bank 1) blue prio 1 */
    put16(BGCNT(0, 0), 0x0100);       /* screenbase 1 */
    put16(BGCNT(0, 1), 0x0201);       /* screenbase 2, prio 1 */
    put32(ENG(0) + 0x00, (1u << 16) | 0x300);
    memset(img + 0x1400, 0x11, 32);   /* shared tile 0: color 1 */
    put16(0x1C00, 0);                 /* BG0 entry */
    put16(0x2400, (1 << 12));         /* BG1 entry: palette bank 1 */
    put16(PAL_A_BG(1), 0x001F);       /* red */
    put16(PAL_A_BG(17), 0x7C00);      /* bank 1 color 1: blue */

    /* alpha blend: 1st BG0, 2nd BG1, EVA=8 EVB=8 */
    put16(ENG(0) + 0x5C, (1 << 6) | 0x01 | (0x02 << 8));
    img[ENG(0) + 0x60] = 8;
    img[ENG(0) + 0x61] = 8;
    render();
    /* red (62,0,0) over blue (0,0,62) at 8/8 -> (31,0,31) -> 0xFF7D007D */
    check_px("T6 blend", px_a(3, 3), 0xFF7D007Du);

    /* brighten: mode 2, EVY=8: r 62->63 (255), g/b 0->32 (130) */
    put16(ENG(0) + 0x5C, (2 << 6) | 0x01);
    img[ENG(0) + 0x62] = 8;
    render();
    check_px("T6 brighten", px_a(3, 3), 0xFFFF8282u);

    /* darken: mode 3, EVY=8 -> 62 - ((62*8+8)>>4) = 31 -> 4*31+(31>>4)=125 */
    put16(ENG(0) + 0x5C, (3 << 6) | 0x01);
    render();
    check_px("T6 darken", px_a(3, 3), 0xFF7D0000);
}

/* ---- T7: master brightness ---- */
static void t7_master_bright(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    put16(BGCNT(0, 0), 0x0100);
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8));
    memset(img + 0x1400, 0x11, 32);
    put16(0x1C00, 0);
    put16(PAL_A_BG(1), 0x001F);
    put16(PAL_A_BG(0), 0x0000);

    /* up factor 16: r 62->63 and g/b 0->63 -> white */
    put16(GBASE + 0x00, 0x4010);
    render();
    check_px("T7 up", px_a(3, 3), WHITE);

    /* down factor 16: 62 - ((62*16+15)>>4) = 0 */
    put16(GBASE + 0x00, 0x8010);
    render();
    check_px("T7 down", px_a(3, 3), BLACK);

    /* factor clamps at 16 */
    put16(GBASE + 0x00, 0x401F);
    render();
    check_px("T7 clamp", px_a(3, 3), WHITE);
}

/* ---- T8: engine B palette/backdrop + screen-off fill ---- */
static void t8_engine_b(void)
{
    img_reset();
    set_engine_defaults(1);
    set_engine_defaults(0);
    set_global_defaults();

    put32(ENG(0) + 0x00, 0); /* engine A: screen off */
    put16(BGCNT(1, 0), 0x0100);
    put32(ENG(1) + 0x00, (1u << 16) | (1u << 8));

    memset(img + 0x81400, 0x11, 32); /* BBG tile 0, color 1 */
    img[0x81400] = 0x10;             /* pixel 0 transparent */
    put16(0x81400 + 0x800, 0);       /* BBG entry */
    put16(PAL_B_BG(0), 0x03E0);      /* B backdrop green */
    put16(PAL_B_BG(1), 0x7C00);      /* B color 1 blue */

    render();
    check_px("T8 B tile", px_b(3, 3), BLUE);
    check_px("T8 B transparent", px_b(0, 0), GREEN);
    check_px("T8 B backdrop", px_b(200, 100), BLUE); /* entry=tile0 color1 */
    /* engine A screen off -> expand(0x3F3F3F) = white */
    check_px("T8 A screen off", px_a(3, 3), WHITE);
}

/* ---- T9: mosaic X on text BG ---- */
static void t9_mosaic(void)
{
    img_reset();
    set_engine_defaults(0);
    set_global_defaults();
    img[GBASE + 0x07] = 0;

    put16(BGCNT(0, 0), 0x0140); /* screenbase 1 + mosaic */
    put32(ENG(0) + 0x00, (1u << 16) | (1u << 8));

    /* tile 0: pixel x has color index x+1 (1..8) */
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
        {
            uint8_t *b = &img[0x1400 + y * 4 + x / 2];
            if (x & 1) *b |= (uint8_t)((x + 1) << 4);
            else       *b |= (uint8_t)(x + 1);
        }
    put16(0x1C00, 0);
    for (int i = 1; i <= 8; i++)
        put16(PAL_A_BG(i), (uint16_t)(i * 0x421));

    img[ENG(0) + 0x58] = 7; /* BG X mosaic size = 8 */

    render();
    uint32_t c0 = px_a(0, 0);
    for (int x = 0; x < 8; x++)
        CHECK(px_a(x, 0) == c0, "T9 mosaic x=%d got %08X want %08X",
              x, px_a(x, 0), c0);
}

/* ---- T10: determinism (3 in-process renders byte-identical) ---- */
static void t10_determinism(void)
{
    static uint32_t a[256 * 192], b[256 * 192], c[256 * 192];
    static uint32_t d[256 * 192], e[256 * 192], f[256 * 192];
    Gx2DState st;

    t4_sprites(); /* reuse T4's state */
    gx2d_state_load(&st, img);
    gx2d_render_frame(&st, NULL, a, d);
    gx2d_render_frame(&st, NULL, b, e);
    gx2d_render_frame(&st, NULL, c, f);
    CHECK(memcmp(a, b, sizeof(a)) == 0, "T10 buffer0 run1 != run2");
    CHECK(memcmp(b, c, sizeof(b)) == 0, "T10 buffer0 run2 != run3");
    CHECK(memcmp(d, e, sizeof(d)) == 0, "T10 buffer1 run1 != run2");
    CHECK(memcmp(e, f, sizeof(e)) == 0, "T10 buffer1 run2 != run3");
}

int main(void)
{
    img = calloc(1, GX2D_STATE_SIZE);
    if (!img)
    {
        printf("out of memory\n");
        return 1;
    }
    t1_text_bg_16();
    t2_text_bg_256_flip();
    t3_affine();
    t4_sprites();
    t5_window();
    t6_effects();
    t7_master_bright();
    t8_engine_b();
    t9_mosaic();
    t10_determinism();

    printf("test_2d: %d checks, %d failures\n", checks, failures);
    free(img);
    return failures ? 1 : 0;
}
