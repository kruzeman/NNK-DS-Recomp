/* gx_fifo.c — DS GX command FIFO decoder + geometry vertex pipeline.
 * Command semantics follow melonDS src/GPU3D.cpp (see GX_COVERAGE.md for the
 * implemented/stubbed command table).  All arithmetic is integer.
 */
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

    v->r = gx->vtx_color[0];
    v->g = gx->vtx_color[1];
    v->b = gx->vtx_color[2];

    if ((gx->tex_param >> 30) == 3) {
        /* texcoords from vertex position */
        v->s = (int32_t)(((p[0] * gx->texm[0] + p[1] * gx->texm[4] + p[2] * gx->texm[8]) >> 24) + gx->raw_s);
        v->t = (int32_t)(((p[0] * gx->texm[1] + p[1] * gx->texm[5] + p[2] * gx->texm[9]) >> 24) + gx->raw_t);
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

/* Clip a polygon against the near plane (clip_w >= GX_NEAR_W).  All vertex
 * attributes are interpolated linearly in clip space.  Returns the clipped
 * vertex count (up to 6).  Full 6-plane frustum clipping is staged. */
#define GX_NEAR_W 0x40 /* 1/64 in 1.19.12 */

static int clip_near(const GxClipVertex *in, int nin, GxClipVertex *out)
{
    int n = 0, i;
    for (i = 0; i < nin; i++) {
        const GxClipVertex *a = &in[i];
        const GxClipVertex *b = &in[(i + 1) % nin];
        int ain = a->clip[3] >= GX_NEAR_W;
        int bin = b->clip[3] >= GX_NEAR_W;
        if (ain)
            out[n++] = *a;
        if (ain != bin) {
            /* intersection: t = (nearw - a.w) / (b.w - a.w) in 1.19.12 */
            int64_t dw = b->clip[3] - a->clip[3];
            int64_t t;
            GxClipVertex *o;
            if (dw == 0)
                continue;
            t = ((int64_t)(GX_NEAR_W - a->clip[3]) << 12) / dw;
            o = &out[n];
            o->clip[0] = a->clip[0] + (int32_t)(((int64_t)(b->clip[0] - a->clip[0]) * t) >> 12);
            o->clip[1] = a->clip[1] + (int32_t)(((int64_t)(b->clip[1] - a->clip[1]) * t) >> 12);
            o->clip[2] = a->clip[2] + (int32_t)(((int64_t)(b->clip[2] - a->clip[2]) * t) >> 12);
            o->clip[3] = GX_NEAR_W;
            o->s = a->s + (int32_t)(((int64_t)(b->s - a->s) * t) >> 12);
            o->t = a->t + (int32_t)(((int64_t)(b->t - a->t) * t) >> 12);
            o->r = (uint8_t)gx_clamp32(a->r + (((int64_t)(b->r - a->r) * t) >> 12), 0, 31);
            o->g = (uint8_t)gx_clamp32(a->g + (((int64_t)(b->g - a->g) * t) >> 12), 0, 31);
            o->b = (uint8_t)gx_clamp32(a->b + (((int64_t)(b->b - a->b) * t) >> 12), 0, 31);
            n++;
        }
        if (n >= 6)
            break;
    }
    return n;
}

static void fill_ras_vertex(Gx *gx, const GxClipVertex *cv, GxRasVertex *rv)
{
    int64_t w = cv->clip[3];
    int32_t wbuffer = (int32_t)((gx->cur_polygon_attr >> 14) & 1);

    /* screen mapping: clip x/y in [-w, w] -> viewport, y down.
     * Coordinates are clamped to +/-512 px: real frustum clipping of
     * off-screen vertices is staged (see GX_COVERAGE.md). */
    rv->sx = gx_clamp32((((int64_t)cv->clip[0] + w) * gx->vp_w * GX_SCREEN_ONE) / (w * 2) +
                        (int64_t)gx->vp_x0 * GX_SCREEN_ONE, -(1 << 13), 1 << 13);
    rv->sy = gx_clamp32((((int64_t)w - cv->clip[1]) * gx->vp_h * GX_SCREEN_ONE) / (w * 2) +
                        (int64_t)gx->vp_y0 * GX_SCREEN_ONE, -(1 << 13), 1 << 13);

    if (wbuffer) {
        rv->depth = gx_clamp32(w, -(1 << 28), 1 << 28);
    } else {
        rv->depth = gx_clamp32(((int64_t)cv->clip[2] << 12) / w, -(1 << 28), 1 << 28);
    }
    rv->invw = (int32_t)(((int64_t)1 << GX_INVW_SHIFT) / w);
    if (rv->invw < 256) rv->invw = 256; /* keeps I_i = invw>>8 nonzero */

    rv->s = cv->s;
    rv->t = cv->t;
    rv->r = cv->r;
    rv->g = cv->g;
    rv->b = cv->b;
}

static void submit_triangle(Gx *gx, const GxClipVertex *a,
                            const GxClipVertex *b, const GxClipVertex *c)
{
    GxPolygon poly;
    int64_t area2;
    uint32_t attr = gx->cur_polygon_attr;

    memset(&poly, 0, sizeof(poly));
    poly.nv = 3;
    poly.attr = attr;
    poly.texparam = gx->tex_param;
    poly.texpal = gx->tex_palette;

    fill_ras_vertex(gx, a, &poly.v[0]);
    fill_ras_vertex(gx, b, &poly.v[1]);
    fill_ras_vertex(gx, c, &poly.v[2]);

    area2 = gx_poly_area2(&poly);
    if (area2 == 0)
        return;
    /* DS: front faces wind clockwise on screen (area2 > 0) */
    if (area2 > 0) {
        if (!(attr & (1u << 6))) return; /* front face disabled */
    } else {
        if (!(attr & (1u << 7))) return; /* back face disabled */
    }

    gx->n_polys_drawn++;
    if (gx->draw_fn)
        gx->draw_fn(gx->draw_user, &poly);
    if (!gx->skip_software)
        gx_raster_polygon(&gx->surface, &poly, &gx->env);
}

static void submit_polygon(Gx *gx)
{
    GxClipVertex clipped[6];
    const GxClipVertex *v = gx->temp_vtx;
    int nv = (gx->polygon_mode == 1) ? 4 : 3;
    int i, n;

    gx->n_polys++;

    for (i = 0; i < nv; i++) {
        if (v[i].clip[3] < GX_NEAR_W) {
            n = clip_near(v, nv, clipped);
            if (n < 3)
                return;
            for (i = 1; i + 1 < n; i++)
                submit_triangle(gx, &clipped[0], &clipped[i], &clipped[i + 1]);
            return;
        }
    }

    if (nv == 4) {
        /* submit as a proper quad (rasterizer fans it 0-1-2 / 0-2-3) */
        GxPolygon poly;
        int64_t area2;
        uint32_t attr = gx->cur_polygon_attr;

        memset(&poly, 0, sizeof(poly));
        poly.nv = 4;
        poly.attr = attr;
        poly.texparam = gx->tex_param;
        poly.texpal = gx->tex_palette;
        for (i = 0; i < 4; i++)
            fill_ras_vertex(gx, &v[i], &poly.v[i]);

        area2 = gx_poly_area2(&poly);
        if (area2 == 0)
            return;
        if (area2 > 0) {
            if (!(attr & (1u << 6))) return;
        } else {
            if (!(attr & (1u << 7))) return;
        }

        gx->n_polys_drawn++;
        if (gx->draw_fn)
            gx->draw_fn(gx->draw_user, &poly);
        if (!gx->skip_software)
            gx_raster_polygon(&gx->surface, &poly, &gx->env);
        return;
    }

    submit_triangle(gx, &v[0], &v[1], &v[2]);
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

    switch (cmd) {
    case 0x16: exec_matrix_load(gx, p, 16); break;
    case 0x17: exec_matrix_load(gx, p, 12); break;
    case 0x18: exec_matrix_mult(gx, p, 16); break;
    case 0x19: exec_matrix_mult(gx, p, 12); break;
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
        break;
    }
}

static void exec_single(Gx *gx, uint8_t cmd, uint32_t param)
{
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

    case 0x22: /* TEXCOORD */
        gx->raw_s = (int16_t)(param & 0xFFFF);
        gx->raw_t = (int16_t)(param >> 16);
        if ((gx->tex_param >> 30) == 1) {
            gx->cur_s = (int32_t)(((int64_t)gx->raw_s * gx->texm[0] +
                        (int64_t)gx->raw_t * gx->texm[4] + gx->texm[8] +
                        gx->texm[12]) >> 12);
            gx->cur_t = (int32_t)(((int64_t)gx->raw_s * gx->texm[1] +
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

    case 0x50: /* SWAP_BUFFERS: rendering is immediate; nothing queued */
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
        break;
    }
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

/* GXFIFO word state machine (mirrors melonDS GPU3D::WriteToGXFIFO):
 * a fresh word holds up to 4 packed command bytes; each command then
 * consumes its parameters from the following words. */
void gx_fifo_write(Gx *gx, uint32_t val)
{
    if (gx->num_commands == 0) {
        gx->num_commands = 4;
        gx->cur_command = val;
        gx->param_count = 0;
        gx->total_params = gx_cmd_num_params[val & 0xFF];
        if (gx->total_params > 0)
            return;
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
}
