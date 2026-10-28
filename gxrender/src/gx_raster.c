/* gx_raster.c — deterministic DS-style software rasterizer.
 *
 * Shading/blend/alpha arithmetic follows melonDS src/GPU3D_Soft.cpp
 * (RenderPixel / AlphaBlend); re-implemented here in our own code.
 * Fill rule: barycentric edge functions with the top-left convention
 * (pixel centers in 1/16 px).  Everything is integer math, so repeated
 * runs over the same input produce byte-identical output.
 */
#include <stdio.h>
#include <string.h>
#include "gx_raster.h"

void gx_surface_clear(GxSurface *sf, uint32_t color6, uint32_t depth)
{
    int i, n = sf->w * sf->h;
    for (i = 0; i < n; i++) {
        sf->color[i] = color6;
        sf->depth[i] = depth;
    }
}

int64_t gx_poly_area2(const GxPolygon *poly)
{
    const GxRasVertex *a = &poly->v[0], *b = &poly->v[1], *c = &poly->v[2];
    return (int64_t)(b->sx - a->sx) * (c->sy - a->sy) -
           (int64_t)(c->sx - a->sx) * (b->sy - a->sy);
}

uint32_t gx_shade_pixel(const GxPolygon *poly, uint8_t vr, uint8_t vg,
                        uint8_t vb, int32_t s, int32_t t,
                        const GxRasterEnv *env)
{
    uint8_t r, g, b, a;
    uint32_t blendmode = (poly->attr >> 4) & 0x3;
    uint32_t polyalpha = (poly->attr >> 16) & 0x1F;
    int wireframe = (polyalpha == 0);
    int textured = env->tex_enable && env->tex &&
                   ((poly->texparam >> 26) & 0x7) != 0;

    if (blendmode == 2) {
        if (env->highlight) {
            /* highlight mode: all vertex channels set to the red value;
             * the toon color is added after texture blending */
            vg = vr;
            vb = vr;
        } else {
            /* toon mode: vertex color replaced by toon table entry */
            uint16_t tooncolor = env->toon[vr >> 1];
            vr = (uint8_t)((tooncolor << 1) & 0x3E); if (vr) vr++;
            vg = (uint8_t)((tooncolor >> 4) & 0x3E); if (vg) vg++;
            vb = (uint8_t)((tooncolor >> 9) & 0x3E); if (vb) vb++;
        }
    }

    if (textured) {
        uint8_t tr, tg, tb, talpha = 0;
        uint16_t tcolor = 0;
        gx_tex_lookup(env->tex, poly->texparam, poly->texpal, s, t,
                      &tcolor, &talpha);

        tr = (uint8_t)((tcolor << 1) & 0x3E); if (tr) tr++;
        tg = (uint8_t)((tcolor >> 4) & 0x3E); if (tg) tg++;
        tb = (uint8_t)((tcolor >> 9) & 0x3E); if (tb) tb++;

        if (blendmode & 0x1) {
            /* decal */
            if (talpha == 0) {
                r = vr; g = vg; b = vb;
            } else if (talpha == 31) {
                r = tr; g = tg; b = tb;
            } else {
                r = (uint8_t)(((tr * talpha) + (vr * (31 - talpha))) >> 5);
                g = (uint8_t)(((tg * talpha) + (vg * (31 - talpha))) >> 5);
                b = (uint8_t)(((tb * talpha) + (vb * (31 - talpha))) >> 5);
            }
            a = (uint8_t)polyalpha;
        } else {
            /* modulate */
            r = (uint8_t)((((int)tr + 1) * ((int)vr + 1) - 1) >> 6);
            g = (uint8_t)((((int)tg + 1) * ((int)vg + 1) - 1) >> 6);
            b = (uint8_t)((((int)tb + 1) * ((int)vb + 1) - 1) >> 6);
            a = (uint8_t)((((int)talpha + 1) * ((int)polyalpha + 1) - 1) >> 5);
        }
    } else {
        r = vr; g = vg; b = vb;
        a = (uint8_t)polyalpha;
    }

    if (blendmode == 2 && env->highlight) {
        /* add the toon color of the original (red) vertex value */
        uint16_t tooncolor = env->toon[vr >> 1];
        uint8_t hvr = (uint8_t)((tooncolor << 1) & 0x3E); if (hvr) hvr++;
        uint8_t hvg = (uint8_t)((tooncolor >> 4) & 0x3E); if (hvg) hvg++;
        uint8_t hvb = (uint8_t)((tooncolor >> 9) & 0x3E); if (hvb) hvb++;

        if ((uint16_t)r + hvr > 63) r = 63; else r = (uint8_t)(r + hvr);
        if ((uint16_t)g + hvg > 63) g = 63; else g = (uint8_t)(g + hvg);
        if ((uint16_t)b + hvb > 63) b = 63; else b = (uint8_t)(b + hvb);
    }

    if (wireframe) a = 31;

    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) |
           ((uint32_t)a << 24);
}

static uint32_t alpha_blend(uint32_t src, uint32_t dst, uint32_t alpha,
                            int blend_enable)
{
    uint32_t dstalpha = dst >> 24;
    uint32_t srcR, srcG, srcB;

    if (dstalpha == 0)
        return src;

    srcR = src & 0x3F;
    srcG = (src >> 8) & 0x3F;
    srcB = (src >> 16) & 0x3F;

    if (blend_enable) {
        uint32_t dstR = dst & 0x3F;
        uint32_t dstG = (dst >> 8) & 0x3F;
        uint32_t dstB = (dst >> 16) & 0x3F;

        alpha++;
        srcR = ((srcR * alpha) + (dstR * (32 - alpha))) >> 5;
        srcG = ((srcG * alpha) + (dstG * (32 - alpha))) >> 5;
        srcB = ((srcB * alpha) + (dstB * (32 - alpha))) >> 5;
        alpha--;
    }

    if (alpha > dstalpha)
        dstalpha = alpha;

    return srcR | (srcG << 8) | (srcB << 16) | (dstalpha << 24);
}

/* edge function (u->v), sample p; positive inside for area2 > 0 winding */
static int64_t edge_fn(const GxRasVertex *u, const GxRasVertex *v,
                       int64_t px, int64_t py)
{
    return (int64_t)(v->sx - u->sx) * (py - u->sy) -
           (int64_t)(v->sy - u->sy) * (px - u->sx);
}

/* top-left fill rule: a pixel exactly on an edge counts only for top and
 * left edges (for the area2 > 0 winding used below).  Top = horizontal,
 * running right; left = any edge running up-screen; everything else is
 * exclusive.  Directions reverse on the shared edge of the neighbor
 * triangle, so shared edges stay gapless. */
static int edge_inclusive(const GxRasVertex *u, const GxRasVertex *v)
{
    if (u->sy == v->sy && u->sx < v->sx) return 1; /* top edge */
    if (v->sy < u->sy) return 1;                   /* left edge (incl. diagonal) */
    return 0;
}

static void raster_tri(GxSurface *sf, const GxRasVertex *va,
                       const GxRasVertex *vb, const GxRasVertex *vc,
                       const GxPolygon *poly, const GxRasterEnv *env)
{
    int64_t area2 = (int64_t)(vb->sx - va->sx) * (vc->sy - va->sy) -
                    (int64_t)(vc->sx - va->sx) * (vb->sy - va->sy);
    int64_t minx16, maxx16, miny16, maxy16;
    int px0, px1, py0, py1, px, py;
    int incl_bc, incl_ca, incl_ab;
    int wireframe, polyalpha, alpha_ref;

    if (area2 == 0)
        return;
    if (area2 < 0) {
        const GxRasVertex *tmp = vb;
        vb = vc;
        vc = tmp;
        area2 = -area2;
    }

    polyalpha = (int)((poly->attr >> 16) & 0x1F);
    wireframe = (polyalpha == 0);
    alpha_ref = env->alpha_ref;

    minx16 = gx_min32(va->sx, gx_min32(vb->sx, vc->sx));
    maxx16 = gx_max32(va->sx, gx_max32(vb->sx, vc->sx));
    miny16 = gx_min32(va->sy, gx_min32(vb->sy, vc->sy));
    maxy16 = gx_max32(va->sy, gx_max32(vb->sy, vc->sy));

    px0 = (int)(((minx16 - 8) / 16) - 1);
    px1 = (int)(((maxx16 + 8) / 16) + 1);
    py0 = (int)(((miny16 - 8) / 16) - 1);
    py1 = (int)(((maxy16 + 8) / 16) + 1);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > sf->w - 1) px1 = sf->w - 1;
    if (py1 > sf->h - 1) py1 = sf->h - 1;
    if (px0 > px1 || py0 > py1)
        return;

    incl_bc = edge_inclusive(vb, vc);
    incl_ca = edge_inclusive(vc, va);
    incl_ab = edge_inclusive(va, vb);

    for (py = py0; py <= py1; py++) {
        for (px = px0; px <= px1; px++) {
            int64_t sx = (int64_t)px * GX_SCREEN_ONE + GX_SCREEN_ONE / 2;
            int64_t sy = (int64_t)py * GX_SCREEN_ONE + GX_SCREEN_ONE / 2;
            int64_t w0 = edge_fn(vb, vc, sx, sy); /* opposite va */
            int64_t w1 = edge_fn(vc, va, sx, sy);
            int64_t w2 = edge_fn(va, vb, sx, sy);
            int64_t ia, ib, ic, isum, num;
            int32_t depth, s, t;
            uint8_t c5, vr, vg, vb_;
            uint32_t pix, pix_a;
            size_t addr;

            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            if ((w0 == 0 && !incl_bc) || (w1 == 0 && !incl_ca) ||
                (w2 == 0 && !incl_ab))
                continue;

            /* perspective-correct attributes via 1/w (same >>8 shift on
             * numerator and denominator so the ratio keeps attr units):
             * attr = sum(w_i * A_i) / sum(w_i * I_i),
             * A_i = (invw_i * attr_i) >> 8, I_i = invw_i >> 8 */
            ia = w0 * (va->invw >> 8);
            ib = w1 * (vb->invw >> 8);
            ic = w2 * (vc->invw >> 8);
            isum = ia + ib + ic;
            if (isum <= 0)
                continue;

            /* depth: linear in screen space (Z = z/w, W = clip w) */
            depth = (int32_t)((w0 * va->depth + w1 * vb->depth +
                               w2 * vc->depth) / area2);

            addr = (size_t)py * sf->w + px;
            if (depth >= (int32_t)sf->depth[addr])
                continue;

            s = (int32_t)((w0 * ((va->invw * va->s) >> 8) +
                           w1 * ((vb->invw * vb->s) >> 8) +
                           w2 * ((vc->invw * vc->s) >> 8) + isum / 2) / isum);
            t = (int32_t)((w0 * ((va->invw * va->t) >> 8) +
                           w1 * ((vb->invw * vb->t) >> 8) +
                           w2 * ((vc->invw * vc->t) >> 8) + isum / 2) / isum);

            num = w0 * ((va->invw * va->r) >> 8) +
                  w1 * ((vb->invw * vb->r) >> 8) +
                  w2 * ((vc->invw * vc->r) >> 8);
            c5 = (uint8_t)gx_clamp32((num + isum / 2) / isum, 0, 31);
            vr = gx_c5_to_c6(c5);
            num = w0 * ((va->invw * va->g) >> 8) +
                  w1 * ((vb->invw * vb->g) >> 8) +
                  w2 * ((vc->invw * vc->g) >> 8);
            c5 = (uint8_t)gx_clamp32((num + isum / 2) / isum, 0, 31);
            vg = gx_c5_to_c6(c5);
            num = w0 * ((va->invw * va->b) >> 8) +
                  w1 * ((vb->invw * vb->b) >> 8) +
                  w2 * ((vc->invw * vc->b) >> 8);
            c5 = (uint8_t)gx_clamp32((num + isum / 2) / isum, 0, 31);
            vb_ = gx_c5_to_c6(c5);

            pix = gx_shade_pixel(poly, vr, vg, vb_, s, t, env);
            pix_a = pix >> 24;

            /* 1-bit alpha test: fully transparent pixels are dropped
             * (wireframe polygons draw at forced alpha 31) */
            if ((int)pix_a <= alpha_ref && !wireframe)
                continue;

            if (pix_a >= 31) {
                sf->color[addr] = pix;
            } else {
                sf->color[addr] = alpha_blend(pix, sf->color[addr], pix_a,
                                              env->alpha_blend);
            }
            sf->depth[addr] = (uint32_t)depth;
        }
    }
}

void gx_raster_polygon(GxSurface *sf, const GxPolygon *poly,
                       const GxRasterEnv *env)
{
    raster_tri(sf, &poly->v[0], &poly->v[1], &poly->v[2], poly, env);
    if (poly->nv == 4)
        raster_tri(sf, &poly->v[0], &poly->v[2], &poly->v[3], poly, env);
}

int gx_write_ppm(const char *path, const GxSurface *sf)
{
    FILE *f = fopen(path, "wb");
    int i, n;
    if (!f)
        return -1;
    fprintf(f, "P6\n%d %d\n255\n", sf->w, sf->h);
    n = sf->w * sf->h;
    for (i = 0; i < n; i++) {
        uint32_t p = sf->color[i];
        uint8_t rgb[3];
        rgb[0] = gx_c6_to_c8((uint8_t)(p & 0x3F));
        rgb[1] = gx_c6_to_c8((uint8_t)((p >> 8) & 0x3F));
        rgb[2] = gx_c6_to_c8((uint8_t)((p >> 16) & 0x3F));
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
