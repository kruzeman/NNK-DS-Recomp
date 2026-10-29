/* test_matrix.c — matrix / transform unit tests against hand-computed values.
 *
 * Every expected constant below is derived by hand from the DS fixed-point
 * formulas (see comments); run with: build/test_matrix
 */
#include <stdio.h>
#include <string.h>
#include "gx_fifo.h"

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

static void check_true(const char *what, int cond)
{
    if (!cond) {
        failures++;
        fprintf(stderr, "FAIL %s\n", what);
    } else {
        fprintf(stderr, "ok   %s\n", what);
    }
}

/* ---- capture hook for the vertex pipeline ---- */
static GxPolygon captured;
static int n_captured;

static void capture_poly(void *user, const GxPolygon *poly)
{
    (void)user;
    captured = *poly;
    n_captured++;
}

static uint32_t colorbuf[GX_SCREEN_W * GX_SCREEN_H];
static uint32_t depthbuf[GX_SCREEN_W * GX_SCREEN_H];

static void test_matrix_ops(void)
{
    int32_t m[16], s[16];

    /* T1: identity */
    gx_mat_identity(m);
    check32("T1 identity m[0]", m[0], 0x1000);
    check32("T1 identity m[5]", m[5], 0x1000);
    check32("T1 identity m[10]", m[10], 0x1000);
    check32("T1 identity m[15]", m[15], 0x1000);
    check32("T1 identity m[12]", m[12], 0);

    /* T2: load4x3 column layout */
    {
        const int32_t p[12] = { 1,2,3, 4,5,6, 7,8,9, 10,11,12 };
        gx_mat_load4x3(m, p);
        check32("T2 load4x3 col0 r0", m[0], 1);
        check32("T2 load4x3 col0 r1", m[1], 2);
        check32("T2 load4x3 col1 r2", m[6], 6);
        check32("T2 load4x3 translation r0", m[12], 10);
        check32("T2 load4x3 M(3,3)", m[15], 0x1000);
        check32("T2 load4x3 col pad", m[3], 0);
    }

    /* T3: mult4x3 = m*s (s applied to the vertex first).
     * m = I|t(x=1.0); s = rotation x->-y (90 deg), no translation.
     * Hand-computed (see derivation in commit message):
     *   m[0] = (0*0x1000 + (-0x1000)*0 + 0*0)>>12            = 0
     *   m[1] = (0*0 + (-0x1000)*0x1000 + 0)>>12              = -0x1000
     *   m[4] = (0x1000*0x1000)>>12                           = 0x1000
     *   m[12]= (0x1000*0x1000)>>12                           = 0x1000
     *   m[15]= 0x1000
     */
    {
        const int32_t t[12] = { 0x1000,0,0, 0,0x1000,0, 0,0,0x1000, 0x1000,0,0 };
        const int32_t r[12] = { 0,-0x1000,0, 0x1000,0,0, 0,0,0x1000, 0,0,0 };
        gx_mat_load4x3(m, t);
        gx_mat_mult4x3(m, r);
        check32("T3 mult4x3 m[0]", m[0], 0);
        check32("T3 mult4x3 m[1]", m[1], -0x1000);
        check32("T3 mult4x3 m[4]", m[4], 0x1000);
        check32("T3 mult4x3 m[5]", m[5], 0);
        check32("T3 mult4x3 m[10]", m[10], 0x1000);
        check32("T3 mult4x3 m[12]", m[12], 0x1000);
        check32("T3 mult4x3 m[13]", m[13], 0);
        check32("T3 mult4x3 m[15]", m[15], 0x1000);
    }

    /* T4: scale: rows 0..2 scaled by 2.0 */
    {
        const int32_t sc[3] = { 0x2000, 0x2000, 0x2000 };
        gx_mat_identity(m);
        gx_mat_scale(m, sc);
        check32("T4 scale m[0]", m[0], 0x2000);
        check32("T4 scale m[5]", m[5], 0x2000);
        check32("T4 scale m[10]", m[10], 0x2000);
        check32("T4 scale m[15]", m[15], 0x1000);
    }

    /* T5: translate on identity: m[12..14] = s */
    {
        const int32_t tr[3] = { 0x1000, 0x2000, -0x3000 };
        gx_mat_identity(m);
        gx_mat_translate(m, tr);
        check32("T5 translate m[12]", m[12], 0x1000);
        check32("T5 translate m[13]", m[13], 0x2000);
        check32("T5 translate m[14]", m[14], -0x3000);
        check32("T5 translate m[15]", m[15], 0x1000);
    }
    (void)s;
}

static void test_transform_and_viewport(void)
{
    /* Pipeline test: proj=identity, pos=translate(0.25, 0.25, -0.5),
     * full-screen viewport.  The triangle lies INSIDE the clip volume
     * (|x|,|y|,|z| <= w) so full frustum clipping leaves it untouched.
     * Hand-computed for vertex A=(0,0,0):
     *   clip = (0x400, 0x400, -0x800, 0x1000)
     *   integer FinalPosition (melonDS GPU3D.cpp:1317-1343):
     *     posX = 0x400+0x1000 = 0x1400, den = 0x2000
     *     sx = (0x1400*256)/0x2000 + 0 = 160
     *     posY = 0x1000-0x400 = 0xC00
     *     sy = (0xC00*192)/0x2000 + 0 = 72
     *   depth (melonDS FinalZ, GPU3D.cpp:1541):
     *     ((-0x800 * 0x4000)/0x1000 + 0x3FFF) * 0x200 = 0x1FFF*0x200
     *     = 0x3FFE00
     *   FinalW: wsize = 16 (W = 0x1000) -> wnorm = 0x1000
     * B=(0x800,0,0): clip x = 0xC00 -> sx = 224, same sy/depth
     * C=(0,0x800,0x400): clip = (0x400, 0xC00, -0x400, 0x1000)
     *   sx = 160, sy = (0x400*192)/0x2000 = 24
     *   depth = ((-0x400*0x4000)/0x1000 + 0x3FFF)*0x200 = 0x2FFF*0x200
     *     = 0x5FFE00
     */
    static Gx gx;
    const uint32_t viewport = (0u) | (0u << 8) | (255u << 16) | (191u << 24);
    const uint32_t attr = (1u << 6) | (1u << 7) | (31u << 16);

    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);
    gx.draw_fn = capture_poly;
    gx.draw_user = NULL;
    n_captured = 0;

    gx_fifo_write(&gx, 0x10); gx_fifo_write(&gx, 0);  /* MTX_MODE proj */
    gx_fifo_write(&gx, 0x15);                          /* MTX_IDENTITY */
    gx_fifo_write(&gx, 0x10); gx_fifo_write(&gx, 1);  /* MTX_MODE pos */
    gx_fifo_write(&gx, 0x15);
    gx_fifo_write(&gx, 0x1C);                          /* MTX_TRANS */
    gx_fifo_write(&gx, 0x00000400);
    gx_fifo_write(&gx, 0x00000400);
    gx_fifo_write(&gx, (uint32_t)-0x800);
    gx_fifo_write(&gx, 0x60); gx_fifo_write(&gx, viewport);
    gx_fifo_write(&gx, 0x29); gx_fifo_write(&gx, attr);
    gx_fifo_write(&gx, 0x40); gx_fifo_write(&gx, 0);   /* BEGIN triangles */
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000000); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000800); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x08000000u); gx_fifo_write(&gx, 0x400);
    gx_fifo_write(&gx, 0x41);                          /* END */

    check32("T6 polygon captured", n_captured, 1);
    if (n_captured != 1)
        return;

    check32("T6 A sx", captured.v[0].sx, 160);
    check32("T6 A sy", captured.v[0].sy, 72);
    check32("T6 A depth", captured.v[0].depth, 0x3FFE00);
    check32("T6 A wnorm", captured.v[0].wnorm, 0x1000);
    check32("T6 B sx", captured.v[1].sx, 224);
    check32("T6 B sy", captured.v[1].sy, 72);
    check32("T6 C sx", captured.v[2].sx, 160);
    check32("T6 C sy", captured.v[2].sy, 24);
    check32("T6 C depth", captured.v[2].depth, 0x5FFE00);

    /* T7: culling (clip-space normal over (x,y,w), melonDS SubmitPolygon;
     * dot < 0 here -> back face).  Screen-space area2 of A,B,C =
     * (224-160)*(24-72) = -3072 (kept as a raster-winding sanity check). */
    {
        int64_t area2;
        GxPolygon p = captured;
        area2 = gx_poly_area2(&p);
        check_true("T7 area2 negative (back face)", area2 == -3072);
    }
    gx_fifo_write(&gx, 0x29);
    gx_fifo_write(&gx, (1u << 6) | (31u << 16)); /* front only */
    gx_fifo_write(&gx, 0x40); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000000); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000800); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x08000000u); gx_fifo_write(&gx, 0x400);
    gx_fifo_write(&gx, 0x41);
    check32("T7 back face culled (front-only)", n_captured, 1);

    gx_fifo_write(&gx, 0x29);
    gx_fifo_write(&gx, (1u << 7) | (31u << 16)); /* back only */
    gx_fifo_write(&gx, 0x40); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000000); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x00000800); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x23); gx_fifo_write(&gx, 0x08000000u); gx_fifo_write(&gx, 0x400);
    gx_fifo_write(&gx, 0x41);
    check32("T7 back face drawn (back-only)", n_captured, 2);
}

static void test_lighting(void)
{
    /* Hand-computed lighting case (see GX_COVERAGE.md "lighting notes"):
     * - vec matrix = identity
     * - light 0 dir = (0, 0, -512) raw -> light_dir = (0, 0, +512)
     *   (hardware negates the direction), spec_recip = 2^18/1024 = 256
     * - normal = (0, 0, 1023); nt = (0, 0, 1023)
     * - dot = (512*1023)>>9 = 1022 -> diffuse = 0 -> no diffuse term
     *   specular dot: 1022+1023 = 2045 -> 11-bit wrap -> -3 -> sq>>10 = 0
     *   shinelevel = (0*256)>>8 - 512 = -512 -> clamped 0
     * - ambient = (4,4,4), light color = (31,31,31):
     *   vtxbuff[i] = (0 + 4<<9) * 31 = 2048*31 = 63488 -> >>14 = 3
     * Expected vertex color: (3, 3, 3)
     */
    static Gx gx;
    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);

    /* DIF_AMB: diff 0, ambient 4/4/4 */
    gx_fifo_write(&gx, 0x30);
    gx_fifo_write(&gx, (4u << 16) | (4u << 21) | (4u << 26));
    /* SPE_EMI: 0 */
    gx_fifo_write(&gx, 0x31); gx_fifo_write(&gx, 0);
    /* LIGHT_COLOR 0 = (31,31,31): light index lives in bits 30-31 (= 0) */
    gx_fifo_write(&gx, 0x33);
    gx_fifo_write(&gx, 31u | (31u << 5) | (31u << 10));
    /* LIGHT_VECTOR 0 = (0, 0, -512): 10-bit fields, index bits 30-31 = 0 */
    gx_fifo_write(&gx, 0x32);
    gx_fifo_write(&gx, (0x200u << 20));
    /* polygon attr: light 0 on, alpha 31 */
    gx_fifo_write(&gx, 0x29);
    gx_fifo_write(&gx, 1u | (31u << 16) | (1u << 6) | (1u << 7));
    gx_fifo_write(&gx, 0x40); gx_fifo_write(&gx, 0); /* latch attr */
    /* NORMAL (0, 0, 1023) */
    gx_fifo_write(&gx, 0x21); gx_fifo_write(&gx, 0x3FFu << 20);

    check32("T8 lighting r", gx.vtx_color[0], 3);
    check32("T8 lighting g", gx.vtx_color[1], 3);
    check32("T8 lighting b", gx.vtx_color[2], 3);
}

static void test_fifo_packing(void)
{
    /* Packed commands: one word holds COLOR(0x20) then MTX_MODE(0x10);
     * their parameter words follow.  Both must take effect. */
    static Gx gx;
    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);

    gx_fifo_write(&gx, 0x00001020u); /* commands 0x20, 0x10 */
    gx_fifo_write(&gx, 0x00000C85u); /* COLOR: r=5, g=4, b=3 */
    gx_fifo_write(&gx, 0x00000003u); /* MTX_MODE = texture */

    check32("T9 packed COLOR r", gx.vtx_color[0], 5);
    check32("T9 packed COLOR g", gx.vtx_color[1], 4);
    check32("T9 packed COLOR b", gx.vtx_color[2], 3);
    check32("T9 packed MTX_MODE", gx.matrix_mode, 3);
}

static void test_texcoord_transform(void)
{
    /* TEX_PARAM texcoord mode 1, texture matrix = identity + trans x = 0x2000.
     * TEXCOORD raw = (s=0x0100, t=0):
     *   cur_s = (0x100*0x1000 + 0 + 0 + 0x2000) >> 12 = 0x102
     *   cur_t = (0 + 0 + 0 + 0) >> 12 = 0
     */
    static Gx gx;
    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);

    gx_fifo_write(&gx, 0x2A); gx_fifo_write(&gx, 1u << 30); /* texcoord mode 1 */
    gx_fifo_write(&gx, 0x10); gx_fifo_write(&gx, 3);        /* MTX_MODE tex */
    gx_fifo_write(&gx, 0x15);                               /* identity */
    gx_fifo_write(&gx, 0x1C); gx_fifo_write(&gx, 0x2000);   /* trans x */
    gx_fifo_write(&gx, 0); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x22); gx_fifo_write(&gx, 0x00000100);

    check32("T10 texcoord xform s", gx.cur_s, 0x102);
    check32("T10 texcoord xform t", gx.cur_t, 0);
}

static void test_matrix_stack(void)
{
    /* Position stack: push/restore/offset-pop round trips. */
    static Gx gx;
    const int32_t tr1[3] = { 0x1000, 0, 0 };
    gx_init(&gx, colorbuf, depthbuf, GX_SCREEN_W, GX_SCREEN_H);

    gx_fifo_write(&gx, 0x10); gx_fifo_write(&gx, 1); /* pos mode */
    gx_fifo_write(&gx, 0x15);                        /* identity */
    gx_fifo_write(&gx, 0x1C); gx_fifo_write(&gx, 0x1000); gx_fifo_write(&gx, 0); gx_fifo_write(&gx, 0);
    gx_fifo_write(&gx, 0x11);                        /* push (stores current) */
    gx_fifo_write(&gx, 0x15);                        /* identity */
    gx_fifo_write(&gx, 0x12); gx_fifo_write(&gx, 1); /* pop 1 level */
    check32("T11 pop restores m[12]", gx.pos[12], tr1[0]);

    /* store at slot 3 then restore */
    gx_fifo_write(&gx, 0x13); gx_fifo_write(&gx, 3);
    gx_fifo_write(&gx, 0x15);
    gx_fifo_write(&gx, 0x14); gx_fifo_write(&gx, 3);
    check32("T11 restore slot3 m[12]", gx.pos[12], tr1[0]);
}

int main(void)
{
    test_matrix_ops();
    test_transform_and_viewport();
    test_lighting();
    test_fifo_packing();
    test_texcoord_transform();
    test_matrix_stack();

    if (failures) {
        fprintf(stderr, "test_matrix: %d FAILURES\n", failures);
        return 1;
    }
    fprintf(stderr, "test_matrix: all checks passed\n");
    return 0;
}
