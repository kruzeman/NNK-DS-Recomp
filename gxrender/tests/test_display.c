/* test_display.c — headless tests for the gx_display runtime bridge.
 *
 * Covers (acceptance items in brackets):
 *   T1  keypad bits read through gxd_io_read(0x04000130) change with
 *       injected key presses (headless)                    [input demo]
 *   T2  KEYCNT + VRAMSTAT io_read/io_write round-trip
 *   T3  touch state via gxd_touch/gxd_get_touch
 *   T4  live register/VRAM write path produces the same compositor output
 *       as the bulk gxd_load_2d_state path for an equivalent state
 *       (small synthetic scene: one BG tile + palette)
 *   T5  determinism: two identical headless runs are byte-identical
 *   T6  key injection through the public setters == direct keymask
 *
 * Run: build/test_display   (exit 0 = all pass)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_display.h"
#include "gx_2d.h"

#define FB_PIXELS (256 * 192)

static int fails;

#define CHECK(cond, ...) do { \
        if (cond) { printf("ok   " __VA_ARGS__); printf("\n"); } \
        else { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } \
    } while (0)

/* ---- synthetic scene via LIVE io writes ---------------------------- */

static void build_scene_live(Gxd *g)
{
    int i;

    /* POWCNT1: screens + engine A + engine B + 3D render + screen swap
     * (bit15 routes engine A to out0 = top, melonDS DrawScanline) */
    gxd_io_write(g, 0x04000304, 0x8207, 2);
    /* VRAMCNT_A: enable, MST=1 (engine A BG) -- BEFORE the VRAM writes */
    gxd_io_write(g, 0x04000240, 0x81, 1);
    /* engine A DISPCNT: display mode 1 (bits 16-17), BG1 on (bit 9) */
    gxd_io_write(g, 0x04000000, 0x00010200, 4);
    /* engine B DISPCNT: display mode 1 (mode 0 = white screen) */
    gxd_io_write(g, 0x04001000, 0x00010000, 4);
    /* BG1CNT: char base 0, screen base block 4 (0x4000), 16-color */
    gxd_io_write(g, 0x0400000A, 0x0400, 2);
    /* palette entry 1 = red (BGR555) */
    gxd_io_write(g, 0x05000002, 0x001F, 2);
    /* tile 0: all pixels color index 1 (4bpp: 32 bytes of 0x11) */
    for (i = 0; i < 32; i += 4)
        gxd_io_write(g, 0x06000000 + i, 0x11111111, 4);
    /* screen entry (0,0) = tile 0, (1,0) = tile 1 (empty -> backdrop);
     * screen base block 4 = 4 * 0x800 = 0x2000 */
    gxd_io_write(g, 0x06002000, 0x0000, 2);
    gxd_io_write(g, 0x06002002, 0x0001, 2);
}

/* ---- same scene as a raw GX2D state image (bulk path) -------------- */

static void build_scene_bulk(uint8_t *img)
{
    int i;

    memset(img, 0, GX2D_STATE_SIZE);
    /* engine A block: dispcnt u32 = 0x00010200, bgcnt[1] u16 = 0x0400 */
    img[0x00] = 0x00; img[0x01] = 0x02; img[0x02] = 0x01; img[0x03] = 0x00;
    img[0x06] = 0x00; img[0x07] = 0x04;   /* bgcnt[1] = 0x0400 */
    /* engine B block: dispcnt u32 = 0x00010000 (display mode 1) */
    img[0x100] = 0x00; img[0x101] = 0x00; img[0x102] = 0x01; img[0x103] = 0x00;
    /* global: screens/engines on, swap on, VRAMCNT_A = 0x81 */
    img[0x204] = 1;
    img[0x205] = 1;
    img[0x206] = 1;
    img[0x207] = 1;
    img[0x208] = 0x81;
    /* palette entry 1 = red */
    img[0x402] = 0x1F; img[0x403] = 0x00;
    /* VRAMFlat_ABG: tile 0 chars at +0x1400, screen entries at +0x3400
     * (screen base block 4 = 4 * 0x800) */
    for (i = 0; i < 32; i++)
        img[0x1400 + i] = 0x11;
    img[0x3400] = 0x00; img[0x3401] = 0x00;   /* entry 0 = tile 0 */
    img[0x3402] = 0x01; img[0x3403] = 0x00;   /* entry 1 = tile 1 (empty) */
}

static uint32_t *run_live(void)
{
    Gxd *g = gxd_open(NULL);
    uint32_t *out;
    if (!g) { printf("FAIL gxd_open(headless)\n"); fails++; return NULL; }
    build_scene_live(g);
    gxd_frame(g);
    out = malloc(FB_PIXELS * 2 * 4);
    memcpy(out, gxd_screen(g, 0), FB_PIXELS * 4);
    memcpy(out + FB_PIXELS, gxd_screen(g, 1), FB_PIXELS * 4);
    gxd_close(g);
    return out;
}

static uint32_t *run_bulk(void)
{
    static uint8_t img[GX2D_STATE_SIZE];
    Gxd *g = gxd_open(NULL);
    uint32_t *out;
    if (!g) { printf("FAIL gxd_open(headless)\n"); fails++; return NULL; }
    build_scene_bulk(img);
    gxd_load_2d_state(g, img);
    gxd_frame(g);
    out = malloc(FB_PIXELS * 2 * 4);
    memcpy(out, gxd_screen(g, 0), FB_PIXELS * 4);
    memcpy(out + FB_PIXELS, gxd_screen(g, 1), FB_PIXELS * 4);
    gxd_close(g);
    return out;
}

int main(void)
{
    Gxd *g;
    uint32_t ki;
    int down, x, y;

    /* ---- T1: keypad bits through io_read (headless input) ---- */
    g = gxd_open(NULL);
    CHECK(g != NULL, "T1 gxd_open(headless)");
    if (!g) return 1;

    ki = gxd_io_read(g, 0x04000130, 2);
    CHECK(ki == 0x3FF, "T1 KEYINPUT idle = 0x3FF (got 0x%03X)", ki);

    gxd_key(g, GXD_KEY_A, 1);
    ki = gxd_io_read(g, 0x04000130, 2);
    CHECK(ki == 0x3FE, "T1 press A -> KEYINPUT 0x3FE (got 0x%03X)", ki);

    gxd_key(g, GXD_KEY_START, 1);
    gxd_key(g, GXD_KEY_UP, 1);
    ki = gxd_io_read(g, 0x04000130, 2);
    CHECK(ki == (0x3FF & ~(1u << 0) & ~(1u << 3) & ~(1u << 6)),
          "T1 press A+START+UP -> 0x%03X (got 0x%03X)",
          0x3FF & ~1u & ~8u & ~0x40u, ki);

    gxd_key(g, GXD_KEY_A, 0);
    gxd_key(g, GXD_KEY_START, 0);
    gxd_key(g, GXD_KEY_UP, 0);
    ki = gxd_io_read(g, 0x04000130, 2);
    CHECK(ki == 0x3FF, "T1 release all -> 0x3FF (got 0x%03X)", ki);

    /* melonDS SetKeyMask schedule semantics (active-low mask) */
    gxd_set_keymask(g, 0x3FB);              /* Select pressed */
    ki = gxd_io_read(g, 0x04000130, 2);
    CHECK(ki == 0x3FB, "T1 set_keymask(0x3FB) -> KEYINPUT 0x3FB (got 0x%03X)", ki);
    gxd_set_keymask(g, 0x3FF);

    /* 32-bit read mirrors the lid/debug bits (16-17) */
    gxd_set_keymask(g, 0x3FF | (1u << 10)); /* lid bit */
    ki = gxd_io_read(g, 0x04000130, 4);
    CHECK(ki == 0x000103FF, "T1 lid bit -> KEYINPUT32 0x000103FF (got 0x%08X)", ki);
    gxd_set_keymask(g, 0x3FF);

    /* ---- T2: KEYCNT / VRAMSTAT ---- */
    gxd_io_write(g, 0x04000132, 0xC001, 2);
    ki = gxd_io_read(g, 0x04000132, 2);
    CHECK(ki == 0xC001, "T2 KEYCNT round-trip (got 0x%04X)", ki);
    gxd_io_write(g, 0x04000242, 0x82, 1);   /* VRAMCNT_C: enable, MST=2 (ARM7) */
    ki = gxd_io_read(g, 0x0400024E, 1);
    CHECK(ki == 0x01, "T2 VRAMSTAT bit0 after VRAM_C->ARM7 (got 0x%02X)", ki);
    gxd_io_write(g, 0x04000242, 0x00, 1);
    ki = gxd_io_read(g, 0x0400024E, 1);
    CHECK(ki == 0x00, "T2 VRAMSTAT back to 0 (got 0x%02X)", ki);

    /* ---- T3: touch ---- */
    gxd_touch(g, 1, 128, 70);
    gxd_get_touch(g, &down, &x, &y);
    CHECK(down == 1 && x == 128 && y == 70,
          "T3 touch (1,128,70) -> down=%d x=%d y=%d", down, x, y);
    gxd_touch(g, 0, 128, 70);
    gxd_get_touch(g, &down, &x, &y);
    CHECK(down == 0, "T3 touch release -> down=%d", down);
    gxd_close(g);

    /* ---- T4: live register path == bulk image path ---- */
    {
        uint32_t *live = run_live();
        uint32_t *bulk = run_bulk();
        if (live && bulk) {
            if (memcmp(live, bulk, FB_PIXELS * 2 * 4) != 0) {
                int d, shown = 0;
                for (d = 0; d < FB_PIXELS * 2 && shown < 8; d++) {
                    if (live[d] != bulk[d]) {
                        printf("     diff px %d (x=%d y=%d scr=%d): "
                               "live=0x%08X bulk=0x%08X\n",
                               d, d % 256, (d % FB_PIXELS) / 256,
                               d / FB_PIXELS, live[d], bulk[d]);
                        shown++;
                    }
                }
            }
            CHECK(memcmp(live, bulk, FB_PIXELS * 2 * 4) == 0,
                  "T4 live io path == bulk state path (both screens)");
            /* the synthetic tile must be visible: pixel (0,0) of the top
             * screen is palette entry 1 = red (BGRA 8-bit layout: red in
             * the third byte, 5-bit 31 expands to 0xFB) */
            CHECK((live[0] & 0x00FFFFFF) == 0x00FB0000,
                  "T4 tile pixel (0,0) is red (got 0x%08X)", live[0]);
            CHECK((live[8] & 0x00FFFFFF) == 0x00000000,
                  "T4 backdrop pixel (8,0) is black (got 0x%08X)", live[8]);
        } else {
            CHECK(0, "T4 live/bulk runs produced output");
        }
        free(live);
        free(bulk);
    }

    /* ---- T5: determinism (two identical headless runs) ---- */
    {
        uint32_t *a = run_live();
        uint32_t *b = run_live();
        if (a && b)
            CHECK(memcmp(a, b, FB_PIXELS * 2 * 4) == 0,
                  "T5 two headless runs byte-identical");
        else
            CHECK(0, "T5 runs produced output");
        free(a);
        free(b);
    }

    /* ---- T6: gxd_key / gxd_keymask coherence ---- */
    g = gxd_open(NULL);
    gxd_key(g, GXD_KEY_L, 1);
    gxd_key(g, GXD_KEY_R, 1);
    CHECK(gxd_keymask(g) == (0x3FF & ~(1u << 9) & ~(1u << 8)),
          "T6 keymask after L+R = 0x%03X", gxd_keymask(g));
    gxd_close(g);

    if (fails) {
        printf("test_display: %d FAILURES\n", fails);
        return 1;
    }
    printf("test_display: all tests passed\n");
    return 0;
}
