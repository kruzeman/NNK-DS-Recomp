/* gx_cube_test.c — headless deterministic golden test.
 *
 * Builds a synthetic DS GX command stream (textured cube: 4x4 matrix stack
 * ops, perspective projection, viewport, toon shading, 16-color texture,
 * quad submission via VTX_16 — see tools/gx_scene.h) and renders it through
 * the full pipeline (FIFO decode -> transform -> cull -> rasterizer) to PPM.
 *
 * Usage:
 *   gx_cube_test [--out FILE] [--runs N]
 *
 * --runs N renders the stream N times and verifies every run produces
 * byte-identical color/depth buffers.  Exit 0 = all runs identical + the
 * scene contains the expected content.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_scene.h"

static uint32_t colorbuf[GX_SCREEN_W * GX_SCREEN_H];
static uint32_t depthbuf[GX_SCREEN_W * GX_SCREEN_H];

static uint64_t fnv1a(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int render_once(uint32_t *color, uint32_t *depth)
{
    static Gx gx;
    gx_init(&gx, color, depth, GX_SCREEN_W, GX_SCREEN_H);
    gx_set_texture_mem(&gx, gx_scene_tex_vram, sizeof(gx_scene_tex_vram),
                       gx_scene_pal, 16);
    memcpy(gx.toon, gx_scene_toon, sizeof(gx_scene_toon));
    gx_surface_clear(&gx.surface, 0x00080402u /* dark blue 6-bit (r=2,g=4,b=8) */,
                     0x7FFFFFFFu);
    gx_fifo_write_block(&gx, gx_scene_words, gx_scene_nwords);
    return (int)gx.n_polys_drawn;
}

int main(int argc, char **argv)
{
    const char *out = "cube.ppm";
    int runs = 1, i, polys;
    uint64_t h0c, h0d;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--out FILE] [--runs N]\n", argv[0]);
            return 2;
        }
    }
    if (runs < 1) runs = 1;

    gx_scene_build_texture();
    gx_scene_build_stream();

    {
        static uint32_t color0[GX_SCREEN_W * GX_SCREEN_H];
        static uint32_t depth0[GX_SCREEN_W * GX_SCREEN_H];

        polys = render_once(color0, depth0);
        h0c = fnv1a(color0, sizeof(color0));
        h0d = fnv1a(depth0, sizeof(depth0));
        printf("run 0: polys=%d color=%016llx depth=%016llx\n", polys,
               (unsigned long long)h0c, (unsigned long long)h0d);

        for (i = 1; i < runs; i++) {
            uint64_t hc, hd;
            polys = render_once(colorbuf, depthbuf);
            hc = fnv1a(colorbuf, sizeof(colorbuf));
            hd = fnv1a(depthbuf, sizeof(depthbuf));
            printf("run %d: polys=%d color=%016llx depth=%016llx\n", i, polys,
                   (unsigned long long)hc, (unsigned long long)hd);
            if (hc != h0c || hd != h0d ||
                memcmp(color0, colorbuf, sizeof(color0)) != 0 ||
                memcmp(depth0, depthbuf, sizeof(depth0)) != 0) {
                fprintf(stderr, "DETERMINISM FAILURE at run %d\n", i);
                return 1;
            }
        }

        /* content sanity: the cube must cover real pixels */
        {
            int n_lit = 0, n;
            for (n = 0; n < GX_SCREEN_W * GX_SCREEN_H; n++)
                if (color0[n] != 0x00080402u)
                    n_lit++;
            printf("covered pixels: %d / %d\n", n_lit, GX_SCREEN_W * GX_SCREEN_H);
            if (n_lit < 1000) {
                fprintf(stderr, "CONTENT FAILURE: cube barely visible\n");
                return 1;
            }
        }

        {
            static Gx gx;
            gx_init(&gx, color0, depth0, GX_SCREEN_W, GX_SCREEN_H);
            if (gx_write_ppm(out, &gx.surface) != 0) {
                fprintf(stderr, "cannot write %s\n", out);
                return 1;
            }
        }
    }

    printf("OK: %d run(s) byte-identical -> %s\n", runs, out);
    return 0;
}
