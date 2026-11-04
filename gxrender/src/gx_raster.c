/* gx_raster.c — deterministic DS-style software rasterizer.
 *
 * Faithful port of the melonDS scanline rasterizer model (our own code):
 *  - Interpolator: 8/9-bit perspective-factor attribute interpolation with
 *    the linear fast path            (GPU3D_Soft.h:74-227)
 *  - Slope: integer-pixel span edges with the X-major interpoffset quirk
 *                                    (GPU3D_Soft.h:230-431)
 *  - span fill: left edge / interior / right edge with the hardware fill
 *    rules (edges always filled when AA or edge marking is on)
 *                                    (GPU3D_Soft.cpp:940-1395)
 *  - pixel plotting: opaque vs translucent (PlotTranslucentPixel,
 *                                    GPU3D_Soft.cpp:565-601), AA second
 *    layer push-down and underneath-depth-test fallback
 *  - shading: RenderPixel arithmetic (GPU3D_Soft.cpp:457-562)
 *  - frame post pass: edge marking, fog, antialiasing
 *                                    (GPU3D_Soft.cpp:1453-1621)
 * Shadow volumes / stencil (RenderShadowMaskScanline) are not implemented;
 * mode-3 polygons are rendered as ordinary translucent polygons.
 * Everything is integer math: repeated runs produce byte-identical output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_raster.h"

/* Phase-5BR: cumulative color-buffer stores and the last polygon's count.
 * Plain globals (not Gxd state) so the savestate layout is untouched. */
unsigned long long gx_ras_pixels_total;
unsigned long long gx_ras_last_pixels;

void gx_surface_clear(GxSurface *sf, uint32_t color6, uint32_t depth,
                      uint32_t attr0)
{
    int i, n = sf->w * sf->h;
    for (i = 0; i < n; i++) {
        sf->color[i] = color6;
        sf->depth[i] = depth;
        if (sf->attr)
            sf->attr[i] = attr0;
    }
    /* note: the AA second layer is NOT cleared here (melonDS ClearBuffers
     * only clears the visible layer; the second layer persists) */
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

/* depth test (melonDS GPU3D_Soft.cpp:385-425):
 * - POLYGON_ATTR bit 14 set: DepthTest_Equal_Z/W, pass when the new depth is
 *   within +-0x200 (Z mode) / +-0xFF (W mode) of the stored depth;
 * - front-facing polygon over an OPAQUE BACK-FACING pixel: ties pass
 *   (DepthTest_LessThan_FrontFacing);
 * - otherwise strictly closer (first-submitted wins ties). */
static int depth_pass(const GxPolygon *poly, uint32_t dstattr,
                      int32_t dstz, int32_t z)
{
    if (poly->attr & (1u << 14)) {
        int32_t lim = poly->wbuffer ? 0xFF : 0x200;
        uint32_t diff = (uint32_t)(dstz - z + lim);
        return diff <= (uint32_t)(lim * 2);
    }
    if (poly->facing_view && (dstattr & 0x00400010u) == 0x00000010u)
        return z <= dstz;
    return z < dstz;
}

/* ------------------------------------------------------------------ */
/* Interpolator (port of GPU3D_Soft.h:74-227)                          */

typedef struct {
    int32_t x0, x1, xdiff, x;
    int shift; /* 8 = X direction, 9 = Y direction */
    int linear, wbuffer;
    int32_t xrecip_z;
    int32_t w0n, w0d, w1d;
    uint32_t yfactor;
} GxInterp;

static void interp_setup(GxInterp *ip, int32_t x0, int32_t x1, int32_t w0,
                         int32_t w1, int wbuffer, int dir)
{
    uint32_t mask = dir ? 0x7Eu : 0x7Fu;
    ip->x0 = x0;
    ip->x1 = x1;
    ip->xdiff = x1 - x0;
    ip->wbuffer = wbuffer;
    ip->x = 0;
    ip->yfactor = 0;
    ip->xrecip_z = (ip->xdiff != 0) ? (int32_t)((1 << 22) / ip->xdiff) : 0;

    /* linear mode is used if both W values are equal and have low-order
     * bits cleared (0-6 along X, 1-6 along Y) */
    ip->linear = (w0 == w1) && !(w0 & (int32_t)mask) && !(w1 & (int32_t)mask);

    if (dir) {
        ip->w0n = w0 >> 1;
        ip->w0d = (w0 + ((w0 & ~w1) & 1)) >> 1;
        ip->w1d = w1 >> 1;
        ip->shift = 9;
    } else {
        ip->w0n = w0;
        ip->w0d = w0;
        ip->w1d = w1;
        ip->shift = 8;
    }
}

static void interp_setx(GxInterp *ip, int32_t x)
{
    x -= ip->x0;
    ip->x = x;
    if ((ip->xdiff != 0) && ((!ip->linear) || ip->wbuffer)) {
        uint32_t num = ((uint32_t)(x * ip->w0n)) << ip->shift;
        int32_t den = (int32_t)(x * ip->w0d) + ((ip->xdiff - x) * ip->w1d);
        /* this seems to be a proper division on hardware */
        if (den == 0) ip->yfactor = 0;
        else          ip->yfactor = num / (uint32_t)den;
    }
}

static int32_t interp_value(const GxInterp *ip, int32_t y0, int32_t y1)
{
    if (ip->xdiff == 0 || y0 == y1)
        return y0;
    if (!ip->linear) {
        /* perspective-correct approx. interpolation */
        if (y0 < y1)
            return y0 + (int32_t)(((int64_t)(y1 - y0) * ip->yfactor) >> ip->shift);
        else
            return y1 + (int32_t)(((int64_t)(y0 - y1) *
                    ((int64_t)(1 << ip->shift) - ip->yfactor)) >> ip->shift);
    } else {
        /* linear interpolation */
        if (y0 < y1)
            return y0 + (int32_t)((int64_t)(y1 - y0) * ip->x / ip->xdiff);
        else
            return y1 + (int32_t)((int64_t)(y0 - y1) * (ip->xdiff - ip->x) / ip->xdiff);
    }
}

static int32_t interp_z(const GxInterp *ip, int32_t z0, int32_t z1)
{
    if (ip->xdiff == 0 || z0 == z1)
        return z0;
    if (ip->wbuffer) {
        /* W-buffering: perspective-correct approx. interpolation */
        if (z0 < z1)
            return z0 + (int32_t)(((int64_t)(z1 - z0) * ip->yfactor) >> ip->shift);
        else
            return z1 + (int32_t)(((int64_t)(z0 - z1) *
                    ((int64_t)(1 << ip->shift) - ip->yfactor)) >> ip->shift);
    } else {
        /* Z-buffering: linear interpolation */
        int32_t base, disp, factor, shift = 0;
        if (z0 < z1) {
            base = z0;
            disp = z1 - z0;
            factor = ip->x;
        } else {
            base = z1;
            disp = z0 - z1;
            factor = ip->xdiff - ip->x;
        }
        if (ip->shift == 9) { /* Y direction */
            while (disp > 0x3FF) {
                disp >>= 1;
                shift++;
            }
            return base + (int32_t)(((((int64_t)disp * factor * ip->xrecip_z) >> 22) << shift));
        } else {
            disp >>= 9;
            return base + (int32_t)(((int64_t)disp * factor * ip->xrecip_z) >> 13);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Slope (port of GPU3D_Soft.h:230-431)                                */

typedef struct {
    int32_t Increment;
    int Negative, XMajor;
    GxInterp Interp;
    /* private */
    int32_t x0, xmin, xmax, xlen, ylen, dx, y, xcov_incr;
} GxSlope;

static int32_t slope_xval(const GxSlope *sl)
{
    int32_t ret = sl->Negative ? (sl->x0 - (sl->dx >> 18))
                               : (sl->x0 + (sl->dx >> 18));
    if (ret < sl->xmin) ret = sl->xmin;
    else if (ret > sl->xmax) ret = sl->xmax;
    return ret;
}

static int32_t slope_setup_dummy(GxSlope *sl, int32_t x0, int wbuffer)
{
    sl->dx = 0;
    sl->x0 = x0;
    sl->xmin = x0;
    sl->xmax = x0;
    sl->Increment = 0;
    sl->Negative = 0;
    sl->XMajor = 0;
    sl->xlen = 1;
    sl->ylen = 0;
    sl->y = 0;
    interp_setup(&sl->Interp, 0, 0, 0, 0, wbuffer, 1);
    interp_setx(&sl->Interp, 0);
    sl->xcov_incr = 0;
    return x0;
}

static int32_t slope_setup(GxSlope *sl, int side, int32_t x0, int32_t x1,
                           int32_t y0, int32_t y1, int32_t w0, int32_t w1,
                           int32_t y, int wbuffer)
{
    int32_t x;
    int interpoffset;

    sl->x0 = x0;
    sl->y = y;
    if (x1 > x0) {
        sl->xmin = x0;
        sl->xmax = x1 - 1;
        sl->Negative = 0;
    } else if (x1 < x0) {
        sl->xmin = x1;
        sl->xmax = x0 - 1;
        sl->Negative = 1;
    } else {
        sl->xmin = x0;
        sl->xmax = x0;
        sl->Negative = 0;
    }
    sl->xlen = sl->xmax + 1 - sl->xmin;
    sl->ylen = y1 - y0;

    /* slope increment has a 18-bit fractional part */
    if (sl->ylen == 0)
        sl->Increment = 0;
    else if (sl->ylen == sl->xlen && sl->xlen != 1)
        sl->Increment = 0x40000;
    else {
        int32_t yrecip = (int32_t)((1 << 18) / sl->ylen);
        sl->Increment = (x1 - x0) * yrecip;
        if (sl->Increment < 0) sl->Increment = -sl->Increment;
    }
    sl->XMajor = (sl->Increment > 0x40000);

    if (side) {
        /* right */
        if (sl->XMajor)      sl->dx = sl->Negative ? (0x20000 + 0x40000) : (sl->Increment - 0x20000);
        else if (sl->Increment != 0) sl->dx = sl->Negative ? 0x40000 : 0;
        else                 sl->dx = 0;
    } else {
        /* left */
        if (sl->XMajor)      sl->dx = sl->Negative ? ((sl->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (sl->Increment != 0) sl->dx = sl->Negative ? 0x40000 : 0;
        else                 sl->dx = 0;
    }
    sl->dx += (y - y0) * sl->Increment;

    x = slope_xval(sl);

    /* X-major edges interpolate attributes offset by one row */
    interpoffset = (sl->Increment >= 0x40000) && (side ^ sl->Negative);
    interp_setup(&sl->Interp, y0 - interpoffset, y1 - interpoffset,
                 w0, w1, wbuffer, 1);
    interp_setx(&sl->Interp, y);

    /* used for calculating AA coverage */
    if (sl->XMajor) sl->xcov_incr = (sl->ylen << 10) / sl->xlen;
    else           sl->xcov_incr = 0;

    return x;
}

static int32_t slope_step(GxSlope *sl)
{
    int32_t x;
    sl->dx += sl->Increment;
    sl->y++;
    x = slope_xval(sl);
    interp_setx(&sl->Interp, sl->y);
    return x;
}

static void slope_edge_params(const GxSlope *sl, int side, int swapped,
                              int32_t *length, int32_t *coverage)
{
    if (sl->XMajor) {
        if (!swapped || side) {
            if (side ^ sl->Negative)
                *length = (sl->dx >> 18) - ((sl->dx - sl->Increment) >> 18);
            else
                *length = ((sl->dx + sl->Increment) >> 18) - (sl->dx >> 18);
        }
        {
            int32_t startx = sl->dx >> 18;
            int32_t startcov;
            if (sl->Negative) startx = sl->xlen - startx;
            if (side)         startx = startx - *length + 1;
            startcov = (int32_t)((((int64_t)startx << 10) + 0x1FF) * sl->ylen / sl->xlen);
            *coverage = (int32_t)((1u << 31) | ((uint32_t)(startcov & 0x3FF) << 12) |
                                  (uint32_t)(sl->xcov_incr & 0x3FF));
        }
        if (swapped) *length = 1;
    } else {
        *length = 1;
        if (sl->Increment == 0) {
            /* for some reason vertical edges' aa values are inverted too
             * when the edges are swapped */
            *coverage = swapped ? 0 : 31;
        } else {
            int32_t cov = ((sl->dx >> 9) + (sl->Increment >> 10)) >> 4;
            if ((cov >> 5) != (sl->dx >> 18)) cov = 31;
            cov &= 0x1F;
            if (swapped) {
                if (side ^ sl->Negative) cov = 0x1F - cov;
            } else {
                if (!(side ^ sl->Negative)) cov = 0x1F - cov;
            }
            *coverage = cov;
        }
    }
}

/* ------------------------------------------------------------------ */
/* polygon setup (port of GPU3D_Soft.cpp SetupPolygon, 604-709)        */

typedef struct {
    const GxPolygon *poly;
    GxSlope slope_l, slope_r;
    int32_t XL, XR;
    int cur_vl, cur_vr, next_vl, next_vr;
    int vtop, vbot;
    int32_t ytop, ybot;
} GxRPoly;

static void setup_left_edge(GxRPoly *rp, int32_t y)
{
    const GxPolygon *p = rp->poly;
    while (y >= p->v[rp->next_vl].sy && rp->cur_vl != rp->vbot) {
        rp->cur_vl = rp->next_vl;
        if (p->facing_view) {
            rp->next_vl = rp->cur_vl + 1;
            if (rp->next_vl >= p->nv) rp->next_vl = 0;
        } else {
            rp->next_vl = rp->cur_vl - 1;
            if (rp->next_vl < 0) rp->next_vl = p->nv - 1;
        }
    }
    rp->XL = slope_setup(&rp->slope_l, 0,
                         p->v[rp->cur_vl].sx, p->v[rp->next_vl].sx,
                         p->v[rp->cur_vl].sy, p->v[rp->next_vl].sy,
                         p->v[rp->cur_vl].wnorm, p->v[rp->next_vl].wnorm,
                         y, p->wbuffer);
}

static void setup_right_edge(GxRPoly *rp, int32_t y)
{
    const GxPolygon *p = rp->poly;
    while (y >= p->v[rp->next_vr].sy && rp->cur_vr != rp->vbot) {
        rp->cur_vr = rp->next_vr;
        if (p->facing_view) {
            rp->next_vr = rp->cur_vr - 1;
            if (rp->next_vr < 0) rp->next_vr = p->nv - 1;
        } else {
            rp->next_vr = rp->cur_vr + 1;
            if (rp->next_vr >= p->nv) rp->next_vr = 0;
        }
    }
    rp->XR = slope_setup(&rp->slope_r, 1,
                         p->v[rp->cur_vr].sx, p->v[rp->next_vr].sx,
                         p->v[rp->cur_vr].sy, p->v[rp->next_vr].sy,
                         p->v[rp->cur_vr].wnorm, p->v[rp->next_vr].wnorm,
                         y, p->wbuffer);
}

static void setup_polygon(GxRPoly *rp, const GxPolygon *poly)
{
    int nverts = poly->nv;
    int vtop = 0, vbot = 0;
    int32_t ytop = 192, ybot = 0;
    int32_t xtop = 256, xbot = 0; (void)xtop; (void)xbot;
    int i;

    /* bounds + VTop/VBottom (melonDS GPU3D.cpp:1476-1513) */
    for (i = 0; i < nverts; i++) {
        int32_t fx = poly->v[i].sx, fy = poly->v[i].sy;
        if (fy < ytop) {
            xtop = fx; ytop = fy; vtop = i;
        }
        if (fy > ybot || (fy == ybot && fx > xbot)) {
            xbot = fx; ybot = fy; vbot = i;
        }
    }
    rp->poly = poly;
    rp->vtop = vtop;
    rp->vbot = vbot;
    rp->ytop = ytop;
    rp->ybot = ybot;

    rp->cur_vl = vtop;
    rp->cur_vr = vtop;
    if (poly->facing_view) {
        rp->next_vl = rp->cur_vl + 1;
        if (rp->next_vl >= nverts) rp->next_vl = 0;
        rp->next_vr = rp->cur_vr - 1;
        if (rp->next_vr < 0) rp->next_vr = nverts - 1;
    } else {
        rp->next_vl = rp->cur_vl - 1;
        if (rp->next_vl < 0) rp->next_vl = nverts - 1;
        rp->next_vr = rp->cur_vr + 1;
        if (rp->next_vr >= nverts) rp->next_vr = 0;
    }

    if (ybot == ytop) {
        /* flat polygon: single scanline, horizontal vertex walk */
        vtop = 0; vbot = 0;
        i = 1;
        if (poly->v[i].sx < poly->v[vtop].sx) vtop = i;
        if (poly->v[i].sx > poly->v[vbot].sx) vbot = i;
        i = nverts - 1;
        if (poly->v[i].sx < poly->v[vtop].sx) vtop = i;
        if (poly->v[i].sx > poly->v[vbot].sx) vbot = i;
        rp->cur_vl = vtop; rp->next_vl = vtop;
        rp->cur_vr = vbot; rp->next_vr = vbot;
        rp->XL = slope_setup_dummy(&rp->slope_l, poly->v[vtop].sx, poly->wbuffer);
        rp->XR = slope_setup_dummy(&rp->slope_r, poly->v[vbot].sx, poly->wbuffer);
    } else {
        setup_left_edge(rp, ytop);
        setup_right_edge(rp, ytop);
    }
}

/* ------------------------------------------------------------------ */
/* pixel plotting (port of GPU3D_Soft.cpp PlotTranslucentPixel 565-601 */
/* and the opaque/translucent branches of RenderPolygonScanline)       */
/* pcolor/pdepth/pattr select the target layer: the top layer           */
/* (sf->color/depth/attr) or the AA second layer (sf->color2/depth2/   */
/* attr2).  melonDS indexes its second layer as pixeladdr+BufferSize   */
/* in unified 2*w*h buffers (GPU3D_Soft.h:475-477); GxSurface uses     */
/* separate per-layer w*h arrays at the same base addr instead.        */

static void plot_translucent(uint32_t *pcolor, uint32_t *pdepth,
                             uint32_t *pattr, size_t pixeladdr,
                             uint32_t color, int32_t z, uint32_t polyattr,
                             const GxRasterEnv *env)
{
    uint32_t dstattr = pattr ? pattr[pixeladdr] : 0;
    uint32_t attr;

    attr = (polyattr & 0xE0F0u) | ((polyattr >> 8) & 0xFF0000u) |
           (1u << 22) | (dstattr & 0xFF001F0Fu);

    /* skip if translucent polygon IDs are equal */
    if (pattr && (dstattr & 0x007F0000u) == (attr & 0x007F0000u))
        return;

    /* fog flag is taken from the opaque pixel underneath */
    if (!(dstattr & (1u << 15)))
        attr &= ~(1u << 15);

    color = alpha_blend(color, pcolor[pixeladdr], color >> 24,
                        env->alpha_blend);

    if (z != -1)
        pdepth[pixeladdr] = (uint32_t)z;

    pcolor[pixeladdr] = color;
    gx_ras_pixels_total++;
    if (pattr)
        pattr[pixeladdr] = attr;
}

/* ------------------------------------------------------------------ */
/* scanline render (port of GPU3D_Soft.cpp RenderPolygonScanline)      */

/* env-gated per-pixel write tracer (debug aid for the AA residual work,
 * GX_TRACES.md §5-6): GX_TRACE_PIX="x,y;x,y" logs every framebuffer
 * write to the listed pixels (tag: opq1/opq2/opq3/push/trans/aacopy/
 * aablend; L1 = top layer, L2 = AA second layer); GX_TRACE_LOG picks
 * the output file (default stderr). */
#define GX_TRACE_MAX_PIX 8
static struct { int x, y; } gx_tr_pix[GX_TRACE_MAX_PIX];
static int gx_tr_n = -1;
static FILE *gx_tr_out;

static void gx_tr_init(void)
{
    const char *spec, *log;
    gx_tr_n = 0;
    spec = getenv("GX_TRACE_PIX");
    if (!spec || !*spec)
        return;
    log = getenv("GX_TRACE_LOG");
    gx_tr_out = log ? fopen(log, "w") : stderr;
    if (!gx_tr_out)
        gx_tr_out = stderr;
    while (gx_tr_n < GX_TRACE_MAX_PIX && *spec) {
        int x, y;
        if (sscanf(spec, "%d,%d", &x, &y) != 2)
            break;
        gx_tr_pix[gx_tr_n].x = x;
        gx_tr_pix[gx_tr_n].y = y;
        gx_tr_n++;
        spec = strchr(spec, ';');
        if (!spec)
            break;
        spec++;
    }
}

static void gx_tr_log(const char *tag, int x, int y, int layer,
                      uint32_t color, uint32_t attr, int32_t z)
{
    int i;
    if (gx_tr_n < 0)
        gx_tr_init();
    if (gx_tr_n <= 0)
        return;
    for (i = 0; i < gx_tr_n; i++) {
        if (gx_tr_pix[i].x == x && gx_tr_pix[i].y == y) {
            fprintf(gx_tr_out, "%s L%d (%d,%d) color=%08x attr=%08x z=%d\n",
                    tag, layer, x, y, color, attr, z);
            break;
        }
    }
}

static void render_polygon_scanline(GxSurface *sf, GxRPoly *rp, int32_t y,
                                    const GxRasterEnv *env)
{
    const GxPolygon *polygon = rp->poly;
    uint32_t polyattr = (polygon->attr & 0x3F008000u);
    const GxRasVertex *vc[4];
    int polyalpha = (int)((polygon->attr >> 16) & 0x1F);
    int wireframe = (polyalpha == 0);
    int aa = env->aa_enable;
    int32_t xstart, xend, wl, wr, zl, zr;
    int32_t rl, gl, bl, sl, tl, rr, gr, br, sr, tr;
    int l_filledge, r_filledge, swapped = 0;
    int32_t l_edgelen = 0, r_edgelen = 0, l_edgecov = 0, r_edgecov = 0;
    int yedge, edge;
    int32_t x, xlimit, xcov = 0;
    GxInterp interp_x;

    if (!polygon->facing_view)
        polyattr |= (1u << 4);

    if (wireframe) polyalpha = 31;

    if (rp->ytop != rp->ybot) {
        if (y >= polygon->v[rp->next_vl].sy && rp->cur_vl != rp->vbot)
            setup_left_edge(rp, y);
        if (y >= polygon->v[rp->next_vr].sy && rp->cur_vr != rp->vbot)
            setup_right_edge(rp, y);
    }

    xstart = rp->XL;
    xend = rp->XR;

    wl = interp_value(&rp->slope_l.Interp, polygon->v[rp->cur_vl].wnorm,
                      polygon->v[rp->next_vl].wnorm);
    wr = interp_value(&rp->slope_r.Interp, polygon->v[rp->cur_vr].wnorm,
                      polygon->v[rp->next_vr].wnorm);
    zl = interp_z(&rp->slope_l.Interp, polygon->v[rp->cur_vl].depth,
                  polygon->v[rp->next_vl].depth);
    zr = interp_z(&rp->slope_r.Interp, polygon->v[rp->cur_vr].depth,
                  polygon->v[rp->next_vr].depth);

    /* right vertical edges are pushed 1px to the left as long as either:
     * the left edge slope is not 0, or the span is not 0 pixels wide, and
     * it is not at the leftmost pixel of the screen */
    if (rp->slope_r.Increment == 0 &&
        (rp->slope_l.Increment != 0 || xstart != xend) && (xend != 0))
        xend--;

    if (xstart > xend) {
        /* if the left and right edges are swapped, render backwards.
         * on hardware, swapped edges seem to break edge length calculation,
         * causing X-major edges to be rendered wrong when filled, and
         * resulting in buggy looking anti-aliasing on X-major edges */
        const GxRasVertex *vcL = &polygon->v[rp->cur_vr], *vnL = &polygon->v[rp->next_vr];
        const GxRasVertex *vcR = &polygon->v[rp->cur_vl], *vnR = &polygon->v[rp->next_vl];
        int32_t tmp;
        vc[0] = vcL; vc[1] = vnL; vc[2] = vcR; vc[3] = vnR;

        slope_edge_params(&rp->slope_r, 1, 1, &l_edgelen, &l_edgecov);
        slope_edge_params(&rp->slope_l, 0, 1, &r_edgelen, &r_edgecov);

        tmp = xstart; xstart = xend; xend = tmp;
        tmp = wl; wl = wr; wr = tmp;
        tmp = zl; zl = zr; zr = tmp;
        swapped = 1;

        /* edge fill rules for swapped opaque edges (see melonDS 1016-1035) */
        if (aa || env->edge_enable ||
            ((polyalpha < 31) && env->alpha_blend) || wireframe) {
            l_filledge = 1;
            r_filledge = 1;
        } else {
            l_filledge = (rp->slope_r.Negative || !rp->slope_r.XMajor)
                || ((y == rp->ybot - 1) && rp->slope_r.XMajor &&
                   (vnL->sx != vnR->sx));
            r_filledge = (!rp->slope_l.Negative && rp->slope_l.XMajor)
                || (!(rp->slope_l.Negative && rp->slope_l.XMajor) &&
                    rp->slope_r.Increment == 0)
                || ((y == rp->ybot - 1) && rp->slope_l.XMajor &&
                   (vnL->sx != vnR->sx));
        }
    } else {
        const GxRasVertex *vcL = &polygon->v[rp->cur_vl], *vnL = &polygon->v[rp->next_vl];
        const GxRasVertex *vcR = &polygon->v[rp->cur_vr], *vnR = &polygon->v[rp->next_vr];
        vc[0] = vcL; vc[1] = vnL; vc[2] = vcR; vc[3] = vnR;

        slope_edge_params(&rp->slope_l, 0, 0, &l_edgelen, &l_edgecov);
        slope_edge_params(&rp->slope_r, 1, 0, &r_edgelen, &r_edgecov);

        /* edge fill rules for unswapped opaque edges (see melonDS 1050-1070) */
        if (aa || env->edge_enable ||
            ((polyalpha < 31) && env->alpha_blend) || wireframe) {
            l_filledge = 1;
            r_filledge = 1;
        } else {
            l_filledge = ((rp->slope_l.Negative || !rp->slope_l.XMajor)
                || ((y == rp->ybot - 1) && rp->slope_l.XMajor &&
                   (vnL->sx != vnR->sx)))
                || ((rp->slope_l.Increment == rp->slope_r.Increment) &&
                   (xstart + l_edgelen == xend + 1));
            r_filledge = (!rp->slope_r.Negative && rp->slope_r.XMajor) ||
                (rp->slope_r.Increment == 0)
                || ((y == rp->ybot - 1) && rp->slope_r.XMajor &&
                   (vnL->sx != vnR->sx));
        }
    }

    /* interpolate attributes along Y */
    {
        const GxInterp *is = swapped ? &rp->slope_r.Interp : &rp->slope_l.Interp;
        const GxInterp *ie = swapped ? &rp->slope_l.Interp : &rp->slope_r.Interp;
        const GxRasVertex *v0 = vc[0], *v1 = vc[1], *v2 = vc[2], *v3 = vc[3];
        rl = interp_value(is, v0->cr, v1->cr);
        gl = interp_value(is, v0->cg, v1->cg);
        bl = interp_value(is, v0->cb, v1->cb);
        sl = interp_value(is, v0->s, v1->s);
        tl = interp_value(is, v0->t, v1->t);
        rr = interp_value(ie, v2->cr, v3->cr);
        gr = interp_value(ie, v2->cg, v3->cg);
        br = interp_value(ie, v2->cb, v3->cb);
        sr = interp_value(ie, v2->s, v3->s);
        tr = interp_value(ie, v2->t, v3->t);
    }

    yedge = 0;
    if (y == rp->ytop)        yedge = 0x4;
    else if (y == rp->ybot - 1) yedge = 0x8;

    x = xstart;
    interp_setup(&interp_x, xstart, xend + 1, wl, wr, polygon->wbuffer, 0);
    if (x < 0) x = 0;

    /* part 1: left edge */
    edge = yedge | 0x1;
    xlimit = xstart + l_edgelen;
    if (xlimit > xend + 1) xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    if (l_edgecov & (1 << 31)) {
        xcov = (l_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }
    if (!l_filledge) {
        x = xlimit;
    } else
    for (; x < xlimit; x++) {
        size_t pixeladdr = (size_t)y * sf->w + x;
        uint32_t dstattr = sf->attr ? sf->attr[pixeladdr] : 0;
        uint32_t *pcolor = sf->color, *pdepth = sf->depth, *pattr = sf->attr;
        int layer2 = 0;
        int32_t z;
        uint32_t vr, vg, vb, color;
        int16_t s, t;
        uint8_t alpha;

        interp_setx(&interp_x, x);
        z = interp_z(&interp_x, zl, zr);

        /* if depth test against the topmost pixel fails, test against the
         * pixel underneath (AA second layer) */
        if (!depth_pass(polygon, dstattr, (int32_t)sf->depth[pixeladdr], z)) {
            if (!sf->attr || !(dstattr & 0xF) || !sf->color2 ||
                !sf->attr2 || !sf->depth2)
                continue;
            /* melonDS falls through to the lower pixel as
             * pixeladdr += BufferSize in unified 2*w*h buffers
             * (GPU3D_Soft.cpp:1142-1147); GxSurface keeps the second
             * layer in separate w*h arrays (color2/depth2/attr2) at the
             * same base addr */
            layer2 = 1;
            pcolor = sf->color2; pdepth = sf->depth2; pattr = sf->attr2;
            dstattr = pattr[pixeladdr];
            if (!depth_pass(polygon, dstattr, (int32_t)pdepth[pixeladdr], z))
                continue;
        }

        vr = (uint32_t)interp_value(&interp_x, rl, rr);
        vg = (uint32_t)interp_value(&interp_x, gl, gr);
        vb = (uint32_t)interp_value(&interp_x, bl, br);
        s = (int16_t)interp_value(&interp_x, sl, sr);
        t = (int16_t)interp_value(&interp_x, tl, tr);

        color = gx_shade_pixel(polygon, (uint8_t)(vr >> 3), (uint8_t)(vg >> 3),
                               (uint8_t)(vb >> 3), s, t, env);
        alpha = (uint8_t)(color >> 24);

        if ((int)alpha <= env->alpha_ref)
            continue;

        if (alpha == 31) {
            uint32_t attr = polyattr | (uint32_t)edge;
            if (aa) {
                /* anti-aliasing: all edges are rendered; calculate coverage */
                int32_t cov = l_edgecov;
                if (cov & (1 << 31)) {
                    cov = xcov >> 5;
                    if (cov > 31) cov = 31;
                    xcov += (l_edgecov & 0x3FF);
                }
                attr |= (uint32_t)(cov << 8);
                /* push old pixel down if needed (only when replacing the
                 * TOP layer: GPU3D_Soft.cpp:1181-1187, pixeladdr <
                 * BufferSize; a second-layer plot replaces in place) */
                if (!layer2 && sf->color2) {
                    gx_tr_log("push", x, y, 2, sf->color[pixeladdr],
                              sf->attr ? sf->attr[pixeladdr] : 0,
                              (int32_t)sf->depth[pixeladdr]);
                    sf->color2[pixeladdr] = sf->color[pixeladdr];
                    sf->depth2[pixeladdr] = sf->depth[pixeladdr];
                    if (sf->attr2)
                        sf->attr2[pixeladdr] = sf->attr[pixeladdr];
                }
            }
            gx_tr_log("opq1", x, y, layer2 + 1, color, attr, z);
            pdepth[pixeladdr] = (uint32_t)z;
            pcolor[pixeladdr] = color;
            gx_ras_pixels_total++;
            if (pattr)
                pattr[pixeladdr] = attr;
        } else {
            uint32_t dstattr2 = dstattr;
            if (!(polygon->attr & (1u << 11))) z = -1;
            gx_tr_log("trans", x, y, layer2 + 1, color, polyattr, z);
            plot_translucent(pcolor, pdepth, pattr, pixeladdr, color, z,
                             polyattr, env);
            /* blend with bottom pixel too, if needed
             * (GPU3D_Soft.cpp:1199-1201: dstattr edge flags + top layer) */
            if (!layer2 && sf->attr && (dstattr2 & 0xF) && sf->color2 &&
                sf->attr2 && sf->depth2) {
                gx_tr_log("trans", x, y, 2, color, polyattr, z);
                plot_translucent(sf->color2, sf->depth2, sf->attr2, pixeladdr,
                                 color, z, polyattr, env);
            }
        }
    }

    /* part 2: polygon inside */
    edge = yedge;
    xlimit = xend - r_edgelen + 1;
    if (xlimit > xend + 1) xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    if (wireframe && !edge) {
        if (x < xlimit) x = xlimit;
    } else
    for (; x < xlimit; x++) {
        size_t pixeladdr = (size_t)y * sf->w + x;
        uint32_t dstattr = sf->attr ? sf->attr[pixeladdr] : 0;
        uint32_t *pcolor = sf->color, *pdepth = sf->depth, *pattr = sf->attr;
        int layer2 = 0;
        int32_t z;
        uint32_t vr, vg, vb, color;
        int16_t s, t;
        uint8_t alpha;

        interp_setx(&interp_x, x);
        z = interp_z(&interp_x, zl, zr);

        if (!depth_pass(polygon, dstattr, (int32_t)sf->depth[pixeladdr], z)) {
            if (!sf->attr || !(dstattr & 0xF) || !sf->color2 ||
                !sf->attr2 || !sf->depth2)
                continue;
            /* second layer (GPU3D_Soft.cpp:1236-1244) */
            layer2 = 1;
            pcolor = sf->color2; pdepth = sf->depth2; pattr = sf->attr2;
            dstattr = pattr[pixeladdr];
            if (!depth_pass(polygon, dstattr, (int32_t)pdepth[pixeladdr], z))
                continue;
        }

        vr = (uint32_t)interp_value(&interp_x, rl, rr);
        vg = (uint32_t)interp_value(&interp_x, gl, gr);
        vb = (uint32_t)interp_value(&interp_x, bl, br);
        s = (int16_t)interp_value(&interp_x, sl, sr);
        t = (int16_t)interp_value(&interp_x, tl, tr);

        color = gx_shade_pixel(polygon, (uint8_t)(vr >> 3), (uint8_t)(vg >> 3),
                               (uint8_t)(vb >> 3), s, t, env);
        alpha = (uint8_t)(color >> 24);

        if ((int)alpha <= env->alpha_ref)
            continue;

        if (alpha == 31) {
            uint32_t attr = polyattr | (uint32_t)edge;
            if (aa && (attr & 0xF)) {
                /* anti-aliasing: set coverage to avoid black lines */
                attr |= (0x1Fu << 8);
                /* push old pixel down (top layer only:
                 * GPU3D_Soft.cpp:1270-1276) */
                if (!layer2 && sf->color2) {
                    gx_tr_log("push", x, y, 2, sf->color[pixeladdr],
                              sf->attr ? sf->attr[pixeladdr] : 0,
                              (int32_t)sf->depth[pixeladdr]);
                    sf->color2[pixeladdr] = sf->color[pixeladdr];
                    sf->depth2[pixeladdr] = sf->depth[pixeladdr];
                    if (sf->attr2)
                        sf->attr2[pixeladdr] = sf->attr[pixeladdr];
                }
            }
            gx_tr_log("opq2", x, y, layer2 + 1, color, attr, z);
            pdepth[pixeladdr] = (uint32_t)z;
            pcolor[pixeladdr] = color;
            gx_ras_pixels_total++;
            if (pattr)
                pattr[pixeladdr] = attr;
        } else {
            uint32_t dstattr2 = dstattr;
            if (!(polygon->attr & (1u << 11))) z = -1;
            gx_tr_log("trans", x, y, layer2 + 1, color, polyattr, z);
            plot_translucent(pcolor, pdepth, pattr, pixeladdr, color, z,
                             polyattr, env);
            if (!layer2 && sf->attr && (dstattr2 & 0xF) && sf->color2 &&
                sf->attr2 && sf->depth2) {
                gx_tr_log("trans", x, y, 2, color, polyattr, z);
                plot_translucent(sf->color2, sf->depth2, sf->attr2, pixeladdr,
                                 color, z, polyattr, env);
            }
        }
    }

    /* part 3: right edge */
    edge = yedge | 0x2;
    xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    if (r_edgecov & (1 << 31)) {
        xcov = (r_edgecov >> 12) & 0x3FF;
        if (xcov == 0x3FF) xcov = 0;
    }
    if (r_filledge)
    for (; x < xlimit; x++) {
        size_t pixeladdr = (size_t)y * sf->w + x;
        uint32_t dstattr = sf->attr ? sf->attr[pixeladdr] : 0;
        uint32_t *pcolor = sf->color, *pdepth = sf->depth, *pattr = sf->attr;
        int layer2 = 0;
        int32_t z;
        uint32_t vr, vg, vb, color;
        int16_t s, t;
        uint8_t alpha;

        interp_setx(&interp_x, x);
        z = interp_z(&interp_x, zl, zr);

        if (!depth_pass(polygon, dstattr, (int32_t)sf->depth[pixeladdr], z)) {
            if (!sf->attr || !(dstattr & 0xF) || !sf->color2 ||
                !sf->attr2 || !sf->depth2)
                continue;
            /* second layer (GPU3D_Soft.cpp:1328-1336) */
            layer2 = 1;
            pcolor = sf->color2; pdepth = sf->depth2; pattr = sf->attr2;
            dstattr = pattr[pixeladdr];
            if (!depth_pass(polygon, dstattr, (int32_t)pdepth[pixeladdr], z))
                continue;
        }

        vr = (uint32_t)interp_value(&interp_x, rl, rr);
        vg = (uint32_t)interp_value(&interp_x, gl, gr);
        vb = (uint32_t)interp_value(&interp_x, bl, br);
        s = (int16_t)interp_value(&interp_x, sl, sr);
        t = (int16_t)interp_value(&interp_x, tl, tr);

        color = gx_shade_pixel(polygon, (uint8_t)(vr >> 3), (uint8_t)(vg >> 3),
                               (uint8_t)(vb >> 3), s, t, env);
        alpha = (uint8_t)(color >> 24);

        if ((int)alpha <= env->alpha_ref)
            continue;

        if (alpha == 31) {
            uint32_t attr = polyattr | (uint32_t)edge;
            if (aa) {
                /* anti-aliasing: calculate coverage */
                int32_t cov = r_edgecov;
                if (cov & (1 << 31)) {
                    cov = 0x1F - (xcov >> 5);
                    if (cov < 0) cov = 0;
                    xcov += (r_edgecov & 0x3FF);
                }
                attr |= (uint32_t)(cov << 8);
                /* push old pixel down (top layer only:
                 * GPU3D_Soft.cpp:1369-1375) */
                if (!layer2 && sf->color2) {
                    gx_tr_log("push", x, y, 2, sf->color[pixeladdr],
                              sf->attr ? sf->attr[pixeladdr] : 0,
                              (int32_t)sf->depth[pixeladdr]);
                    sf->color2[pixeladdr] = sf->color[pixeladdr];
                    sf->depth2[pixeladdr] = sf->depth[pixeladdr];
                    if (sf->attr2)
                        sf->attr2[pixeladdr] = sf->attr[pixeladdr];
                }
            }
            gx_tr_log("opq3", x, y, layer2 + 1, color, attr, z);
            pdepth[pixeladdr] = (uint32_t)z;
            pcolor[pixeladdr] = color;
            gx_ras_pixels_total++;
            if (pattr)
                pattr[pixeladdr] = attr;
        } else {
            uint32_t dstattr2 = dstattr;
            if (!(polygon->attr & (1u << 11))) z = -1;
            gx_tr_log("trans", x, y, layer2 + 1, color, polyattr, z);
            plot_translucent(pcolor, pdepth, pattr, pixeladdr, color, z,
                             polyattr, env);
            if (!layer2 && sf->attr && (dstattr2 & 0xF) && sf->color2 &&
                sf->attr2 && sf->depth2) {
                gx_tr_log("trans", x, y, 2, color, polyattr, z);
                plot_translucent(sf->color2, sf->depth2, sf->attr2, pixeladdr,
                                 color, z, polyattr, env);
            }
        }
    }
}

void gx_raster_polygon(GxSurface *sf, const GxPolygon *poly,
                       const GxRasterEnv *env)
{
    GxRPoly rp;
    int32_t y;
    unsigned long long px0 = gx_ras_pixels_total;

    setup_polygon(&rp, poly);
    if (rp.ytop == rp.ybot) {
        render_polygon_scanline(sf, &rp, rp.ytop, env);
        gx_ras_last_pixels = gx_ras_pixels_total - px0;
        return;
    }
    for (y = rp.ytop; y < rp.ybot; y++) {
        render_polygon_scanline(sf, &rp, y, env);
        rp.XL = slope_step(&rp.slope_l);
        rp.XR = slope_step(&rp.slope_r);
    }
    gx_ras_last_pixels = gx_ras_pixels_total - px0;
}

/* ---- span parameter recorder for GPU backends (see gx_raster.h) ----
 * Mirrors the prologue + x-flow of render_polygon_scanline() exactly
 * (same static slope state machine), recording what the fill loops would
 * do without touching any buffer. */

static void record_span_row(GxSpanRow *out, GxRPoly *rp, int32_t y,
                            const GxPolygon *polygon, const GxRasterEnv *env)
{
    int polyalpha = (int)((polygon->attr >> 16) & 0x1F);
    int wireframe = (polyalpha == 0);
    int aa = env->aa_enable;
    int32_t xstart, xend;
    int l_filledge, r_filledge, swapped = 0;
    int32_t l_edgelen = 0, r_edgelen = 0, l_edgecov = 0, r_edgecov = 0;
    int yedge;
    int32_t x, xlimit;

    if (wireframe) polyalpha = 31;

    if (rp->ytop != rp->ybot) {
        if (y >= polygon->v[rp->next_vl].sy && rp->cur_vl != rp->vbot)
            setup_left_edge(rp, y);
        if (y >= polygon->v[rp->next_vr].sy && rp->cur_vr != rp->vbot)
            setup_right_edge(rp, y);
    }

    xstart = rp->XL;
    xend = rp->XR;

    /* right vertical edges are pushed 1px to the left as long as either:
     * the left edge slope is not 0, or the span is not 0 pixels wide, and
     * it is not at the leftmost pixel of the screen */
    if (rp->slope_r.Increment == 0 &&
        (rp->slope_l.Increment != 0 || xstart != xend) && (xend != 0))
        xend--;

    if (xstart > xend) {
        const GxRasVertex *vnL = &polygon->v[rp->next_vr];
        const GxRasVertex *vnR = &polygon->v[rp->next_vl];
        int32_t tmp;

        slope_edge_params(&rp->slope_r, 1, 1, &l_edgelen, &l_edgecov);
        slope_edge_params(&rp->slope_l, 0, 1, &r_edgelen, &r_edgecov);

        tmp = xstart; xstart = xend; xend = tmp;
        swapped = 1;

        if (aa || env->edge_enable ||
            ((polyalpha < 31) && env->alpha_blend) || wireframe) {
            l_filledge = 1;
            r_filledge = 1;
        } else {
            l_filledge = (rp->slope_r.Negative || !rp->slope_r.XMajor)
                || ((y == rp->ybot - 1) && rp->slope_r.XMajor &&
                   (vnL->sx != vnR->sx));
            r_filledge = (!rp->slope_l.Negative && rp->slope_l.XMajor)
                || (!(rp->slope_l.Negative && rp->slope_l.XMajor) &&
                    rp->slope_r.Increment == 0)
                || ((y == rp->ybot - 1) && rp->slope_l.XMajor &&
                   (vnL->sx != vnR->sx));
        }
    } else {
        const GxRasVertex *vnL = &polygon->v[rp->next_vl];
        const GxRasVertex *vnR = &polygon->v[rp->next_vr];

        slope_edge_params(&rp->slope_l, 0, 0, &l_edgelen, &l_edgecov);
        slope_edge_params(&rp->slope_r, 1, 0, &r_edgelen, &r_edgecov);

        if (aa || env->edge_enable ||
            ((polyalpha < 31) && env->alpha_blend) || wireframe) {
            l_filledge = 1;
            r_filledge = 1;
        } else {
            l_filledge = ((rp->slope_l.Negative || !rp->slope_l.XMajor)
                || ((y == rp->ybot - 1) && rp->slope_l.XMajor &&
                   (vnL->sx != vnR->sx)))
                || ((rp->slope_l.Increment == rp->slope_r.Increment) &&
                    (xstart + l_edgelen == xend + 1));
            r_filledge = (!rp->slope_r.Negative && rp->slope_r.XMajor) ||
                (rp->slope_r.Increment == 0)
                || ((y == rp->ybot - 1) && rp->slope_r.XMajor &&
                   (vnL->sx != vnR->sx));
        }
    }
    (void)swapped;

    yedge = 0;
    if (y == rp->ytop)        yedge = 0x4;
    else if (y == rp->ybot - 1) yedge = 0x8;

    x = xstart;
    if (x < 0) x = 0;

    /* part 1: left edge */
    xlimit = xstart + l_edgelen;
    if (xlimit > xend + 1) xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    out->a_lo = x;
    out->a_hi = l_filledge ? xlimit : x;
    if (l_edgecov & (1 << 31)) {
        out->l_mode = 1;
        out->l_cov0 = (l_edgecov >> 12) & 0x3FF;
        if (out->l_cov0 == 0x3FF) out->l_cov0 = 0;
        out->l_incr = l_edgecov & 0x3FF;
    } else {
        out->l_mode = 0;
        out->l_cov0 = l_edgecov & 0x1F;
        out->l_incr = 0;
    }
    x = xlimit;

    /* part 2: polygon inside */
    xlimit = xend - r_edgelen + 1;
    if (xlimit > xend + 1) xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    out->b_lo = x;
    if (wireframe && !yedge) {
        out->b_hi = x;
        if (x < xlimit) x = xlimit;
    } else {
        out->b_hi = (x < xlimit) ? xlimit : x;
        if (x < xlimit) x = xlimit;
    }

    /* part 3: right edge */
    xlimit = xend + 1;
    if (xlimit > 256) xlimit = 256;
    out->c_lo = x;
    out->c_hi = (r_filledge && x < xlimit) ? xlimit : x;
    if (r_edgecov & (1 << 31)) {
        out->r_mode = 1;
        out->r_cov0 = (r_edgecov >> 12) & 0x3FF;
        if (out->r_cov0 == 0x3FF) out->r_cov0 = 0;
        out->r_incr = r_edgecov & 0x3FF;
    } else {
        out->r_mode = 0;
        out->r_cov0 = r_edgecov & 0x1F;
        out->r_incr = 0;
    }
}

int gx_raster_span_params(const GxPolygon *poly, const GxRasterEnv *env,
                          GxSpanRow *rows, int max_rows)
{
    GxRPoly rp;
    int32_t y;

    setup_polygon(&rp, poly);
    if (rp.ytop == rp.ybot) {
        if (rp.ytop >= 0 && rp.ytop < max_rows)
            record_span_row(&rows[rp.ytop], &rp, rp.ytop, poly, env);
        return rp.ytop;
    }
    for (y = rp.ytop; y < rp.ybot; y++) {
        if (y >= 0 && y < max_rows)
            record_span_row(&rows[y], &rp, y, poly, env);
        rp.XL = slope_step(&rp.slope_l);
        rp.XR = slope_step(&rp.slope_r);
    }
    return rp.ytop;
}

/* ------------------------------------------------------------------ */
/* frame post pass: edge marking + fog + antialiasing                  */

uint32_t gx_fog_density(const uint8_t density[34], uint32_t fog_offset,
                        int fog_shift, uint32_t z)
{
    uint32_t id, frac, d;

    if (z < fog_offset) {
        id = 0;
        frac = 0;
    } else {
        /* z difference is shifted right by two, then left by the fog shift;
         * bits 0-16 are the fractional part, bits 17-31 the table index
         * (melonDS CalculateFogDensity, GPU3D_Soft.cpp:1414-1451) */
        z -= fog_offset;
        z = (z >> 2) << fog_shift;
        id = z >> 17;
        if (id >= 32) {
            id = 32;
            frac = 0;
        } else
            frac = z & 0x1FFFF;
    }
    d = ((uint32_t)density[id] * (0x20000 - frac) +
         (uint32_t)density[id + 1] * frac) >> 17;
    if (d >= 127)
        d = 128;
    return d;
}

/* blend one 6-bit-channel pixel toward the fog color (GPU3D_Soft.cpp:1508-1525
 * top pixel / 1548-1568 lower pixel: RGB blended only when DISP3DCNT bit 6
 * is clear, alpha always) */
static uint32_t fog_blend_pixel(uint32_t col, uint32_t density,
                                uint32_t fogR, uint32_t fogG, uint32_t fogB,
                                uint32_t fogA, int fogcolor)
{
    uint32_t r = col & 0x3F;
    uint32_t g = (col >> 8) & 0x3F;
    uint32_t b = (col >> 16) & 0x3F;
    uint32_t a = (col >> 24) & 0x1F;

    if (fogcolor) {
        r = ((fogR * density) + (r * (128 - density))) >> 7;
        g = ((fogG * density) + (g * (128 - density))) >> 7;
        b = ((fogB * density) + (b * (128 - density))) >> 7;
    }
    a = ((fogA * density) + (a * (128 - density))) >> 7;
    return r | (g << 8) | (b << 16) | (a << 24);
}

void gx_post_pass(GxSurface *sf, const GxPostEnv *env)
{
    static const int dx[4] = { -1, 1, 0, 0 };
    static const int dy[4] = { 0, 0, -1, 1 };
    int x, y;

    if (!sf->attr)
        return;

    /* edge marking (melonDS GPU3D_Soft.cpp:1459-1490): pixels on a polygon
     * boundary (attr edge flags set) whose opaque polygon ID differs from a
     * 4-neighbor and that sit in front of that neighbor get the edge color
     * of their polygon ID's table entry (alpha untouched). */
    if (env->edge_enable && env->edge) {
        for (y = 0; y < sf->h; y++) {
            for (x = 0; x < sf->w; x++) {
                size_t addr = (size_t)y * sf->w + x;
                uint32_t attr = sf->attr[addr];
                uint32_t polyid, z, ec, er, eg, eb;
                int i, mark = 0;

                if (!(attr & 0xF))
                    continue;
                polyid = attr >> 24;
                z = sf->depth[addr];
                for (i = 0; i < 4 && !mark; i++) {
                    int nx = x + dx[i], ny = y + dy[i];
                    uint32_t npolyid, nz;
                    if (nx < 0 || nx >= sf->w || ny < 0 || ny >= sf->h) {
                        /* offscreen 1px border holds the clear attr/depth
                         * (ClearBuffers, GPU3D_Soft.cpp:1628-1652) */
                        npolyid = env->clear_attr >> 24;
                        nz = env->clear_depth;
                    } else {
                        size_t na = (size_t)ny * sf->w + nx;
                        npolyid = sf->attr[na] >> 24;
                        nz = sf->depth[na];
                    }
                    if (polyid != npolyid && z < nz)
                        mark = 1;
                }
                if (!mark)
                    continue;

                ec = env->edge[polyid >> 3];
                er = (ec << 1) & 0x3E; if (er) er++;
                eg = (ec >> 4) & 0x3E; if (eg) eg++;
                eb = (ec >> 9) & 0x3E; if (eb) eb++;
                sf->color[addr] = er | (eg << 8) | (eb << 16) |
                                  (sf->color[addr] & 0xFF000000);
                /* break antialiasing coverage: force the coverage field
                 * (attr bits 8-12) to 16 so the marked pixel blends 17/32
                 * against the AA second layer in the pass below, whatever
                 * coverage the rasterizer assigned
                 * (GPU3D_Soft.cpp:1486-1487) */
                sf->attr[addr] = (sf->attr[addr] & 0xFFFFE0FFu) | 0x00001000u;
            }
        }
    }

    /* fog (melonDS GPU3D_Soft.cpp:1492-1570): per-pixel density from the
     * depth buffer, blended into color (unless DISP3DCNT bit 6 = alpha-only)
     * and always into alpha.  Fog is applied to the topmost TWO pixels: the
     * AA second layer ("lower pixel") is fogged as well, gated on the
     * topmost pixel's edge flags and the lower pixel's own fog flag. */
    if (env->fog_enable) {
        int fogcolor = !env->fog_alpha_only;
        uint32_t fogR = (env->fog_color << 1) & 0x3E; if (fogR) fogR++;
        uint32_t fogG = (env->fog_color >> 4) & 0x3E; if (fogG) fogG++;
        uint32_t fogB = (env->fog_color >> 9) & 0x3E; if (fogB) fogB++;
        uint32_t fogA = (env->fog_color >> 16) & 0x1F;

        for (y = 0; y < sf->h; y++) {
            for (x = 0; x < sf->w; x++) {
                size_t addr = (size_t)y * sf->w + x;
                uint32_t attr = sf->attr[addr];

                if (attr & (1u << 15)) {
                    uint32_t density = gx_fog_density(env->fog_density,
                            env->fog_offset, env->fog_shift, sf->depth[addr]);
                    sf->color[addr] = fog_blend_pixel(sf->color[addr], density,
                            fogR, fogG, fogB, fogA, fogcolor);
                }

                /* fog for the AA second layer ("lower pixel"): only when the
                 * topmost pixel has edge flags and the lower pixel has its
                 * own fog flag; density comes from the lower pixel's depth
                 * (GPU3D_Soft.cpp:1527-1570) */
                if (!(attr & 0xF) || !sf->color2 || !sf->attr2 || !sf->depth2)
                    continue;
                if (!(sf->attr2[addr] & (1u << 15)))
                    continue;
                {
                    uint32_t density = gx_fog_density(env->fog_density,
                            env->fog_offset, env->fog_shift, sf->depth2[addr]);
                    sf->color2[addr] = fog_blend_pixel(sf->color2[addr],
                            density, fogR, fogG, fogB, fogA, fogcolor);
                }
            }
        }
    }

    /* antialiasing (melonDS GPU3D_Soft.cpp:1570-1620): edge pixels with
     * partial coverage are blended with the pixel underneath. */
    if (env->aa_enable && sf->color2) {
        for (y = 0; y < sf->h; y++) {
            for (x = 0; x < sf->w; x++) {
                size_t addr = (size_t)y * sf->w + x;
                uint32_t attr = sf->attr[addr];
                uint32_t coverage, top, bot;
                uint32_t topR, topG, topB, topA, botR, botG, botB, botA;

                if (!(attr & 0xF))
                    continue;
                coverage = (attr >> 8) & 0x1F;
                if (coverage == 0x1F)
                    continue;
                if (coverage == 0) {
                    gx_tr_log("aacopy", x, y, 1, sf->color2[addr], attr, 0);
                    sf->color[addr] = sf->color2[addr];
                    continue;
                }

                top = sf->color[addr];
                topR = top & 0x3F;
                topG = (top >> 8) & 0x3F;
                topB = (top >> 16) & 0x3F;
                topA = (top >> 24) & 0x1F;
                bot = sf->color2[addr];
                botR = bot & 0x3F;
                botG = (bot >> 8) & 0x3F;
                botB = (bot >> 16) & 0x3F;
                botA = (bot >> 24) & 0x1F;

                coverage++;
                /* only blend color if the bottom pixel isn't fully
                 * transparent */
                if (botA > 0) {
                    topR = ((topR * coverage) + (botR * (32 - coverage))) >> 5;
                    topG = ((topG * coverage) + (botG * (32 - coverage))) >> 5;
                    topB = ((topB * coverage) + (botB * (32 - coverage))) >> 5;
                }
                /* alpha is always blended */
                topA = ((topA * coverage) + (botA * (32 - coverage))) >> 5;

                gx_tr_log("aablend", x, y, 1,
                          (uint32_t)(topR | (topG << 8) | (topB << 16) |
                                     (topA << 24)),
                          attr, (int32_t)coverage);
                sf->color[addr] = topR | (topG << 8) | (topB << 16) |
                                  (topA << 24);
            }
        }
    }
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
