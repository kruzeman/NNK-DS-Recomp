/* test_tex.c — DS texture decode unit tests (hand-computed texels). */
#include <stdio.h>
#include <string.h>
#include "gx_tex.h"

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

static uint8_t vram[1024];
static uint16_t pal[64];
static GxTexMem mem;

static uint32_t texparam(uint32_t fmt, uint32_t size, uint32_t addr)
{
    /* bits: 0-15 addr>>3, 20-22 width log2-3, 23-25 height log2-3, 26-28 fmt */
    return (addr & 0xFFFF) | (size << 20) | (size << 23) | (fmt << 26);
}

static void lookup_eq(const char *what, uint32_t tp, uint32_t texpal,
                      int32_t s, int32_t t, int want_vis,
                      uint16_t want_color, uint8_t want_alpha)
{
    uint16_t color = 0xDEAD;
    uint8_t alpha = 0xEE;
    int vis = gx_tex_lookup(&mem, tp, texpal, s, t, &color, &alpha);
    check32(what, vis, want_vis);
    if (vis && want_vis) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s color", what);
        check32(buf, color, want_color);
        snprintf(buf, sizeof(buf), "%s alpha", what);
        check32(buf, alpha, want_alpha);
    }
}

int main(void)
{
    int i;
    memset(vram, 0, sizeof(vram));
    mem.vram = vram;
    mem.vram_size = sizeof(vram);
    mem.pal = pal;
    mem.pal_count = 64;

    for (i = 0; i < 64; i++)
        pal[i] = 0;
    pal[1] = 0x001F; /* red */
    pal[2] = 0x03E0; /* green */
    pal[3] = 0x7C00; /* blue */
    pal[5] = 0x2345;
    pal[7] = 0x1234;

    /* 16-color (4bpp), 8x8: texel (0,0)=idx1, (1,0)=idx2 (s,t in 12.4) */
    vram[0] = 0x21;
    lookup_eq("T1 fmt3 (0,0)", texparam(3, 0, 0), 0, 0, 0, 1, 0x001F, 31);
    lookup_eq("T1 fmt3 (1,0)", texparam(3, 0, 0), 0, 1 * 16, 0, 1, 0x03E0, 31);
    /* color index 0 -> alpha 31 when bit29 clear */
    vram[1] = 0x00;
    lookup_eq("T1 fmt3 idx0 opaque", texparam(3, 0, 0), 0, 2 * 16, 0, 1, 0x0000, 31);
    /* with bit29 set, index 0 -> alpha 0 (color 0 transparent) */
    lookup_eq("T1 fmt3 idx0 transp", texparam(3, 0, 0) | (1u << 29), 0, 2 * 16, 0,
              1, 0x0000, 0);

    /* 4-color (2bpp), 8x8: vram[0]=0x1B -> texels 3,2,1,0 */
    vram[0] = 0x1B;
    lookup_eq("T2 fmt2 (0,0)=3", texparam(2, 0, 0), 0, 0, 0, 1, 0x7C00, 31);
    lookup_eq("T2 fmt2 (1,0)=2", texparam(2, 0, 0), 0, 1 * 16, 0, 1, 0x03E0, 31);
    lookup_eq("T2 fmt2 (2,0)=1", texparam(2, 0, 0), 0, 2 * 16, 0, 1, 0x001F, 31);
    lookup_eq("T2 fmt2 (3,0)=0", texparam(2, 0, 0), 0, 3 * 16, 0, 1, 0x0000, 31);

    /* 256-color (8bpp) */
    vram[0] = 7;
    lookup_eq("T3 fmt4 idx7", texparam(4, 0, 0), 0, 0, 0, 1, 0x1234, 31);

    /* A3I5: pixel 0xA7 -> idx 7, alpha = ((0xA7>>3)&0x1C) + (0xA7>>6) = 22 */
    vram[0] = 0xA7;
    lookup_eq("T4 fmt1 A3I5", texparam(1, 0, 0), 0, 0, 0, 1, 0x1234, 22);

    /* A5I3: pixel 0xF5 -> idx 5, alpha = 0xF5>>3 = 30 */
    vram[0] = 0xF5;
    lookup_eq("T5 fmt6 A5I3", texparam(6, 0, 0), 0, 0, 0, 1, 0x2345, 30);

    /* direct 15bpp: alpha bit decides */
    vram[0] = 0x1F; vram[1] = 0x80; /* 0x801F: alpha set, red */
    lookup_eq("T6 fmt7 with alpha", texparam(7, 0, 0), 0, 0, 0, 1, 0x801F, 31);
    vram[0] = 0x1F; vram[1] = 0x00; /* 0x001F: no alpha bit */
    lookup_eq("T6 fmt7 no alpha", texparam(7, 0, 0), 0, 0, 0, 1, 0x001F, 0);

    /* wrapping: 8x8 texture, s=8 with repeat -> wraps to 0 */
    vram[0] = 0x21;
    lookup_eq("T7 repeat wrap s=8", texparam(3, 0, 0) | (1u << 16), 0, 8 << 4, 0,
              1, 0x001F, 31);
    /* flip: s=8 with repeat+flip -> mirrors to s=7 */
    vram[3] = 0x20; /* texel (7,0) -> idx2 (high nibble of byte 3) */
    lookup_eq("T7 flip wrap s=8", texparam(3, 0, 0) | (1u << 16) | (1u << 18), 0,
              8 << 4, 0, 1, 0x03E0, 31);

    if (failures) {
        fprintf(stderr, "test_tex: %d FAILURES\n", failures);
        return 1;
    }
    fprintf(stderr, "test_tex: all checks passed\n");
    return 0;
}
