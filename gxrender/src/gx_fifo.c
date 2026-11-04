/* gx_fifo.c — DS GX command FIFO decoder + geometry vertex pipeline.
 * Command semantics follow melonDS src/GPU3D.cpp (see GX_COVERAGE.md for the
 * implemented/stubbed command table).  All arithmetic is integer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_fifo.h"

/* Parameter counts per GX command (melonDS GPU3D.cpp CmdNumParams). */
static const uint8_t gx_cmd_num_params[256] = {
    /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x10 */ 1,0,1,1,1,0,16,12,16,12,9,3,3,0,0,0,
    /* 0x20 */ 1,1,1,2,1,1,1,1,1,1,1,1,0,0,0,0,
    /* 0x30 */ 1,1,1,1,32,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x40 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x50 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x60 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x70 */ 3,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x80+ */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

void gx_init(Gx *gx, uint32_t *color, uint32_t *depth, int w, int h)
{
    memset(gx, 0, sizeof(*gx));

    gx_mat_identity(gx->proj);
    gx_mat_identity(gx->pos);
    gx_mat_identity(gx->vec);
    gx_mat_identity(gx->texm);
    gx_mat_identity(gx->clip_mat);

    gx->vp_x0 = 0;
    gx->vp_y0 = 0;
    gx->vp_w = GX_SCREEN_W;
    gx->vp_h = GX_SCREEN_H;

    gx->polygon_attr = (31u << 16) | (1u << 6) | (1u << 7); /* opaque, both sides */
    gx->cur_polygon_attr = gx->polygon_attr;
    gx->zero_dot_w = 0xFFFFFF; /* melonDS GPU3D.cpp:408 */

    gx->surface.color = color;
    gx->surface.depth = depth;
    gx->surface.w = w;
    gx->surface.h = h;

    gx->env.tex = &gx->texmem;
    gx->env.toon = gx->toon;
    gx->env.tex_enable = 1;
    gx->env.alpha_blend = 1;
    gx->env.alpha_ref = 0;

    gx->disp_cnt = 0x9; /* texture + alpha blend on */
}

void gx_set_texture_mem(Gx *gx, const uint8_t *vram, uint32_t vram_size,
                        const uint16_t *pal, uint32_t pal_count)
{
    gx->texmem.vram = vram;
    gx->texmem.vram_size = vram_size;
    gx->texmem.pal = pal;
    gx->texmem.pal_count = pal_count;
}

void gx_set_disp_cnt(Gx *gx, uint32_t disp_cnt)
{
    gx->disp_cnt = disp_cnt;
    gx->env.tex_enable = (disp_cnt >> 0) & 1;
    gx->env.highlight = (disp_cnt >> 1) & 1;
    gx->env.alpha_blend = (disp_cnt >> 3) & 1;
}

/* ------------------------------------------------------------------ */
/* matrix engine                                                      */

static int32_t *matrix_target(Gx *gx)
{
    switch (gx->matrix_mode) {
    case 0: return gx->proj;
    case 3: return gx->texm;
    default: return gx->pos;
    }
}

static void matrix_dirty(Gx *gx)
{
    if (gx->matrix_mode != 3)
        gx->clip_dirty = 1;
}

static void update_clip_matrix(Gx *gx)
{
    if (!gx->clip_dirty)
        return;
    gx->clip_dirty = 0;
    memcpy(gx->clip_mat, gx->proj, sizeof(gx->proj));
    gx_mat_mult4x4(gx->clip_mat, gx->pos);
}

/* ------------------------------------------------------------------ */
/* lighting (melonDS GPU3D::CalculateLighting arithmetic)              */

static void calculate_lighting(Gx *gx)
{
    /* texture modes 2/3 derive texcoords from the normal */
    if ((gx->tex_param >> 30) == 2) {
        gx->cur_s = gx->raw_s + (int32_t)(((int64_t)gx->normal[0] * gx->texm[0] +
                    (int64_t)gx->normal[1] * gx->texm[4] +
                    (int64_t)gx->normal[2] * gx->texm[8]) >> 21);
        gx->cur_t = gx->raw_t + (int32_t)(((int64_t)gx->normal[0] * gx->texm[1] +
                    (int64_t)gx->normal[1] * gx->texm[5] +
                    (int64_t)gx->normal[2] * gx->texm[9]) >> 21);
    }

    int32_t nt[3];
    nt[0] = (int32_t)(((int64_t)gx->normal[0] * gx->vec[0] +
                       (int64_t)gx->normal[1] * gx->vec[4] +
                       (int64_t)gx->normal[2] * gx->vec[8]) << 9 >> 21);
    nt[1] = (int32_t)(((int64_t)gx->normal[0] * gx->vec[1] +
                       (int64_t)gx->normal[1] * gx->vec[5] +
                       (int64_t)gx->normal[2] * gx->vec[9]) << 9 >> 21);
    nt[2] = (int32_t)(((int64_t)gx->normal[0] * gx->vec[2] +
                       (int64_t)gx->normal[1] * gx->vec[6] +
                       (int64_t)gx->normal[2] * gx->vec[10]) << 9 >> 21);

    uint32_t vtxbuff[3] = {
        (uint32_t)gx->mat_emi[0] << 14,
        (uint32_t)gx->mat_emi[1] << 14,
        (uint32_t)gx->mat_emi[2] << 14
    };
    for (int i = 0; i < 4; i++) {
        int32_t dot, diffdot, shinelevel;
        if (!(gx->cur_polygon_attr & (1u << i)))
            continue;

        dot = (int32_t)(((int64_t)gx->light_dir[i][0] * nt[0]) >> 9) +
              (int32_t)(((int64_t)gx->light_dir[i][1] * nt[1]) >> 9) +
              (int32_t)(((int64_t)gx->light_dir[i][2] * nt[2]) >> 9);

        if (dot > 0) {
            diffdot = (dot << 21) >> 21;
            vtxbuff[0] += (uint32_t)(gx->mat_diff[0] * gx->light_color[i][0] * diffdot) & 0xFFFFF;
            vtxbuff[1] += (uint32_t)(gx->mat_diff[1] * gx->light_color[i][1] * diffdot) & 0xFFFFF;
            vtxbuff[2] += (uint32_t)(gx->mat_diff[2] * gx->light_color[i][2] * diffdot) & 0xFFFFF;

            dot += nt[2];
            dot = (dot << 21) >> 21;
            dot = ((dot * dot) >> 10) & 0x3FF;
            shinelevel = (int32_t)(((int64_t)dot * gx->spec_recip[i]) >> 8) - (1 << 9);
            if (shinelevel < 0) {
                shinelevel = 0;
            } else {
                shinelevel = (shinelevel << 18) >> 18;
                if (shinelevel < 0) shinelevel = 0;
                else if (shinelevel > 0x1FF) shinelevel = 0x1FF;
            }
        } else {
            shinelevel = 0;
        }

        if (gx->use_shininess) {
            shinelevel >>= 2;
            shinelevel = gx->shininess[shinelevel & 0x7F];
            shinelevel <<= 1;
        }

        vtxbuff[0] += ((uint32_t)(gx->mat_spec[0] * shinelevel) + ((uint32_t)gx->mat_amb[0] << 9)) * gx->light_color[i][0];
        vtxbuff[1] += ((uint32_t)(gx->mat_spec[1] * shinelevel) + ((uint32_t)gx->mat_amb[1] << 9)) * gx->light_color[i][1];
        vtxbuff[2] += ((uint32_t)(gx->mat_spec[2] * shinelevel) + ((uint32_t)gx->mat_amb[2] << 9)) * gx->light_color[i][2];
    }

    gx->vtx_color[0] = (uint8_t)((vtxbuff[0] >> 14 > 31) ? 31 : (vtxbuff[0] >> 14));
    gx->vtx_color[1] = (uint8_t)((vtxbuff[1] >> 14 > 31) ? 31 : (vtxbuff[1] >> 14));
    gx->vtx_color[2] = (uint8_t)((vtxbuff[2] >> 14 > 31) ? 31 : (vtxbuff[2] >> 14));
}

/* ------------------------------------------------------------------ */
/* vertex / polygon submission                                        */

static void submit_polygon(Gx *gx);
static void dbg_cmdlog(uint8_t cmd, const int32_t *p, int n);

/* Phase-5BO diagnostic: count of polygons printed by NNK_GX_CLIPDBG (static:
 * it must not enter struct Gx, whose size is hashed into the savestate
 * layout fingerprint). */
static unsigned dbg_clip_n;

/* Phase-5BP diagnostic: ring of the last 32 raw FIFO words, so the first
 * submitted polygon can show the exact command stream that produced it. */
static uint32_t dbg_ring[32];
static int dbg_ring_i;

/* Phase-5BP diagnostic: matrix/geometry command log + unknown-command
 * histogram (both behind NNK_GX_CMDBG / always-histogram-in-CLIPDBG). */
static unsigned dbg_cmd_n;
static unsigned dbg_unknown_hist[256];
static unsigned dbg_zero_mult;
int gx_dbg_unknown_hist(unsigned cmd) { return (int)dbg_unknown_hist[cmd & 0xFF]; }
unsigned gx_dbg_zero_mult(void) { return dbg_zero_mult; }



/* Phase-5BO diagnostic: count of polygons printed by NNK_GX_CLIPDBG (static:
 * it must not enter struct Gx, whose size is hashed into the savestate
 * layout fingerprint). */
static unsigned dbg_clip_n;

static void submit_vertex(Gx *gx)
{
    GxClipVertex *v;
    int64_t p[3] = { gx->cur_vtx[0], gx->cur_vtx[1], gx->cur_vtx[2] };
    const int32_t *m;

    update_clip_matrix(gx);
    m = gx->clip_mat;
    v = &gx->temp_vtx[gx->vtx_in_poly];

    v->clip[0] = (int32_t)((p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + (int64_t)GX_FX_ONE * m[12]) >> 12);
    v->clip[1] = (int32_t)((p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + (int64_t)GX_FX_ONE * m[13]) >> 12);
    v->clip[2] = (int32_t)((p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + (int64_t)GX_FX_ONE * m[14]) >> 12);
    v->clip[3] = (int32_t)((p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + (int64_t)GX_FX_ONE * m[15]) >> 12);

    v->col20[0] = (uint32_t)gx->vtx_color[0] << 12 | 0xFFF;
    v->col20[1] = (uint32_t)gx->vtx_color[1] << 12 | 0xFFF;
    v->col20[2] = (uint32_t)gx->vtx_color[2] << 12 | 0xFFF;

    if ((gx->tex_param >> 30) == 3) {
        /* texcoords from vertex position (result truncated to s16,
         * melonDS GPU3D.cpp:1576-1585) */
        v->s = (int16_t)(int32_t)(((p[0] * gx->texm[0] + p[1] * gx->texm[4] + p[2] * gx->texm[8]) >> 24) + gx->raw_s);
        v->t = (int16_t)(int32_t)(((p[0] * gx->texm[1] + p[1] * gx->texm[5] + p[2] * gx->texm[9]) >> 24) + gx->raw_t);
    } else {
        v->s = gx->cur_s;
        v->t = gx->cur_t;
    }

    gx->n_verts++;
    gx->vtx_in_poly++;
    gx->vtx_count++;

    switch (gx->polygon_mode) {
    case 0: /* triangle */
        if (gx->vtx_in_poly == 3) {
            gx->vtx_in_poly = 0;
            submit_polygon(gx);
            gx->num_consecutive++;
        }
        break;
    case 1: /* quad */
        if (gx->vtx_in_poly == 4) {
            gx->vtx_in_poly = 0;
            submit_polygon(gx);
            gx->num_consecutive++;
        }
        break;
    case 2: /* triangle strip */
        if (gx->num_consecutive & 1) {
            GxClipVertex tmp = gx->temp_vtx[1];
            gx->temp_vtx[1] = gx->temp_vtx[0];
            gx->temp_vtx[0] = tmp;

            gx->vtx_in_poly = 2;
            submit_polygon(gx);
            gx->num_consecutive++;

            gx->temp_vtx[1] = gx->temp_vtx[2];
        } else if (gx->vtx_in_poly == 3) {
            gx->vtx_in_poly = 2;
            submit_polygon(gx);
            gx->num_consecutive++;

            gx->temp_vtx[0] = gx->temp_vtx[1];
            gx->temp_vtx[1] = gx->temp_vtx[2];
        }
        break;
    case 3: /* quad strip */
        if (gx->vtx_in_poly == 4) {
            GxClipVertex tmp = gx->temp_vtx[3];
            gx->temp_vtx[3] = gx->temp_vtx[2];
            gx->temp_vtx[2] = tmp;

            gx->vtx_in_poly = 2;
            submit_polygon(gx);
            gx->num_consecutive++;

            gx->temp_vtx[0] = gx->temp_vtx[3];
            gx->temp_vtx[1] = gx->temp_vtx[2];
        }
        break;
    }
}

/* Polygon culling and clipping — mirrors melonDS GPU3D.cpp SubmitPolygon /
 * ClipSegment / ClipAgainstPlane / ClipPolygon:
 *  - culling runs in CLIP space on the raw polygon, using the normal over
 *    (x, y, w): dot < 0 = back face (POLYGON_ATTR bit 7), dot > 0 = front
 *    face (bit 6), dot == 0 draws regardless of the attribute bits;
 *  - clipping is the hardware's vertex-walk against z=+w/-w, then y, then x;
 *    a vertex past the far plane (z > w) rejects the whole polygon unless
 *    POLYGON_ATTR bit 12 (far-plane intersect) is set;
 *  - attributes interpolate with the hardware's trunc-toward-zero arithmetic;
 *  - W is truncated to 24 bits and any vertex ending at w == 0 makes the
 *    polygon degenerate (melonDS Degenerate flag: not rendered).
 * After full +/-w clipping every vertex maps inside the viewport, so screen
 * coordinates need no clamping for ordinary geometry (the wide clamp in
 * fill_ras_vertex is only an overflow guard). */

static void clip_segment(GxClipVertex *out, const GxClipVertex *vin,
                         const GxClipVertex *voutv, int comp, int plane)
{
    int64_t factor_num = vin->clip[3] - (int64_t)plane * vin->clip[comp];
    int32_t factor_den = (int32_t)(factor_num -
        ((int64_t)voutv->clip[3] - (int64_t)plane * voutv->clip[comp]));
    int i;
    if (factor_den == 0) {
        *out = *vin;
        out->clip[comp] = plane * out->clip[3];
        return;
    }
    for (i = 0; i < 4; i++)
        if (i != comp)
            out->clip[i] = vin->clip[i] + (int32_t)(((int64_t)(voutv->clip[i] -
                vin->clip[i]) * factor_num) / factor_den);
    out->clip[comp] = plane * out->clip[3];
    out->s = (int16_t)(vin->s + (int32_t)(((int64_t)(voutv->s - vin->s) *
                factor_num) / factor_den));
    out->t = (int16_t)(vin->t + (int32_t)(((int64_t)(voutv->t - vin->t) *
                factor_num) / factor_den));
    for (i = 0; i < 3; i++)
        out->col20[i] = (uint32_t)((int64_t)vin->col20[i] +
            (((int64_t)voutv->col20[i] - (int64_t)vin->col20[i]) *
            factor_num) / factor_den);
}

/* Clip one component against both planes comp = +w / -w (melonDS
 * ClipAgainstPlane: the + side runs first into temp[], the - side back into
 * verts[]).  Returns the new vertex count, 0 if the polygon is rejected. */
static int clip_comp(Gx *gx, int comp, GxClipVertex *verts, int nverts,
                     GxClipVertex *temp)
{
    int i, prev, next, c = 0;

    for (i = 0; i < nverts; i++) {
        GxClipVertex vtx = verts[i];
        prev = i ? i - 1 : nverts - 1;
        next = i + 1 < nverts ? i + 1 : 0;
        if (vtx.clip[comp] > vtx.clip[3]) {
            if (comp == 2 && !(gx->cur_polygon_attr & (1u << 12)))
                return 0; /* far-plane reject */
            if (verts[prev].clip[comp] <= verts[prev].clip[3]) {
                clip_segment(&temp[c], &vtx, &verts[prev], comp, 1);
                c++;
            }
            if (verts[next].clip[comp] <= verts[next].clip[3]) {
                clip_segment(&temp[c], &vtx, &verts[next], comp, 1);
                c++;
            }
        } else {
            temp[c++] = vtx;
        }
        if (c >= 9)
            break;
    }
    nverts = c;
    c = 0;
    for (i = 0; i < nverts; i++) {
        GxClipVertex vtx = temp[i];
        prev = i ? i - 1 : nverts - 1;
        next = i + 1 < nverts ? i + 1 : 0;
        if (vtx.clip[comp] < -vtx.clip[3]) {
            if (temp[prev].clip[comp] >= -temp[prev].clip[3]) {
                clip_segment(&verts[c], &vtx, &temp[prev], comp, -1);
                c++;
            }
            if (temp[next].clip[comp] >= -temp[next].clip[3]) {
                clip_segment(&verts[c], &vtx, &temp[next], comp, -1);
                c++;
            }
        } else {
            verts[c++] = vtx;
        }
        if (c >= 9)
            break;
    }

    /* clipped vertex colors get re-rounded to the 12.20 format
     * (melonDS ClipAgainstPlane, GPU3D.cpp:1108-1116) */
    for (i = 0; i < c; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            verts[i].col20[k] &= ~0xFFFu;
            verts[i].col20[k] += 0xFFFu;
        }
    }
    return c;
}

static int clip_polygon(Gx *gx, GxClipVertex *verts, int nverts,
                        GxClipVertex *temp)
{
    nverts = clip_comp(gx, 2, verts, nverts, temp);
    if (nverts == 0) return 0;
    nverts = clip_comp(gx, 1, verts, nverts, temp);
    if (nverts == 0) return 0;
    return clip_comp(gx, 0, verts, nverts, temp);
}

/* Per-vertex output (melonDS GPU3D.cpp:1303-1551):
 * - integer FinalPosition via the 32-bit divider, with the W>0xFFFF
 *   half-shift quirk and the 9/8-bit result masks;
 * - FinalW: W normalized so the polygon's W values fit in 16 bits
 *   (per-polygon, 4-bit steps);
 * - depth: Z mode clamp(((z*0x4000)/w + 0x3FFF)*0x200, 0, 0xFFFFFF),
 *   W mode = W truncated to its top 16 bits;
 * - FinalColor: 5-bit color expanded to 9 bits ((c<<4)+0xF). */
static void fill_ras_vertex(Gx *gx, const GxClipVertex *cv, GxRasVertex *rv,
                            int wsize)
{
    uint32_t w = (uint32_t)cv->clip[3];
    uint32_t posX, posY, den;
    int32_t wshifted;
    int s;

    if (w == 0) {
        posX = 0;
        posY = 0;
    } else {
        posX = (uint32_t)cv->clip[0] + w;
        posY = (uint32_t)(-cv->clip[1]) + w;
        den = w;
        if (w > 0xFFFF) {
            posX >>= 1;
            posY >>= 1;
            den >>= 1;
        }
        den <<= 1;
        posX = (posX * (uint32_t)gx->vp_w) / den + (uint32_t)gx->vp_x0;
        posY = (posY * (uint32_t)gx->vp_h) / den + (uint32_t)gx->vp_y0;
    }
    rv->sx = (int32_t)(posX & 0x1FF);
    rv->sy = (int32_t)(posY & 0xFF);

    s = (wsize < 16) ? (16 - wsize) : (wsize - 16);
    if (wsize < 16) {
        rv->wnorm = (int32_t)(w << s);
        wshifted = rv->wnorm >> s;
    } else {
        rv->wnorm = (int32_t)(w >> s);
        wshifted = rv->wnorm << s;
    }

    if (gx->wbuffer) {
        rv->depth = wshifted;
    } else {
        int64_t z = (((int64_t)cv->clip[2] * 0x4000) / cv->clip[3] + 0x3FFF) * 0x200;
        rv->depth = (int32_t)gx_clamp32(z, 0, 0xFFFFFF);
    }

    rv->cr = (int32_t)(cv->col20[0] >> 12);
    if (rv->cr) rv->cr = (rv->cr << 4) + 0xF;
    rv->cg = (int32_t)(cv->col20[1] >> 12);
    if (rv->cg) rv->cg = (rv->cg << 4) + 0xF;
    rv->cb = (int32_t)(cv->col20[2] >> 12);
    if (rv->cb) rv->cb = (rv->cb << 4) + 0xF;

    rv->s = cv->s;
    rv->t = cv->t;
}

static void submit_polygon(Gx *gx)
{
    GxClipVertex clipped[10], temp[10];
    GxRasVertex rv[10];
    GxPolygon poly;
    const GxClipVertex *v = gx->temp_vtx;
    int nv = (gx->polygon_mode == 1) ? 4 : 3;
    int i, n, wsize;
    int64_t nx, ny, nz, dot;
    int facing_view;
    uint32_t attr = gx->cur_polygon_attr;

    gx->n_polys++;

    /* culling in clip space (melonDS GPU3D::SubmitPolygon): normal over the
     * (x, y, w) components of the raw polygon, normalized to 32 bits */
    nx = (int64_t)(int32_t)((uint32_t)v[0].clip[1] - (uint32_t)v[1].clip[1]) *
             (int32_t)((uint32_t)v[2].clip[3] - (uint32_t)v[1].clip[3]) -
         (int64_t)(int32_t)((uint32_t)v[0].clip[3] - (uint32_t)v[1].clip[3]) *
             (int32_t)((uint32_t)v[2].clip[1] - (uint32_t)v[1].clip[1]);
    ny = (int64_t)(int32_t)((uint32_t)v[0].clip[3] - (uint32_t)v[1].clip[3]) *
             (int32_t)((uint32_t)v[2].clip[0] - (uint32_t)v[1].clip[0]) -
         (int64_t)(int32_t)((uint32_t)v[0].clip[0] - (uint32_t)v[1].clip[0]) *
             (int32_t)((uint32_t)v[2].clip[3] - (uint32_t)v[1].clip[3]);
    nz = (int64_t)(int32_t)((uint32_t)v[0].clip[0] - (uint32_t)v[1].clip[0]) *
             (int32_t)((uint32_t)v[2].clip[1] - (uint32_t)v[1].clip[1]) -
         (int64_t)(int32_t)((uint32_t)v[0].clip[1] - (uint32_t)v[1].clip[1]) *
             (int32_t)((uint32_t)v[2].clip[0] - (uint32_t)v[1].clip[0]);
    while (nx != (int32_t)nx || ny != (int32_t)ny || nz != (int32_t)nz) {
        nx >>= 4; ny >>= 4; nz >>= 4;
    }
    dot = (int64_t)v[1].clip[0] * nx + (int64_t)v[1].clip[1] * ny +
          (int64_t)v[1].clip[3] * nz;
    facing_view = (dot <= 0); /* melonDS FacingView (GPU3D.cpp:1202) */
    if (dot < 0) {
        if (!(attr & (1u << 7))) { gx->n_culled_back++; return; }
    } else if (dot > 0) {
        if (!(attr & (1u << 6))) { gx->n_culled_front++; return; }
    }

    for (i = 0; i < nv; i++)
        clipped[i] = v[i];
    {
        static unsigned dbg_zero3 = 0, dbg_seen = 0;
        const char *e = getenv("NNK_GX_CLIPDBG");
        if (e && e[0]) {
            int k, zero3 = 1;
            for (k = 0; k < 12; k++) if (gx->clip_mat[k]) { zero3 = 0; break; }
            dbg_seen++;
            if (zero3) dbg_zero3++;
            if (dbg_seen % 200 == 1) {
                fprintf(stderr, "clipdbg seen=%u zero3x3=%u nv=%d v0=(%d,%d,%d) w=%d "
                                "m=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d attr=%08x\n",
                        dbg_seen, dbg_zero3, nv,
                        v[0].clip[0], v[0].clip[1], v[0].clip[2], v[0].clip[3],
                        gx->clip_mat[0], gx->clip_mat[1], gx->clip_mat[2], gx->clip_mat[3],
                        gx->clip_mat[4], gx->clip_mat[5], gx->clip_mat[6], gx->clip_mat[7],
                        gx->clip_mat[8], gx->clip_mat[9], gx->clip_mat[10], gx->clip_mat[11],
                        gx->clip_mat[12], gx->clip_mat[13], gx->clip_mat[14], gx->clip_mat[15],
                        gx->cur_polygon_attr);
            }
            if (dbg_seen == 1) {
                int j;
                fprintf(stderr, "clipdbg proj:");
                for (j = 0; j < 16; j++) fprintf(stderr, " %d", gx->proj[j]);
                fprintf(stderr, "\nclipdbg pos :");
                for (j = 0; j < 16; j++) fprintf(stderr, " %d", gx->pos[j]);
                fprintf(stderr, "\nclipdbg vtx :");
                for (j = 0; j < nv; j++) fprintf(stderr, " (%d,%d,%d|%d,%d,%d,%d)",
                        gx->temp_vtx[j].clip[0], gx->temp_vtx[j].clip[1],
                        gx->temp_vtx[j].clip[2], gx->temp_vtx[j].clip[3],
                        v[j].clip[0], v[j].clip[1], v[j].clip[2], v[j].clip[3]);
                fprintf(stderr, "\nclipdbg mode=%d pmode=%d vp=%d,%d,%d,%d wbuf=%d nverts=%u\n",
                        gx->matrix_mode, gx->polygon_mode, gx->vp_x0, gx->vp_y0,
                        gx->vp_w, gx->vp_h, gx->wbuffer, gx->n_verts);
                fprintf(stderr, "clipdbg fifo:");
                for (j = 0; j < 32; j++)
                    fprintf(stderr, " %08x", dbg_ring[(dbg_ring_i + j) & 31]);
                fprintf(stderr, "\n");
            }
        }
    }
    n = clip_polygon(gx, clipped, nv, temp);
    if (n < 3) {
        gx->n_drop_clip++;
        return;
    }

    /* W truncated to 24 bits; a zero W anywhere makes the polygon degenerate
     * (melonDS marks it Degenerate and the renderer skips it) */
    for (i = 0; i < n; i++) {
        clipped[i].clip[3] &= 0x00FFFFFF;
        if (clipped[i].clip[3] == 0) {
            gx->n_drop_degenerate++;
            return;
        }
    }

    /* per-polygon W normalization: the smallest multiple of 4 bits that
     * holds every W (melonDS SubmitPolygon, GPU3D.cpp:1498-1535) */
    wsize = 0;
    for (i = 0; i < n; i++) {
        uint32_t w = (uint32_t)clipped[i].clip[3];
        while ((w >> wsize) && (wsize < 32))
            wsize += 4;
    }

    for (i = 0; i < n; i++)
        fill_ras_vertex(gx, &clipped[i], &rv[i], wsize);

    /* zero-dot W check (melonDS GPU3D.cpp:1357-1390): a polygon whose
     * vertices all land on the same screen pixel is rejected when every W
     * is beyond the 0x04000610 threshold, unless POLYGON_ATTR bit 13 */
    if (!(gx->cur_polygon_attr & (1u << 13))) {
        int zerodot = 1, allbehind = 1;
        for (i = 0; i < n; i++) {
            if (rv[i].sx != rv[0].sx || rv[i].sy != rv[0].sy) {
                zerodot = 0;
                break;
            }
            if ((uint32_t)clipped[i].clip[3] <= gx->zero_dot_w) {
                allbehind = 0;
                break;
            }
        }
        if (zerodot && allbehind) {
            gx->n_drop_degenerate++;
            return;
        }
    }

    /* bottom bound check (melonDS GPU3D.cpp:1509) */
    {
        int32_t ybot = 0;
        for (i = 0; i < n; i++)
            if (rv[i].sy > ybot) ybot = rv[i].sy;
        if (ybot > 192) {
            gx->n_drop_degenerate++;
            return;
        }
    }

    memset(&poly, 0, sizeof(poly));
    poly.nv = n;
    poly.attr = gx->cur_polygon_attr;
    poly.texparam = gx->tex_param;
    poly.texpal = gx->tex_palette;
    poly.wbuffer = (uint8_t)gx->wbuffer;
    poly.facing_view = (uint8_t)facing_view;
    for (i = 0; i < n; i++)
        poly.v[i] = rv[i];

    gx->n_polys_drawn++;
    if (gx->draw_fn)
        gx->draw_fn(gx->draw_user, &poly);
    if (!gx->skip_software)
        gx_raster_polygon(&gx->surface, &poly, &gx->env);
}

/* ------------------------------------------------------------------ */
/* command execution                                                  */

static void exec_matrix_load(Gx *gx, const int32_t *p, int n)
{
    int32_t *m = matrix_target(gx);
    if (n == 16) gx_mat_load4x4(m, p);
    else         gx_mat_load4x3(m, p);
    if (gx->matrix_mode == 2) {
        if (n == 16) gx_mat_load4x4(gx->vec, p);
        else         gx_mat_load4x3(gx->vec, p);
    }
    matrix_dirty(gx);
}

static void exec_matrix_mult(Gx *gx, const int32_t *p, int kind)
{
    int32_t *m = matrix_target(gx);
    switch (kind) {
    case 16: gx_mat_mult4x4(m, p); break;
    case 12: gx_mat_mult4x3(m, p); break;
    default: gx_mat_mult3x3(m, p); break;
    }
    if (gx->matrix_mode == 2) {
        switch (kind) {
        case 16: gx_mat_mult4x4(gx->vec, p); break;
        case 12: gx_mat_mult4x3(gx->vec, p); break;
        default: gx_mat_mult3x3(gx->vec, p); break;
        }
    }
    matrix_dirty(gx);
}

static void exec_multi(Gx *gx, uint8_t cmd, const uint32_t *params)
{
    int32_t p[16];
    int i, n = gx_cmd_num_params[cmd];

    for (i = 0; i < n && i < 16; i++)
        p[i] = (int32_t)params[i];
    dbg_cmdlog(cmd, p, n);

    switch (cmd) {
    case 0x16: exec_matrix_load(gx, p, 16); break;
    case 0x17: exec_matrix_load(gx, p, 12); break;
    case 0x18: exec_matrix_mult(gx, p, 16); break;
    case 0x19: /* Phase-5BP diagnostic: measure what happens when the guest's
                * all-zero 4x3 multiply into the PROJECTION slot is skipped.
                * Never enabled in a normal run (NNK_GX_SKIPZEROMULT only). */
        if (gx->matrix_mode == 0 && getenv("NNK_GX_SKIPZEROMULT")) {
            int k, allzero = 1;
            for (k = 0; k < 12; k++) if (p[k]) { allzero = 0; break; }
            if (allzero) { dbg_zero_mult++; break; }
        }
        exec_matrix_mult(gx, p, 12);
        break;
    case 0x1A: exec_matrix_mult(gx, p, 9); break;

    case 0x1B: /* MTX_SCALE */
        gx_mat_scale(matrix_target(gx), p);
        /* VecMatrix is intentionally not scaled (hardware behavior) */
        matrix_dirty(gx);
        break;

    case 0x1C: /* MTX_TRANS */
        gx_mat_translate(matrix_target(gx), p);
        if (gx->matrix_mode == 2)
            gx_mat_translate(gx->vec, p);
        matrix_dirty(gx);
        break;

    case 0x23: /* VTX_16: x|y then z */
        gx->cur_vtx[0] = (int16_t)(params[0] & 0xFFFF);
        gx->cur_vtx[1] = (int16_t)(params[0] >> 16);
        gx->cur_vtx[2] = (int16_t)(params[1] & 0xFFFF);
        submit_vertex(gx);
        break;

    case 0x34: /* SHININESS: 128-byte table, 32 words */
        for (i = 0; i < 32; i++)
            for (int j = 0; j < 4; j++)
                gx->shininess[i * 4 + j] = (uint8_t)(params[i] >> (j * 8));
        break;

    case 0x70: /* BOX_TEST: stub */
    case 0x71: /* POS_TEST: stub */
        break;

    default:
        gx->n_unknown_cmds++;
        dbg_unknown_hist[cmd]++;
        break;
    }
}

static void exec_single(Gx *gx, uint8_t cmd, uint32_t param)
{
    int32_t pp[1];
    pp[0] = (int32_t)param;
    dbg_cmdlog(cmd, pp, 1);
    switch (cmd) {
    case 0x10: /* MTX_MODE */
        gx->matrix_mode = (int)(param & 0x3);
        break;

    case 0x11: /* MTX_PUSH */
        if (gx->matrix_mode == 0) {
            memcpy(gx->proj_stack, gx->proj, sizeof(gx->proj));
            gx->proj_sp = (gx->proj_sp + 1) & 0x1;
        } else if (gx->matrix_mode == 3) {
            memcpy(gx->tex_stack, gx->texm, sizeof(gx->texm));
            gx->tex_sp = (gx->tex_sp + 1) & 0x1;
        } else {
            memcpy(gx->pos_stack[gx->pos_sp & 0x1F], gx->pos, sizeof(gx->pos));
            memcpy(gx->vec_stack[gx->pos_sp & 0x1F], gx->vec, sizeof(gx->vec));
            gx->pos_sp = (gx->pos_sp + 1) & 0x3F;
        }
        break;

    case 0x12: /* MTX_POP */
        if (gx->matrix_mode == 0) {
            gx->proj_sp = (gx->proj_sp - 1) & 0x1;
            memcpy(gx->proj, gx->proj_stack, sizeof(gx->proj));
            gx->clip_dirty = 1;
        } else if (gx->matrix_mode == 3) {
            gx->tex_sp = (gx->tex_sp - 1) & 0x1;
            memcpy(gx->texm, gx->tex_stack, sizeof(gx->texm));
        } else {
            int32_t offset = (int32_t)(param << 26) >> 26; /* sign-extend 6 bits */
            gx->pos_sp = (uint32_t)((gx->pos_sp - offset) & 0x3F);
            memcpy(gx->pos, gx->pos_stack[gx->pos_sp & 0x1F], sizeof(gx->pos));
            memcpy(gx->vec, gx->vec_stack[gx->pos_sp & 0x1F], sizeof(gx->vec));
            gx->clip_dirty = 1;
        }
        break;

    case 0x13: /* MTX_STORE */
        if (gx->matrix_mode == 0) {
            memcpy(gx->proj_stack, gx->proj, sizeof(gx->proj));
        } else if (gx->matrix_mode == 3) {
            memcpy(gx->tex_stack, gx->texm, sizeof(gx->texm));
        } else {
            uint32_t addr = param & 0x1F;
            memcpy(gx->pos_stack[addr], gx->pos, sizeof(gx->pos));
            memcpy(gx->vec_stack[addr], gx->vec, sizeof(gx->vec));
        }
        break;

    case 0x14: /* MTX_RESTORE */
        if (gx->matrix_mode == 0) {
            memcpy(gx->proj, gx->proj_stack, sizeof(gx->proj));
            gx->clip_dirty = 1;
        } else if (gx->matrix_mode == 3) {
            memcpy(gx->texm, gx->tex_stack, sizeof(gx->texm));
        } else {
            uint32_t addr = param & 0x1F;
            memcpy(gx->pos, gx->pos_stack[addr], sizeof(gx->pos));
            memcpy(gx->vec, gx->vec_stack[addr], sizeof(gx->vec));
            gx->clip_dirty = 1;
        }
        break;

    case 0x15: /* MTX_IDENTITY */
        if (gx->matrix_mode == 0) {
            gx_mat_identity(gx->proj);
            gx->clip_dirty = 1;
        } else if (gx->matrix_mode == 3) {
            gx_mat_identity(gx->texm);
        } else {
            gx_mat_identity(gx->pos);
            if (gx->matrix_mode == 2)
                gx_mat_identity(gx->vec);
            gx->clip_dirty = 1;
        }
        break;

    case 0x20: /* COLOR */
        gx->vtx_color[0] = (uint8_t)(param & 0x1F);
        gx->vtx_color[1] = (uint8_t)((param >> 5) & 0x1F);
        gx->vtx_color[2] = (uint8_t)((param >> 10) & 0x1F);
        break;

    case 0x21: /* NORMAL */
        gx->normal[0] = (int16_t)((int16_t)((param & 0x000003FF) << 6) >> 6);
        gx->normal[1] = (int16_t)((int16_t)((param & 0x000FFC00) >> 4) >> 6);
        gx->normal[2] = (int16_t)((int16_t)((param & 0x3FF00000) >> 14) >> 6);
        calculate_lighting(gx);
        break;

    case 0x22: /* TEXCOORD (melonDS GPU3D.cpp:2132-2146: s16 semantics) */
        gx->raw_s = (int16_t)(param & 0xFFFF);
        gx->raw_t = (int16_t)(param >> 16);
        if ((gx->tex_param >> 30) == 1) {
            gx->cur_s = (int16_t)(int32_t)(((int64_t)gx->raw_s * gx->texm[0] +
                        (int64_t)gx->raw_t * gx->texm[4] + gx->texm[8] +
                        gx->texm[12]) >> 12);
            gx->cur_t = (int16_t)(int32_t)(((int64_t)gx->raw_s * gx->texm[1] +
                        (int64_t)gx->raw_t * gx->texm[5] + gx->texm[9] +
                        gx->texm[13]) >> 12);
        } else {
            gx->cur_s = gx->raw_s;
            gx->cur_t = gx->raw_t;
        }
        break;

    case 0x24: /* VTX_10 */
        gx->cur_vtx[0] = (int16_t)((param & 0x000003FF) << 6);
        gx->cur_vtx[1] = (int16_t)((param & 0x000FFC00) >> 4);
        gx->cur_vtx[2] = (int16_t)((param & 0x3FF00000) >> 14);
        submit_vertex(gx);
        break;

    case 0x25: /* VTX_XY */
        gx->cur_vtx[0] = (int16_t)(param & 0xFFFF);
        gx->cur_vtx[1] = (int16_t)(param >> 16);
        submit_vertex(gx);
        break;

    case 0x26: /* VTX_XZ */
        gx->cur_vtx[0] = (int16_t)(param & 0xFFFF);
        gx->cur_vtx[2] = (int16_t)(param >> 16);
        submit_vertex(gx);
        break;

    case 0x27: /* VTX_YZ */
        gx->cur_vtx[1] = (int16_t)(param & 0xFFFF);
        gx->cur_vtx[2] = (int16_t)(param >> 16);
        submit_vertex(gx);
        break;

    case 0x28: /* VTX_DIFF */
        gx->cur_vtx[0] = (int16_t)(gx->cur_vtx[0] + (((int16_t)((param & 0x000003FF) << 6)) >> 6));
        gx->cur_vtx[1] = (int16_t)(gx->cur_vtx[1] + (((int16_t)((param & 0x000FFC00) >> 4)) >> 6));
        gx->cur_vtx[2] = (int16_t)(gx->cur_vtx[2] + (((int16_t)((param & 0x3FF00000) >> 14)) >> 6));
        submit_vertex(gx);
        break;

    case 0x29: /* POLYGON_ATTR */
        gx->polygon_attr = param;
        break;

    case 0x2A: /* TEXIMAGE_PARAM */
        gx->tex_param = param;
        break;

    case 0x2B: /* PLTT_BASE */
        gx->tex_palette = param & 0x1FFF;
        break;

    case 0x30: /* DIF_AMB */
        gx->mat_diff[0] = (uint8_t)(param & 0x1F);
        gx->mat_diff[1] = (uint8_t)((param >> 5) & 0x1F);
        gx->mat_diff[2] = (uint8_t)((param >> 10) & 0x1F);
        gx->mat_amb[0] = (uint8_t)((param >> 16) & 0x1F);
        gx->mat_amb[1] = (uint8_t)((param >> 21) & 0x1F);
        gx->mat_amb[2] = (uint8_t)((param >> 26) & 0x1F);
        if (param & 0x8000) {
            gx->vtx_color[0] = gx->mat_diff[0];
            gx->vtx_color[1] = gx->mat_diff[1];
            gx->vtx_color[2] = gx->mat_diff[2];
        }
        break;

    case 0x31: /* SPE_EMI */
        gx->mat_spec[0] = (uint8_t)(param & 0x1F);
        gx->mat_spec[1] = (uint8_t)((param >> 5) & 0x1F);
        gx->mat_spec[2] = (uint8_t)((param >> 10) & 0x1F);
        gx->mat_emi[0] = (uint8_t)((param >> 16) & 0x1F);
        gx->mat_emi[1] = (uint8_t)((param >> 21) & 0x1F);
        gx->mat_emi[2] = (uint8_t)((param >> 26) & 0x1F);
        gx->use_shininess = (param & 0x8000) != 0;
        break;

    case 0x32: { /* LIGHT_VECTOR */
        uint32_t l = param >> 30;
        int16_t dir[3];
        int64_t vx, vy, vz;
        dir[0] = (int16_t)((int16_t)((param & 0x000003FF) << 6) >> 6);
        dir[1] = (int16_t)((int16_t)((param & 0x000FFC00) >> 4) >> 6);
        dir[2] = (int16_t)((int16_t)((param & 0x3FF00000) >> 14) >> 6);
        vx = (int64_t)dir[0] * gx->vec[0] + (int64_t)dir[1] * gx->vec[4] + (int64_t)dir[2] * gx->vec[8];
        vy = (int64_t)dir[0] * gx->vec[1] + (int64_t)dir[1] * gx->vec[5] + (int64_t)dir[2] * gx->vec[9];
        vz = (int64_t)dir[0] * gx->vec[2] + (int64_t)dir[1] * gx->vec[6] + (int64_t)dir[2] * gx->vec[10];
        gx->light_dir[l][0] = (int32_t)(((-(vx >> 12)) << 21) >> 21);
        gx->light_dir[l][1] = (int32_t)(((-(vy >> 12)) << 21) >> 21);
        gx->light_dir[l][2] = (int32_t)(((-(vz >> 12)) << 21) >> 21);
        {
            int32_t den = (int32_t)(-((vz << 9) >> 21)) + (1 << 9);
            gx->spec_recip[l] = (den == 0) ? 0 : (int32_t)((1 << 18) / den);
        }
        break;
    }

    case 0x33: { /* LIGHT_COLOR */
        uint32_t l = param >> 30;
        gx->light_color[l][0] = (uint8_t)(param & 0x1F);
        gx->light_color[l][1] = (uint8_t)((param >> 5) & 0x1F);
        gx->light_color[l][2] = (uint8_t)((param >> 10) & 0x1F);
        break;
    }

    case 0x40: /* BEGIN_VTXS */
        gx->polygon_mode = (int)(param & 0x3);
        gx->vtx_in_poly = 0;
        gx->vtx_count = 0;
        gx->num_consecutive = 0;
        gx->cur_polygon_attr = gx->polygon_attr;
        break;

    case 0x41: /* END_VTXS */
        break;

    case 0x50: /* SWAP_BUFFERS: rendering is immediate; nothing queued.
                * Param bit 1 selects W-buffering for polygons submitted
                * after this command (melonDS GPU3D.cpp:2284, 1514). */
        gx->wbuffer = (int)((param >> 1) & 1);
        break;

    case 0x60: /* VIEWPORT */
        /* x1,y1 = top-left, x2,y2 = bottom-right (GBATEK reading; see
         * GX_COVERAGE.md for the melonDS Y-flip discussion) */
        gx->vp_x0 = (int32_t)(param & 0xFF);
        gx->vp_y0 = (int32_t)((param >> 8) & 0xFF);
        gx->vp_w = (int32_t)((param >> 16) & 0xFF) - gx->vp_x0 + 1;
        gx->vp_h = (int32_t)((param >> 24) & 0xFF) - gx->vp_y0 + 1;
        break;

    case 0x72: /* VEC_TEST: stub */
        break;

    default:
        gx->n_unknown_cmds++;
        dbg_unknown_hist[cmd]++;
        break;
    }
}

/* Phase-5BP: dump the matrix / setup command stream (the words that decide the
 * 3D transform) for the first N commands, N = NNK_GX_CMDBG. */
static void dbg_cmdlog(uint8_t cmd, const int32_t *p, int n)
{
    const char *e;
    int i;
    if (!(cmd <= 0x1F || cmd == 0x40 || cmd == 0x41 || cmd == 0x50 || cmd == 0x60))
        return;
    e = getenv("NNK_GX_CMDBG");
    if (!e || !e[0]) return;
    if (dbg_cmd_n >= (unsigned)strtoul(e, NULL, 0)) return;
    dbg_cmd_n++;
    fprintf(stderr, "cmdbg #%u cmd=%02x n=%d:", dbg_cmd_n, cmd, n);
    for (i = 0; i < n; i++) fprintf(stderr, " %d(%08x)", p[i], (uint32_t)p[i]);
    fprintf(stderr, "\n");
}

static void gx_exec(Gx *gx, uint8_t cmd, uint32_t param)
{
    int nparams = gx_cmd_num_params[cmd];

    if (nparams <= 1) {
        exec_single(gx, cmd, param);
        return;
    }

    gx->exec_params[gx->exec_param_count++] = param;
    if (gx->exec_param_count >= nparams) {
        gx->exec_param_count = 0;
        exec_multi(gx, cmd, gx->exec_params);
    }
}

/* Phase-5BL: FIFO occupancy and GXSTAT bookkeeping, both derived from the
 * decoder state above (never stored as a constant).  melonDS's GPU3D keeps
 * these in GXStat/CmdFIFO; ours must follow OUR FIFO, otherwise a status read
 * makes the guest's poll pass once and then lie. */
uint32_t gx_fifo_level(const Gx *gx)
{
    uint32_t lvl = 0;
    if (gx->num_commands > 0) lvl += (uint32_t)gx->num_commands;
    if (gx->total_params > 0 && gx->param_count < gx->total_params) lvl += 1u;
    return lvl;
}

static void gx_fifo_sync_gxstat(Gx *gx)
{
    uint32_t pend = 0, test = 0;
    uint32_t lvl = gx_fifo_level(gx);
    uint32_t c = gx->cur_command;
    int i;

    if (gx->total_params > 0 && gx->param_count < gx->total_params) {
        uint8_t cmd = (uint8_t)(c & 0xFF);
        if (cmd == 0x11 || cmd == 0x12) pend = 1;
        if (cmd == 0x70 || cmd == 0x71 || cmd == 0x72) test = 1;
    }
    for (i = 0; i < gx->num_commands; i++) {
        uint8_t cmd = (uint8_t)(c >> (8 * i));
        if (cmd == 0x11 || cmd == 0x12) pend = 1;
        if (cmd == 0x70 || cmd == 0x71 || cmd == 0x72) test = 1;
    }
    gx->gxstat = (gx->gxstat & ~0x08004001u)
               | (lvl ? 0x08000000u : 0u)    /* bit27: commands still queued */
               | (pend ? 0x00004000u : 0u)   /* bit14: push/pop pending */
               | (test ? 0x00000001u : 0u);  /* bit0:  pos/vec test pending */
}

/* GXFIFO word state machine (mirrors melonDS GPU3D::WriteToGXFIFO):
 * a fresh word holds up to 4 packed command bytes; each command then
 * consumes its parameters from the following words. */
void gx_fifo_write(Gx *gx, uint32_t val)
{
    dbg_ring[dbg_ring_i] = val;
    dbg_ring_i = (dbg_ring_i + 1) & 31;
    {
        static long dbg_wn = -1;
        static unsigned long dbg_lim;
        if (dbg_wn < 0) {
            const char *e = getenv("NNK_GX_WORDS");
            dbg_wn = 0;
            dbg_lim = (e && e[0]) ? strtoul(e, NULL, 0) : 0;
        }
        if (dbg_lim && (unsigned long)dbg_wn < dbg_lim)
            fprintf(stderr, "word #%ld %08x\n", dbg_wn, val);
        dbg_wn++;
    }
    if (gx->num_commands == 0) {
        gx->num_commands = 4;
        gx->cur_command = val;
        gx->param_count = 0;
        gx->total_params = gx_cmd_num_params[val & 0xFF];
        if (gx->total_params > 0) {
            gx_fifo_sync_gxstat(gx);
            return;
        }
    } else {
        gx->param_count++;
    }

    for (;;) {
        if ((gx->cur_command & 0xFF) ||
            (gx->num_commands == 4 && gx->cur_command == 0)) {
            gx_exec(gx, (uint8_t)(gx->cur_command & 0xFF), val);
        }

        if (gx->param_count >= gx->total_params) {
            gx->cur_command >>= 8;
            gx->num_commands--;
            if (gx->num_commands == 0)
                break;
            gx->param_count = 0;
            gx->total_params = gx_cmd_num_params[gx->cur_command & 0xFF];
        }
        if (gx->param_count < gx->total_params)
            break;
    }
    gx_fifo_sync_gxstat(gx);
}

void gx_fifo_write_block(Gx *gx, const uint32_t *words, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        gx_fifo_write(gx, words[i]);
}

void gx_cmd_entry(Gx *gx, uint8_t cmd, uint32_t param)
{
    gx_exec(gx, cmd, param);
    gx_fifo_sync_gxstat(gx);
}
