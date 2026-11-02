/* probe2d2.c -- per-layer attribution on a GX2D state image. */
#include <stdio.h>
#include <stdlib.h>
#include "gx_2d.h"

static uint8_t *img;
static uint32_t buf[256 * 192];

static int nonblack(const uint32_t *px)
{
    int n = 0;
    for (int i = 0; i < 256 * 192; i++)
        if ((px[i] & 0xFFFFFF) != 0) n++;
    return n;
}

static void save(const char *pre, const char *tag, Gx2DState *st, int eng)
{
    char path[512];
    gx2d_render_screen(st, eng, NULL, buf);
    snprintf(path, sizeof(path), "%s_%s_%s.ppm", pre, eng ? "B" : "A", tag);
    gx2d_write_ppm(path, buf);
    printf("  %-12s nonblack=%d\n", tag, nonblack(buf));
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: probe2d2 <state> <prefix>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    img = malloc(GX2D_STATE_SIZE);
    if (!f || fread(img, 1, GX2D_STATE_SIZE, f) != GX2D_STATE_SIZE) { fprintf(stderr, "bad\n"); return 1; }
    fclose(f);
    Gx2DState st;
    gx2d_state_load(&st, img);
    for (int e = 0; e < 2; e++) {
        printf("engine %c\n", "AB"[e]);
        save(argv[2], "full", &st, e);
        for (int b = 0; b < 4; b++) {
            Gx2DState t = st;
            t.eng[e].dispcnt &= ~(0x1Fu << 8);
            t.eng[e].dispcnt &= ~(1u << 12);
            t.eng[e].dispcnt |= 1u << (8 + b);
            char tag[16]; snprintf(tag, sizeof tag, "onlyBG%d", b);
            save(argv[2], tag, &t, e);
        }
        {
            Gx2DState t = st;
            t.eng[e].dispcnt &= ~(0x1Fu << 8);
            t.eng[e].dispcnt |= 1u << 12;
            save(argv[2], "onlyOBJ", &t, e);
        }
        {
            Gx2DState t = st;
            t.eng[e].dispcnt &= ~(0x1Fu << 8);
            t.eng[e].dispcnt &= ~(1u << 12);
            save(argv[2], "none", &t, e);
        }
    }
    return 0;
}
