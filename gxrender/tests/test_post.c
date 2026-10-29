/* test_post.c — DS frame post-pass unit tests: fog density lookup + fog
 * blend + edge marking.  All constants hand-computed from the melonDS
 * formulas (GPU3D_Soft.cpp CalculateFogDensity 1414-1451, ScanlineFinalPass
 * 1453-1570); the arithmetic is shown in each test comment. */
#include <stdio.h>
#include <string.h>
#include "gx_raster.h"

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

/* Build the 34-entry melonDS RenderFogDensityTable from the 32 register
 * entries: [0]=reg[0], [1..32]=reg[0..31], [33]=reg[31]
 * (GPU3D.cpp:2699-2701). */
static void build_fog_table(uint8_t t[34], const uint8_t reg[32])
{
    t[0] = reg[0];
    memcpy(&t[1], reg, 32);
    t[33] = reg[31];
}

static uint32_t fog10(uint32_t z)
{
    uint8_t reg[32], t[34];
    int i;
    for (i = 0; i < 32; i++) reg[i] = (uint8_t)i;
    reg[0] = 10;
    reg[1] = 20;
    reg[31] = 30;
    build_fog_table(t, reg);
    /* fog offset = 0x100 (register value 0x100*0x200 = 0x20000 is what
     * gx_replay latches; here we pass the latched value directly) */
    return gx_fog_density(t, 0x20000, 0, z);
}

static void test_fog_density(void)
{
    uint8_t reg[32], t[34];
    int i;

    /* F1: z < fog offset -> table index 0, frac 0 -> density = T[0]
     * = (T[0] * 0x20000) >> 17 = T[0] = 10 */
    check32("F1 below offset", (int32_t)fog10(0x1FFFF), 10);

    /* F2: z = offset + 4 -> z' = (4>>2)<<0 = 1 -> id 0, frac 1:
     * T[0] = T[1] = reg[0] = 10 (both map to entry 0) -> flat -> 10 */
    check32("F2 id0 flat segment", (int32_t)fog10(0x20000 + 4), 10);

    /* F3: z' = 0x20000 (z = offset + 0x80000) -> id 1, frac 0:
     * density = T[1] = reg[0] = 10 */
    check32("F3 id1 boundary", (int32_t)fog10(0x20000 + 0x80000), 10);

    /* F4: z' = 0x30000 -> id 1, frac 0x10000 (half):
     * (T[1]*(0x20000-0x10000) + T[2]*0x10000) >> 17
     * = (10 + 20) * 0x10000 >> 17 = 30/2 = 15 */
    check32("F4 id1 half interp", (int32_t)fog10(0x20000 + 0xC0000), 15);

    /* F5: z' = 0x40000 -> id 2, frac 0 -> T[2] = reg[1] = 20 */
    check32("F5 id2 boundary", (int32_t)fog10(0x20000 + 0x100000), 20);

    /* F6: z' = 0x400000 -> id 32 -> clamped: T[32] = reg[31] = 30 */
    check32("F6 id32 clamp", (int32_t)fog10(0x20000 + 0x1000000), 30);

    /* F7: even deeper stays at id 32 -> 30 */
    check32("F7 id32 deep", (int32_t)fog10(0x20000 + 0x2000000), 30);

    /* F8: fog shift (DISP3DCNT bits 8-11): shift 2, z = offset + 4:
     * z' = (4>>2)<<2 = 4 -> id 0 -> flat segment -> 10 */
    for (i = 0; i < 32; i++) reg[i] = (uint8_t)i;
    reg[0] = 10; reg[1] = 20; reg[31] = 30;
    build_fog_table(t, reg);
    check32("F8 fog shift", (int32_t)gx_fog_density(t, 0x20000, 2, 0x20004), 10);

    /* F9: the 127 -> 128 bump (GPU3D_Soft.cpp:1448) */
    for (i = 0; i < 32; i++) reg[i] = 127;
    build_fog_table(t, reg);
    check32("F9 density 127 bumps to 128",
            (int32_t)gx_fog_density(t, 0, 0, 0), 128);
}

static void test_fog_blend(void)
{
    /* 2x2 surface; only pixel 0 has the fog flag (attr bit 15).
     * fog color reg = 0x001F (r5=31, g5=b5=0, alpha=0):
     *   fogR = (0x001F<<1)&0x3E = 62 -> +1 = 63; fogG = fogB = 0; fogA = 0
     * density: fog offset 0x20000, all z below -> T[0] = reg[0] = 64
     * source pixel (20, 0, 0), alpha 31 (6-bit channels):
     *   r = (63*64 + 20*64) >> 7 = 5312>>7 = 41
     *   g = b = 0
     *   a = (0*64 + 31*64) >> 7 = 1984>>7 = 15 */
    uint32_t color[4], depth[4], attr[4];
    uint16_t edge[8];
    uint8_t reg[32];
    GxSurface sf = { .color = color, .depth = depth, .attr = attr, .w = 2, .h = 2 };
    GxPostEnv env;
    int i;

    memset(&env, 0, sizeof(env));
    for (i = 0; i < 32; i++) reg[i] = 0;
    reg[0] = 64;
    build_fog_table(env.fog_density, reg);
    env.edge = edge;
    memset(edge, 0, sizeof(edge));
    env.fog_color = 0x001F;
    env.fog_offset = 0x20000;
    env.fog_enable = 1;

    for (i = 0; i < 4; i++) {
        color[i] = 20 | (31u << 24);
        depth[i] = 0x10000; /* below fog offset */
        attr[i] = 0;
    }
    attr[0] = (1u << 15); /* fog enabled for this pixel only */

    gx_post_pass(&sf, &env);
    check32("F10 fogged r", (int32_t)(color[0] & 0x3F), 41);
    check32("F10 fogged g", (int32_t)((color[0] >> 8) & 0x3F), 0);
    check32("F10 fogged b", (int32_t)((color[0] >> 16) & 0x3F), 0);
    check32("F10 fogged a", (int32_t)(color[0] >> 24), 15);
    check32("F11 unfogged pixel", (int32_t)(color[1] & 0x3F), 20);
    check32("F11 unfogged alpha", (int32_t)(color[1] >> 24), 31);

    /* F12: fog alpha only (DISP3DCNT bit 6): color unchanged, alpha blends */
    for (i = 0; i < 4; i++) {
        color[i] = 20 | (31u << 24);
        attr[i] = 0;
    }
    attr[0] = (1u << 15);
    env.fog_alpha_only = 1;
    gx_post_pass(&sf, &env);
    check32("F12 alpha-only r", (int32_t)(color[0] & 0x3F), 20);
    check32("F12 alpha-only a", (int32_t)(color[0] >> 24), 15);
    env.fog_alpha_only = 0;
}

static void test_edge_marking(void)
{
    /* 3x3 surface (the center pixel is the only one not touching the
     * offscreen border).  Pixel (1,1) has opaque polygon ID 8 (edge color
     * index 1 = 0x001F red: er = (0x1F<<1)&0x3E = 62 -> 63, eg = eb = 0),
     * depth 100 (in front); the rest polygon ID 0, depth 200.  Clear attr
     * polyid 0, clear depth 1000 (offscreen border, ClearBuffers 1628-1652).
     *
     * (1,1): neighbor (1,0) has a different ID and 100 < 200 -> marked.
     * The ID-0 pixels: (1,1) differs but 200 < 100 fails; other neighbors
     * and the border share ID 0 -> unmarked. */
    uint32_t color[9], depth[9], attr[9];
    uint16_t edge[8];
    GxSurface sf = { .color = color, .depth = depth, .attr = attr, .w = 3, .h = 3 };
    GxPostEnv env;
    int i;

    memset(&env, 0, sizeof(env));
    memset(edge, 0, sizeof(edge));
    edge[1] = 0x001F;
    env.edge = edge;
    env.edge_enable = 1;
    env.clear_attr = 0;      /* clear polygon ID 0 */
    env.clear_depth = 1000;

    for (i = 0; i < 9; i++) {
        color[i] = 20 | (31u << 24); /* r=20, alpha 31 */
        depth[i] = 200;
        attr[i] = 0xF;
    }
    attr[4] = (8u << 24) | 0xF; /* center: polyid 8, edge flags set */
    depth[4] = 100;

    gx_post_pass(&sf, &env);
    check32("E1 marked edge r", (int32_t)(color[4] & 0x3F), 63);
    check32("E1 marked edge g", (int32_t)((color[4] >> 8) & 0x3F), 0);
    check32("E1 marked edge b", (int32_t)((color[4] >> 16) & 0x3F), 0);
    check32("E1 alpha kept", (int32_t)(color[4] >> 24), 31);
    check32("E2 behind unmarked (1,0)", (int32_t)(color[1] & 0x3F), 20);
    check32("E2 behind unmarked (0,1)", (int32_t)(color[3] & 0x3F), 20);
    check32("E2 behind unmarked (2,1)", (int32_t)(color[5] & 0x3F), 20);
    check32("E2 corner unmarked", (int32_t)(color[0] & 0x3F), 20);

    /* E3: pixels without edge flags (attr bits 0-3 clear) are never
     * marked, even at a polyID boundary in front (1459-1469) */
    for (i = 0; i < 9; i++) {
        color[i] = 20 | (31u << 24);
        depth[i] = 200;
        attr[i] = 0;
    }
    attr[4] = (8u << 24); /* no edge flags */
    depth[4] = 100;
    gx_post_pass(&sf, &env);
    check32("E3 no edge flags -> no mark", (int32_t)(color[4] & 0x3F), 20);

    /* E4: same polygon ID everywhere (including the clear/border ID) ->
     * no mark */
    env.clear_attr = (8u << 24);
    for (i = 0; i < 9; i++) {
        color[i] = 20 | (31u << 24);
        depth[i] = 200;
        attr[i] = (8u << 24) | 0xF;
    }
    depth[4] = 100;
    gx_post_pass(&sf, &env);
    check32("E4 same polyid -> no mark", (int32_t)(color[4] & 0x3F), 20);
    check32("E4 corner unmarked", (int32_t)(color[0] & 0x3F), 20);

    /* E5: screen-border neighbors carry the clear polyid (0): pixels
     * touching the border are marked (8 != 0, 100 < 1000); the center is
     * not, all its neighbors share polyid 8 */
    env.clear_attr = 0;
    gx_post_pass(&sf, &env);
    check32("E5 border pixel marked", (int32_t)(color[0] & 0x3F), 63);
    check32("E5 center unmarked", (int32_t)(color[4] & 0x3F), 20);
}

static void test_edge_breaks_aa_coverage(void)
{
    /* E6: edge marking breaks antialiasing coverage.  melonDS forces the
     * coverage field to 16 on every edge-marked pixel BEFORE the AA pass
     * (GPU3D_Soft.cpp:1486-1487: AttrBuffer[pixeladdr] =
     * (attr & 0xFFFFE0FF) | 0x00001000), so a marked pixel blends
     * (coverage+1)/32 = 17/32 against the AA second layer regardless of the
     * coverage the rasterizer assigned.
     *
     * 3x3; only the center has edge flags, with rasterized coverage 8.  It
     * gets edge-marked (polyid 8 vs 0, z 100 < 200) -> edge color
     * (63, 0, 0) alpha 31; the second layer at (1,1) is (0, 0, 63) alpha 31.
     * After the post pass (AA blend, coverage 16 -> 17):
     *   r = (63*17 + 0*15) >> 5 = 1071 >> 5 = 33
     *   g = 0
     *   b = (0*17 + 63*15) >> 5 = 945 >> 5 = 29
     *   a = (31*17 + 31*15) >> 5 = 992 >> 5 = 31
     * (without the forced coverage the rasterized 8 would blend 9/32:
     *   r = (63*9 + 0*23) >> 5 = 17) */
    uint32_t color[9], depth[9], attr[9];
    uint32_t color2[9], depth2[9], attr2[9];
    uint16_t edge[8];
    GxSurface sf = { .color = color, .depth = depth, .attr = attr,
                     .color2 = color2, .depth2 = depth2, .attr2 = attr2,
                     .w = 3, .h = 3 };
    GxPostEnv env;
    int i;

    memset(&env, 0, sizeof(env));
    memset(edge, 0, sizeof(edge));
    edge[1] = 0x001F;
    env.edge = edge;
    env.edge_enable = 1;
    env.aa_enable = 1;
    env.clear_attr = 0;
    env.clear_depth = 1000;

    for (i = 0; i < 9; i++) {
        color[i] = 20 | (31u << 24);
        color2[i] = 20 | (31u << 24);
        depth[i] = 200;
        depth2[i] = 200;
        attr[i] = 0;   /* no edge flags: edge + AA passes skip these */
        attr2[i] = 0;
    }
    attr[4] = (8u << 24) | (8u << 8) | 0xF; /* polyid 8, coverage 8, edges */
    depth[4] = 100;
    color2[4] = (63u << 16) | (31u << 24); /* (0, 0, 63) alpha 31 */

    gx_post_pass(&sf, &env);
    check32("E6 coverage forced to 16", (int32_t)((attr[4] >> 8) & 0x1F), 16);
    check32("E6 blend 17/32 r", (int32_t)(color[4] & 0x3F), 33);
    check32("E6 blend 17/32 g", (int32_t)((color[4] >> 8) & 0x3F), 0);
    check32("E6 blend 17/32 b", (int32_t)((color[4] >> 16) & 0x3F), 29);
    check32("E6 blend 17/32 a", (int32_t)(color[4] >> 24), 31);
}

static void test_fog_second_layer(void)
{
    /* F13: fog is applied to the AA second layer ("lower pixel") too
     * (GPU3D_Soft.cpp:1500-1503 "fog is applied to the topmost two pixels",
     * 1527-1570): the lower pixel is fogged when the TOP pixel has edge
     * flags (attr & 0xF) and the lower pixel itself has the fog flag
     * (attr bit 15), with the density computed from the lower pixel's own
     * depth.  The top pixel's fog is independent (its own bit 15).
     *
     * Fog env like F10: fog color 0x001F -> fogR = (0x1F<<1)&0x3E = 62+1
     * = 63, fogA = 0.  Table from reg[i]=i, reg[0]=10, reg[1]=20 (fog10):
     *   depth2 = 0x120000 -> z' = (0x100000>>2) = 0x40000 -> id 2, frac 0
     *            -> density = T[2] = reg[1] = 20
     *   depth  = 0x10000 (below fog offset 0x20000) -> density = T[0] = 10
     * Source (20, 0, 0) alpha 31, fogA 0:
     *   density 10: r = (63*10 + 20*118) >> 7 = 2990 >> 7 = 23
     *               a = (0*10 + 31*118) >> 7 = 3658 >> 7 = 28
     *   density 20: r = (63*20 + 20*108) >> 7 = 3420 >> 7 = 26
     *               a = (0*20 + 31*108) >> 7 = 3348 >> 7 = 26 */
    uint32_t color[4], depth[4], attr[4];
    uint32_t color2[4], depth2[4], attr2[4];
    uint16_t edge[8];
    uint8_t reg[32];
    GxSurface sf = { .color = color, .depth = depth, .attr = attr,
                     .color2 = color2, .depth2 = depth2, .attr2 = attr2,
                     .w = 2, .h = 2 };
    GxPostEnv env;
    int i;

    memset(&env, 0, sizeof(env));
    for (i = 0; i < 32; i++) reg[i] = (uint8_t)i;
    reg[0] = 10;
    reg[1] = 20;
    reg[31] = 30;
    build_fog_table(env.fog_density, reg);
    env.edge = edge;
    memset(edge, 0, sizeof(edge));
    env.fog_color = 0x001F;
    env.fog_offset = 0x20000;
    env.fog_enable = 1;

    for (i = 0; i < 4; i++) {
        color[i] = 20 | (31u << 24);
        color2[i] = 20 | (31u << 24);
        depth[i] = 0x10000;   /* below fog offset -> density 10 */
        depth2[i] = 0x120000; /* -> density 20 */
        attr[i] = 0xF;        /* top: edge flags set, no fog flag */
        attr2[i] = 1u << 15;  /* lower: fog flag set */
    }
    attr[0] = 0xF | (1u << 15); /* pixel 0: both layers fogged */
    attr[1] = 1u << 15;         /* pixel 1: top has NO edge flags -> lower
                                 * pixel must NOT be fogged (1528) */
    attr2[2] = 0;               /* pixel 2: lower has no fog flag -> not
                                 * fogged (1533-1534) */
    /* pixel 3: top not fogged (no bit 15), lower fogged -> shows the lower
     * pass is independent of the top pixel's fog flag */

    gx_post_pass(&sf, &env);
    check32("F13 top fogged r", (int32_t)(color[0] & 0x3F), 23);
    check32("F13 top fogged a", (int32_t)(color[0] >> 24), 28);
    check32("F13 lower fogged r", (int32_t)(color2[0] & 0x3F), 26);
    check32("F13 lower fogged a", (int32_t)(color2[0] >> 24), 26);
    check32("F13 gate top edge flags r", (int32_t)(color2[1] & 0x3F), 20);
    check32("F13 gate top edge flags a", (int32_t)(color2[1] >> 24), 31);
    check32("F13 gate lower fog flag r", (int32_t)(color2[2] & 0x3F), 20);
    check32("F13 gate lower fog flag a", (int32_t)(color2[2] >> 24), 31);
    check32("F13 lower-only fogged r", (int32_t)(color2[3] & 0x3F), 26);
    check32("F13 lower-only fogged a", (int32_t)(color2[3] >> 24), 26);
    check32("F13 lower-only top clean", (int32_t)(color[3] & 0x3F), 20);
}

int main(void)
{
    test_fog_density();
    test_fog_blend();
    test_edge_marking();
    test_edge_breaks_aa_coverage();
    test_fog_second_layer();

    if (failures) {
        fprintf(stderr, "test_post: %d FAILURES\n", failures);
        return 1;
    }
    fprintf(stderr, "test_post: all checks passed\n");
    return 0;
}
