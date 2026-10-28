/* gx_gl_smoke.c — GL backend skeleton smoke test.
 *
 * Feeds the synthetic textured-cube GX stream (tools/gx_scene.h) through the
 * real FIFO decoder, routes the decoded polygons to the OpenGL backend
 * (draw callback, software rasterizer disabled), renders via SDL2+GL under
 * xvfb-run, reads the framebuffer back and writes a PPM.  Verifies that
 * guest-derived draw commands reach the GL backend and cover the screen.
 *
 * Run:  xvfb-run -a build/gx_gl_smoke [--out FILE] [--window]
 * Exit 0 = GL context created, polygons drawn, content present, PPM written.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gx_scene.h"
#include "gx_gl.h"

#define MAX_POLYS 256

static GxPolygon polys[MAX_POLYS];
static int n_polys;

static void collect_poly(void *user, const GxPolygon *poly)
{
    (void)user;
    if (n_polys < MAX_POLYS)
        polys[n_polys++] = *poly;
}

int main(int argc, char **argv)
{
    const char *out = "cube_gl.ppm";
    static Gx gx;
    static uint32_t colorbuf[GX_SCREEN_W * GX_SCREEN_H];
    static uint32_t depthbuf[GX_SCREEN_W * GX_SCREEN_H];
    static uint8_t rgba[GX_SCREEN_W * GX_SCREEN_H * 4];
    GxGl *gl;
    int i, windowed = 0, n_lit = 0;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--window")) windowed = 1;
        else {
            fprintf(stderr, "usage: %s [--out FILE] [--window]\n", argv[0]);
            return 2;
        }
    }

    gx_scene_build_texture();
    gx_scene_build_stream();

    /* decode the GX stream, routing polygons to a collection callback */
    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);
    gx_set_texture_mem(&gx, gx_scene_tex_vram, sizeof(gx_scene_tex_vram),
                       gx_scene_pal, 16);
    memcpy(gx.toon, gx_scene_toon, sizeof(gx_scene_toon));
    gx.draw_fn = collect_poly;
    gx.skip_software = 1;
    gx_fifo_write_block(&gx, gx_scene_words, gx_scene_nwords);

    printf("GX decode: %u verts, %u polys submitted, %d captured\n",
           gx.n_verts, gx.n_polys_drawn, n_polys);
    if (n_polys < 6) { /* the cube scene submits exactly 6 quads */
        fprintf(stderr, "FAIL: too few polygons reached the GL callback\n");
        return 1;
    }

    gl = gx_gl_open(windowed ? 2 : 1);
    if (!gl) {
        fprintf(stderr, "FAIL: cannot create SDL2+GL context\n");
        return 1;
    }
    printf("GL_VENDOR:   %s\n", gx_gl_vendor(gl));
    printf("GL_RENDERER: %s\n", gx_gl_renderer(gl));
    printf("GL_VERSION:  %s\n", gx_gl_version(gl));

    gx_gl_begin_frame(gl, 0x00080402u);
    for (i = 0; i < n_polys; i++)
        gx_gl_draw_polygon(gl, &polys[i], &gx.texmem);
    gx_gl_end_frame(gl);

    gx_gl_read_rgba(gl, rgba);
    for (i = 0; i < GX_SCREEN_W * GX_SCREEN_H; i++) {
        /* anything that is not the dark-blue clear color counts as lit */
        if (rgba[i * 4 + 0] > 16 || rgba[i * 4 + 1] > 16 || rgba[i * 4 + 2] > 32)
            n_lit++;
    }
    printf("GL covered pixels: %d / %d\n", n_lit, GX_SCREEN_W * GX_SCREEN_H);
    if (n_lit < 500) {
        fprintf(stderr, "FAIL: GL frame is (nearly) empty\n");
        gx_gl_close(gl);
        return 1;
    }

    if (gx_gl_read_ppm(gl, out) != 0) {
        fprintf(stderr, "FAIL: cannot write %s\n", out);
        gx_gl_close(gl);
        return 1;
    }
    printf("GL smoke OK -> %s\n", out);

    if (windowed)
        SDL_Delay(2000);
    gx_gl_close(gl);
    return 0;
}
