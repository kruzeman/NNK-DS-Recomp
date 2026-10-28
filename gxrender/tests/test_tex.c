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

static uint8_t vram[0x40000]; /* 256 KiB: format 5 keeps its block table at +0x20000 */
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

    /* ---- 4x4-texel compressed (fmt5; melonDS TextureLookup case 5) ----
     * 8x8 texture -> four 4x4 blocks; each block = 4 data bytes at
     * vram[base + (t&3) + (s&0x3FC) + (t&0x3FC)*(w>>2)] and one 16-bit
     * palette-info word at vram[0x20000 + ((dataaddr & 0x1FFFC) >> 1)]:
     *   block (0,0) data vram[0..3]   palinfo @ 0x20000
     *   block (1,0) data vram[4..7]   palinfo @ 0x20002
     *   block (0,1) data vram[8..11]  palinfo @ 0x20004
     *   block (1,1) data vram[12..15] palinfo @ 0x20006
     * Row byte 0xE4 holds texel indices 0,1,2,3.  palinfo:
     * bits 0-13 palette offset/4 (byte offset = idx*4), bits 14-15 mode. */
    vram[0] = vram[4] = vram[8] = vram[12] = 0xE4;
    pal[0] = 0x001F; pal[1] = 0x03E0; pal[2] = 0x7C00; pal[3] = 0x1234;
    pal[4] = 0x001F; pal[5] = 0x0000;
    pal[8] = 0x001F; pal[9] = 0x0000;
    pal[12] = 0x001F; pal[13] = 0x03E0; pal[14] = 0x7C00; pal[15] = 0x1234;
    /* block (0,0): mode 0 -> idx0/1 = pal0/1, idx2 = pal2, idx3 = transparent */
    vram[0x20000] = 0x00; vram[0x20001] = 0x00;
    lookup_eq("T8 fmt5 m0 idx0", texparam(5, 0, 0), 0, 0, 0, 1, 0x001F, 31);
    lookup_eq("T8 fmt5 m0 idx1", texparam(5, 0, 0), 0, 1 << 4, 0, 1, 0x03E0, 31);
    lookup_eq("T8 fmt5 m0 idx2", texparam(5, 0, 0), 0, 2 << 4, 0, 1, 0x7C00, 31);
    lookup_eq("T8 fmt5 m0 idx3", texparam(5, 0, 0), 0, 3 << 4, 0, 1, 0x0000, 0);
    /* block (1,0): mode 1 -> idx2 = (c0+c1)/2, idx3 = transparent */
    vram[0x20002] = 0x02; vram[0x20003] = 0x40; /* palinfo 0x4002 */
    lookup_eq("T8 fmt5 m1 idx0", texparam(5, 0, 0), 0, 4 << 4, 0, 1, 0x001F, 31);
    lookup_eq("T8 fmt5 m1 idx1", texparam(5, 0, 0), 0, 5 << 4, 0, 1, 0x0000, 31);
    lookup_eq("T8 fmt5 m1 idx2", texparam(5, 0, 0), 0, 6 << 4, 0, 1, 0x000F, 31);
    lookup_eq("T8 fmt5 m1 idx3", texparam(5, 0, 0), 0, 7 << 4, 0, 1, 0x0000, 0);
    /* block (0,1): mode 3 -> idx2 = (5c0+3c1)/8, idx3 = (3c0+5c1)/8 */
    vram[0x20004] = 0x04; vram[0x20005] = 0xC0; /* palinfo 0xC004 */
    lookup_eq("T8 fmt5 m3 idx0", texparam(5, 0, 0), 0, 0, 4 << 4, 1, 0x001F, 31);
    lookup_eq("T8 fmt5 m3 idx1", texparam(5, 0, 0), 0, 1 << 4, 4 << 4, 1, 0x0000, 31);
    lookup_eq("T8 fmt5 m3 idx2", texparam(5, 0, 0), 0, 2 << 4, 4 << 4, 1, 0x0013, 31);
    lookup_eq("T8 fmt5 m3 idx3", texparam(5, 0, 0), 0, 3 << 4, 4 << 4, 1, 0x000B, 31);
    /* block (1,1): mode 2 -> idx2 = pal2, idx3 = pal3 */
    vram[0x20006] = 0x06; vram[0x20007] = 0x80; /* palinfo 0x8006 */
    lookup_eq("T8 fmt5 m2 idx0", texparam(5, 0, 0), 0, 4 << 4, 4 << 4, 1, 0x001F, 31);
    lookup_eq("T8 fmt5 m2 idx1", texparam(5, 0, 0), 0, 5 << 4, 4 << 4, 1, 0x03E0, 31);
    lookup_eq("T8 fmt5 m2 idx2", texparam(5, 0, 0), 0, 6 << 4, 4 << 4, 1, 0x7C00, 31);
    lookup_eq("T8 fmt5 m2 idx3", texparam(5, 0, 0), 0, 7 << 4, 4 << 4, 1, 0x1234, 31);
    /* texpal base: texpal<<4 = 16 bytes -> palette pair at pal[8]/pal[9] */
    lookup_eq("T8 fmt5 texpal<<4", texparam(5, 0, 0), 1, 0, 0, 1, 0x001F, 31);
    /* slot-1 texel reads are forced to 0 (melonDS); data byte ignored */
    vram[0x20004] = 0x00; vram[0x20005] = 0x00; /* palinfo @0x20004 -> pal[0] */
    vram[0x20008] = 0xE4;                       /* garbage texel data */
    lookup_eq("T8 fmt5 slot1 texel=0", texparam(5, 0, 0x4000), 0, 0, 4 << 4,
              1, 0x001F, 31);

    if (failures) {
        fprintf(stderr, "test_tex: %d FAILURES\n", failures);
        return 1;
    }
    fprintf(stderr, "test_tex: all checks passed\n");
    return 0;
}
