/* gx_scene.h — shared synthetic scene for the cube golden test and the GL
 * smoke test: an 8x8 16-color checker texture + a textured cube GX stream
 * (matrix stack ops, perspective projection, viewport, toon shading, quads).
 * Included by the tools sources; everything is static.
 */
#ifndef GX_SCENE_H
#define GX_SCENE_H

#include <string.h>
#include "gx_fifo.h"

#define GX_SCENE_TEX_SIZE 8

static uint8_t gx_scene_tex_vram[GX_SCENE_TEX_SIZE * GX_SCENE_TEX_SIZE / 2];
static uint16_t gx_scene_pal[16];
static uint16_t gx_scene_toon[32];

#define GX_SCENE_MAX_WORDS 2048
static uint32_t gx_scene_words[GX_SCENE_MAX_WORDS];
static size_t gx_scene_nwords;

static void gx_scene_emit0(uint32_t cmd)
{
    gx_scene_words[gx_scene_nwords++] = cmd;
}

static void gx_scene_emit1(uint32_t cmd, uint32_t p)
{
    gx_scene_words[gx_scene_nwords++] = cmd;
    gx_scene_words[gx_scene_nwords++] = p;
}

static void gx_scene_emitN(uint32_t cmd, const int32_t *p, int n)
{
    int i;
    gx_scene_words[gx_scene_nwords++] = cmd;
    for (i = 0; i < n; i++)
        gx_scene_words[gx_scene_nwords++] = (uint32_t)p[i];
}

/* texture: 8x8 16-color, dark border, 2x2 white/red checker */
static void gx_scene_build_texture(void)
{
    int x, y;
    memset(gx_scene_tex_vram, 0, sizeof(gx_scene_tex_vram));
    for (y = 0; y < GX_SCENE_TEX_SIZE; y++) {
        for (x = 0; x < GX_SCENE_TEX_SIZE; x++) {
            uint8_t idx;
            if (x == 0 || y == 0 || x == GX_SCENE_TEX_SIZE - 1 ||
                y == GX_SCENE_TEX_SIZE - 1)
                idx = 4;
            else
                idx = (uint8_t)((((x >> 1) ^ (y >> 1)) & 1) ? 2 : 1);
            {
                size_t byte = ((size_t)y * GX_SCENE_TEX_SIZE + x) >> 1;
                if (x & 1) gx_scene_tex_vram[byte] |= (uint8_t)(idx << 4);
                else       gx_scene_tex_vram[byte] |= idx;
            }
        }
    }
    gx_scene_pal[0] = 0x0000;
    gx_scene_pal[1] = 0x7FFF; /* white */
    gx_scene_pal[2] = 0x001F; /* red */
    gx_scene_pal[3] = 0x7C00; /* blue */
    gx_scene_pal[4] = 0x1084; /* dark gray border */
    for (x = 5; x < 16; x++)
        gx_scene_pal[x] = 0;
    for (x = 0; x < 32; x++)
        gx_scene_toon[x] = (uint16_t)(x | (x << 5) | (x << 10)); /* gray ramp */
}

static void gx_scene_build_stream(void)
{
    /* projection: clip_x = 2x, clip_y = 2y, clip_z = A*z + B, clip_w = -z
     * with A = B = -1.25 (fx -5120): z=-1 -> z/w=0, z=-5 -> z/w=1 */
    const int32_t proj[16] = {
        0x2000, 0, 0, 0,
        0, 0x2000, 0, 0,
        0, 0, -5120, -0x1000,
        0, 0, -5120, 0
    };
    /* position: T(0,0,-3) * Ry(45deg) * Rx(20deg) (applied right-to-left) */
    const int32_t trans[12] = {
        0x1000, 0, 0,  0, 0x1000, 0,  0, 0, 0x1000,  0, 0, -0x3000
    };
    const int32_t ry[12] = { /* cos=sin=0.70703 (fx 0xB50) */
        0xB50, 0, -0xB50,  0, 0x1000, 0,  0xB50, 0, 0xB50,  0, 0, 0
    };
    const int32_t rx[12] = { /* cos20=0xF08, sin20=0x579 */
        0x1000, 0, 0,  0, 0xF08, 0x579,  0, -0x579, 0xF08,  0, 0, 0
    };
    static const int16_t cube[8][3] = {
        { -0x800, -0x800, -0x800 }, {  0x800, -0x800, -0x800 },
        {  0x800,  0x800, -0x800 }, { -0x800,  0x800, -0x800 },
        { -0x800, -0x800,  0x800 }, {  0x800, -0x800,  0x800 },
        {  0x800,  0x800,  0x800 }, { -0x800,  0x800,  0x800 }
    };
    static const int faces[6][4] = {
        { 4, 5, 6, 7 }, /* front  */
        { 1, 0, 3, 2 }, /* back   */
        { 0, 4, 7, 3 }, /* left   */
        { 5, 1, 2, 6 }, /* right  */
        { 7, 6, 2, 3 }, /* top    */
        { 0, 1, 5, 4 }  /* bottom */
    };
    static const uint32_t face_colors[6] = {
        31u | (31u << 5) | (31u << 10),
        16u | (16u << 5) | (16u << 10),
        24u | (20u << 5) | (16u << 10),
        20u | (24u << 5) | (16u << 10),
        28u | (28u << 5) | (24u << 10),
        12u | (12u << 5) | (12u << 10)
    };
    static const int32_t uv[4][2] = {
        { 0, 0 }, { 8 * 16, 0 }, { 8 * 16, 8 * 16 }, { 0, 8 * 16 }
    };

    int f, i;

    gx_scene_nwords = 0;

    gx_scene_emit1(0x10, 0);   /* MTX_MODE projection */
    gx_scene_emit0(0x15);      /* MTX_IDENTITY */
    gx_scene_emitN(0x16, proj, 16);
    gx_scene_emit1(0x10, 2);   /* MTX_MODE position+vector */
    gx_scene_emit0(0x15);
    gx_scene_emitN(0x17, trans, 12);
    gx_scene_emitN(0x19, ry, 12);
    gx_scene_emitN(0x19, rx, 12);

    gx_scene_emit1(0x60, (0u) | (0u << 8) | (255u << 16) | (191u << 24));
    /* POLYGON_ATTR: toon mode (2), render front+back, alpha 31 */
    gx_scene_emit1(0x29, (2u << 4) | (1u << 6) | (1u << 7) | (31u << 16));
    /* TEXIMAGE_PARAM: 16-color, 8x8, addr 0, S/T repeat */
    gx_scene_emit1(0x2A, (3u << 26) | (1u << 16) | (1u << 17));
    gx_scene_emit1(0x2B, 0);   /* PLTT_BASE */

    gx_scene_emit1(0x40, 1);   /* BEGIN_VTXS quads */
    for (f = 0; f < 6; f++) {
        gx_scene_emit1(0x20, face_colors[f]); /* COLOR */
        for (i = 0; i < 4; i++) {
            int v = faces[f][i];
            gx_scene_emit1(0x22, ((uint32_t)(uint16_t)uv[i][1] << 16) |
                                 (uint32_t)(uint16_t)uv[i][0]);
            gx_scene_emit1(0x23, ((uint32_t)(uint16_t)cube[v][1] << 16) |
                                 (uint32_t)(uint16_t)cube[v][0]);
            gx_scene_words[gx_scene_nwords++] = (uint32_t)(uint16_t)cube[v][2];
        }
    }
    gx_scene_emit0(0x41);      /* END_VTXS */
    gx_scene_emit1(0x50, 0);   /* SWAP_BUFFERS */
}

#endif /* GX_SCENE_H */
