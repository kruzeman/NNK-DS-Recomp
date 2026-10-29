/* gx_replay_common.h -- shared replay frame model for the melonDS GX
 * capture traces (f%05u.gx records, see GX_TRACES.md).
 *
 * This is the single source of truth for the env latching / frame model
 * used by BOTH replay backends:
 *   - tools/gx_replay.c    (software path, golden reference)
 *   - tools/gx_glreplay.c  (OpenGL path, validated against the software)
 * Both must replay streams identically: render env regs latch at
 * SWAP_BUFFERS flushes (GPU3D::VBlank), submitted polygons are retained and
 * the last flushed set is re-rendered every frame (RenderPolygonRAM/
 * RenderNumPolygons -- a flush with no polygons keeps the previous set),
 * render order is melonDS YSort (opaque by SortKey (ybot<<8)|ytop, then
 * translucent after in submission order / per SWAP sort mode bit 0).
 *
 * Include after gx_fifo.h / gx_raster.h.
 */
#ifndef GX_REPLAY_COMMON_H
#define GX_REPLAY_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* melonDS GPU3D.cpp CmdNumParams (word-decode tracker needs it). */
static const uint8_t rx_cmd_np[256] = {
    /* 0x00 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x10 */ 1,0,1,1,1,0,16,12,16,12,9,3,3,0,0,0,
    /* 0x20 */ 1,1,1,2,1,1,1,1,1,1,1,1,0,0,0,0,
    /* 0x30 */ 1,1,1,1,32,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x40 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x50 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x60 */ 1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x70 */ 3,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* ---- retained polygon lists (melonDS RenderPolygonRAM model) ---- */

typedef struct {
    GxPolygon *poly;
    int n, cap;
} RxPolyList;

static void rx_list_push(RxPolyList *l, const GxPolygon *p)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->poly = realloc(l->poly, (size_t)l->cap * sizeof(GxPolygon));
        if (!l->poly) { fprintf(stderr, "gx_replay: out of memory\n"); exit(1); }
    }
    l->poly[l->n++] = *p;
}

/* ---- melonDS render-env register state ---- */

typedef struct {
    uint32_t disp_cnt;
    uint32_t alpha_ref_val;
    uint32_t clear_attr1;
    uint32_t clear_attr2;
    uint32_t fog_color;
    uint32_t fog_offset;
    uint8_t fog_density[32];
    uint16_t edge_table[8];
    uint16_t toon_table[32];
    uint32_t zero_dot_w; /* 0x04000610: ZeroDotWLimit = val*0x200+0x1FF */
} RxEnvRegs;

/* Apply one MMIO write exactly like melonDS GPU3D::Write8/16/32. */
static void rx_apply_reg(RxEnvRegs *e, uint32_t addr, int width, uint32_t val)
{
    switch (addr) {
    case 0x04000060:
        e->disp_cnt = (val & 0x4FFF) | (e->disp_cnt & 0x3000);
        if (val & (1u << 12)) e->disp_cnt &= ~(1u << 12);
        if (val & (1u << 13)) e->disp_cnt &= ~(1u << 13);
        return;
    case 0x04000340:
        e->alpha_ref_val = val & 0x1F;
        return;
    case 0x04000350:
        if (width == 2) e->clear_attr1 = (e->clear_attr1 & 0xFFFF0000) | val;
        else            e->clear_attr1 = val;
        return;
    case 0x04000352:
        e->clear_attr1 = (e->clear_attr1 & 0xFFFF) | (val << 16);
        return;
    case 0x04000354:
        if (width == 2) e->clear_attr2 = (e->clear_attr2 & 0xFFFF0000) | val;
        else            e->clear_attr2 = val;
        return;
    case 0x04000356:
        e->clear_attr2 = (e->clear_attr2 & 0xFFFF) | (val << 16);
        return;
    case 0x04000358:
        if (width == 2) e->fog_color = (e->fog_color & 0xFFFF0000) | val;
        else            e->fog_color = val;
        return;
    case 0x0400035A:
        e->fog_color = (e->fog_color & 0xFFFF) | (val << 16);
        return;
    case 0x0400035C:
        e->fog_offset = val & 0x7FFF;
        return;
    case 0x04000610:
        e->zero_dot_w = ((val & 0x7FFF) * 0x200) + 0x1FF;
        return;
    }
    if (addr >= 0x04000330 && addr < 0x04000340) {
        if (width == 1) ((uint8_t *)e->edge_table)[addr - 0x04000330] = (uint8_t)val;
        else if (width == 2) e->edge_table[(addr - 0x04000330) >> 1] = (uint16_t)val;
        else {
            uint32_t i = (addr - 0x04000330) >> 1;
            e->edge_table[i] = val & 0xFFFF;
            e->edge_table[i + 1] = val >> 16;
        }
        return;
    }
    if (addr >= 0x04000360 && addr < 0x04000380) {
        uint32_t o = addr - 0x04000360;
        if (width == 1) e->fog_density[o] = val & 0x7F;
        else if (width == 2) {
            e->fog_density[o] = val & 0x7F;
            e->fog_density[o + 1] = (val >> 8) & 0x7F;
        } else {
            e->fog_density[o] = val & 0x7F;
            e->fog_density[o + 1] = (val >> 8) & 0x7F;
            e->fog_density[o + 2] = (val >> 16) & 0x7F;
            e->fog_density[o + 3] = (val >> 24) & 0x7F;
        }
        return;
    }
    if (addr >= 0x04000380 && addr < 0x040003C0) {
        uint32_t o = (addr - 0x04000380) >> 1;
        if (width == 1) ((uint8_t *)e->toon_table)[addr - 0x04000380] = (uint8_t)val;
        else if (width == 2) e->toon_table[o] = (uint16_t)val;
        else {
            e->toon_table[o] = val & 0xFFFF;
            e->toon_table[o + 1] = val >> 16;
        }
        return;
    }
}

/* ---- word-decode tracker (records which commands executed) ---- */

typedef struct {
    uint32_t cur;
    int num, pc, tp;
    int saw_swap, saw_vtx, n_entries;
    uint32_t last_swap_param;
} RxTracker;

static void rx_tracker_entry(RxTracker *t, uint8_t cmd, uint32_t param)
{
    if (cmd == 0x50) { t->saw_swap = 1; t->last_swap_param = param; }
    if (cmd >= 0x23 && cmd <= 0x28) t->saw_vtx = 1;
    t->n_entries++;
}

static void rx_tracker_word(RxTracker *t, uint32_t val)
{
    if (t->num == 0) {
        t->num = 4;
        t->cur = val;
        t->pc = 0;
        t->tp = rx_cmd_np[val & 0xFF];
        if (t->tp > 0) return;
    } else {
        t->pc++;
    }
    for (;;) {
        if ((t->cur & 0xFF) || (t->num == 4 && t->cur == 0))
            rx_tracker_entry(t, (uint8_t)(t->cur & 0xFF), val);
        if (t->pc >= t->tp) {
            t->cur >>= 8;
            t->num--;
            if (t->num == 0) break;
            t->pc = 0;
            t->tp = rx_cmd_np[t->cur & 0xFF];
        }
        if (t->pc < t->tp) break;
    }
}

/* ---- replay model state ---- */

typedef struct {
    Gx gx;
    RxPolyList pending;   /* submitted since the last flush */
    RxPolyList current;   /* set rendered every frame */
    RxPolyList scratch;   /* sort workspace */
    RxEnvRegs live, latched;
    uint32_t latched_sort; /* SWAP_BUFFERS param & 3 (FlushAttributes) */
    RxTracker tr;          /* tracker of the most recent fed unit */
} RxModel;

static void rx_retain_poly(void *user, const GxPolygon *poly)
{
    rx_list_push((RxPolyList *)user, poly);
}

/* Init the model on top of an initialized surface (color/depth buffers are
 * only used by the software rasterizer path). */
static void rx_init(RxModel *m, uint32_t *color, uint32_t *depth, int w, int h)
{
    memset(m, 0, sizeof(*m));
    gx_init(&m->gx, color, depth, w, h);
    m->gx.skip_software = 1;
    m->gx.draw_fn = rx_retain_poly;
    m->gx.draw_user = &m->pending;
    m->live.disp_cnt = m->latched.disp_cnt = 0x9;
}

/* Feed one unit's record stream (the parsed contents of f%05u.gx).  Applies
 * the melonDS latch model: a unit containing SWAP_BUFFERS latches the live
 * env regs and swaps the retained polygon sets.  Returns 0, or the 1-based
 * record offset of a bad record (also prints a diagnostic). */
static int rx_feed_unit(RxModel *m, const uint8_t *data, size_t len)
{
    size_t off = 0;
    memset(&m->tr, 0, sizeof(m->tr));
    while (off < len) {
        uint8_t type = data[off++];
        if (type == 0x01 && off + 4 <= len) {
            uint32_t val;
            memcpy(&val, data + off, 4);
            off += 4;
            gx_fifo_write(&m->gx, val);
            rx_tracker_word(&m->tr, val);
        } else if (type == 0x02 && off + 9 <= len) {
            uint8_t width = data[off];
            uint32_t addr, val;
            memcpy(&addr, data + off + 1, 4);
            memcpy(&val, data + off + 5, 4);
            off += 9;
            rx_apply_reg(&m->live, addr, width, val);
            m->gx.zero_dot_w = m->live.zero_dot_w;
        } else if (type == 0x03 && off + 5 <= len) {
            uint8_t cmd = data[off];
            uint32_t val;
            memcpy(&val, data + off + 1, 4);
            off += 5;
            gx_cmd_entry(&m->gx, cmd, val);
            rx_tracker_entry(&m->tr, cmd, val);
        } else {
            return (int)off;
        }
    }
    if (m->tr.saw_swap) {
        m->latched = m->live;
        m->latched_sort = m->tr.last_swap_param & 3;
        if (m->pending.n > 0) {
            RxPolyList tmp = m->current;
            m->current = m->pending;
            m->pending = tmp;
            m->pending.n = 0;
        }
    }
    return 0;
}

/* ---- frame clear (melonDS SoftRenderer3D::ClearBuffers, 1623-1710) ---- */

static void rx_clear_frame(GxSurface *sf, const RxEnvRegs *live,
                           const uint8_t *texvram, int fb_w, int fb_h)
{
    uint32_t clearz = ((live->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
    uint32_t polyid = live->clear_attr1 & 0x3F008000; /* clear polygon ID */

    if (live->disp_cnt & (1u << 14)) {
        /* clear-image mode: bitmap in texture VRAM at 40000h/60000h */
        uint8_t xoff = (live->clear_attr2 >> 16) & 0xFF;
        uint8_t yoff = (live->clear_attr2 >> 24) & 0xFF;
        int x, y;
        for (y = 0; y < fb_h; y++) {
            for (x = 0; x < fb_w; x++) {
                uint32_t off = (yoff << 9) + (xoff << 1);
                uint32_t val2 = texvram[0x40000 + off] | (texvram[0x40000 + off + 1] << 8);
                uint32_t val3 = texvram[0x60000 + off] | (texvram[0x60000 + off + 1] << 8);
                uint32_t r = (val2 << 1) & 0x3E; if (r) r++;
                uint32_t g = (val2 >> 4) & 0x3E; if (g) g++;
                uint32_t b = (val2 >> 9) & 0x3E; if (b) b++;
                uint32_t a = (val2 & 0x8000) ? 0x1F000000 : 0;
                sf->color[y * fb_w + x] = r | (g << 8) | (b << 16) | a;
                sf->depth[y * fb_w + x] = ((val3 & 0x7FFF) * 0x200) + 0x1FF;
                if (sf->attr)
                    sf->attr[y * fb_w + x] = polyid | (val3 & 0x8000);
                xoff++;
            }
            yoff++;
        }
    } else {
        uint32_t r = (live->clear_attr1 << 1) & 0x3E; if (r) r++;
        uint32_t g = (live->clear_attr1 >> 4) & 0x3E; if (g) g++;
        uint32_t b = (live->clear_attr1 >> 9) & 0x3E; if (b) b++;
        uint32_t a = (live->clear_attr1 >> 16) & 0x1F;
        polyid |= (live->clear_attr1 & 0x8000); /* clear fog flag */
        gx_surface_clear(sf, r | (g << 8) | (b << 16) | (a << 24), clearz,
                         polyid);
    }
}

/* ---- polygon ordering (melonDS YSort) ---- */

static int rx_poly_sortkey(const GxPolygon *p)
{
    /* melonDS SortKey = (ybot << 8) | ytop on FinalPosition Y rows
     * (GPU3D.cpp:1511); opaque polygons with lower bottom Y come first */
    int i, ymin = 1 << 30, ymax = -(1 << 30);
    for (i = 0; i < p->nv; i++) {
        int y = p->v[i].sy;
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    return (ymax << 8) | ymin;
}

static int rx_poly_translucent(const GxPolygon *p)
{
    uint32_t texfmt = (p->texparam >> 26) & 0x7;
    uint32_t alpha = (p->attr >> 16) & 0x1F;
    return (texfmt == 1 || texfmt == 6) || (alpha > 0 && alpha < 31);
}

static int rx_cmp_sortkey(const void *a, const void *b)
{
    const GxPolygon *pa = a, *pb = b;
    return rx_poly_sortkey(pa) - rx_poly_sortkey(pb);
}

/* Build the render order in m->scratch: [opaque sorted by SortKey (stable)]
 * [translucent in submission order] -- mirrors GPU3D::VBlank's flush +
 * YSort, using m->latched_sort (SWAP sort mode bit 0). */
static void rx_build_render_order(RxModel *m)
{
    int i;
    RxPolyList *current = &m->current, *scratch = &m->scratch;
    int sort_opaque_only = (int)(m->latched_sort & 1);
    scratch->n = 0;
    for (i = 0; i < current->n; i++)
        if (!rx_poly_translucent(&current->poly[i]))
            rx_list_push(scratch, &current->poly[i]);
    /* stable insertion sort by sortkey */
    for (i = 1; i < scratch->n; i++) {
        GxPolygon tmp = scratch->poly[i];
        int j = i - 1;
        while (j >= 0 && rx_cmp_sortkey(&scratch->poly[j], &tmp) > 0) {
            scratch->poly[j + 1] = scratch->poly[j];
            j--;
        }
        scratch->poly[j + 1] = tmp;
    }
    int opaque_n = scratch->n;
    if (sort_opaque_only) {
        /* SWAP sort mode 1 (FlushAttributes bit 0): only the opaque
         * region is Y-sorted, translucents keep submission order
         * (GPU3D.cpp:2669-2671 sorts [0, NumOpaquePolygons)) */
        for (i = 0; i < current->n; i++)
            if (rx_poly_translucent(&current->poly[i]))
                rx_list_push(scratch, &current->poly[i]);
    } else {
        /* SWAP sort mode 0: Y-sort the translucent region too.  melonDS
         * stable_sorts the whole list (GPU3D.cpp:2669-2671) but its
         * SortKey |= 0x10000 for translucent polygons
         * (GPU3D.cpp:1511-1512), so translucents always stay behind
         * every opaque polygon and are only ordered among themselves --
         * sorting the translucent region in place (bounded at opaque_n)
         * reproduces that exactly. */
        for (i = 0; i < current->n; i++)
            if (rx_poly_translucent(&current->poly[i]))
                rx_list_push(scratch, &current->poly[i]);
        for (i = opaque_n + 1; i < scratch->n; i++) {
            GxPolygon tmp = scratch->poly[i];
            int j = i - 1;
            while (j >= opaque_n && rx_cmp_sortkey(&scratch->poly[j], &tmp) > 0) {
                scratch->poly[j + 1] = scratch->poly[j];
                j--;
            }
            scratch->poly[j + 1] = tmp;
        }
    }
}

/* Build the latched per-frame raster + post env (the same values both
 * backends render with).  texvram/texpal are the unit's memory snapshot. */
static void rx_build_env(const RxModel *m, GxRasterEnv *env, GxPostEnv *post,
                         GxTexMem *tex, const uint8_t *texvram,
                         uint32_t texvram_size, uint16_t *texpal,
                         uint32_t texpal_count)
{
    const RxEnvRegs *latched = &m->latched;
    tex->vram = texvram;
    tex->vram_size = texvram_size;
    tex->pal = texpal;
    tex->pal_count = texpal_count;

    memset(env, 0, sizeof(*env));
    env->tex = tex;
    env->toon = latched->toon_table;
    env->tex_enable = (latched->disp_cnt >> 0) & 1;
    env->highlight = (latched->disp_cnt >> 1) & 1;
    env->alpha_blend = (latched->disp_cnt >> 3) & 1;
    env->aa_enable = (int)((latched->disp_cnt >> 4) & 1);
    env->edge_enable = (int)((latched->disp_cnt >> 5) & 1);
    env->alpha_ref = (latched->disp_cnt & 4) ? (int)latched->alpha_ref_val : 0;

    /* post pass env: edge table + fog regs (melonDS latch,
     * GPU3D.cpp:2690-2701: FogOffset scaled *0x200, the 32-entry
     * fog table mapped into RenderFogDensityTable[34] as
     * [0]=reg[0], [1..32]=reg[0..31], [33]=reg[31]) */
    memset(post, 0, sizeof(*post));
    post->edge = latched->edge_table;
    post->clear_attr = latched->clear_attr1 & 0x3F008000;
    post->clear_depth = ((latched->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
    post->fog_color = latched->fog_color;
    post->fog_offset = latched->fog_offset * 0x200;
    post->fog_density[0] = latched->fog_density[0];
    memcpy(&post->fog_density[1], latched->fog_density, 32);
    post->fog_density[33] = latched->fog_density[31];
    post->fog_shift = (int)((latched->disp_cnt >> 8) & 0xF);
    post->edge_enable = (int)((latched->disp_cnt >> 5) & 1);
    post->fog_enable = (int)((latched->disp_cnt >> 7) & 1);
    post->fog_alpha_only = (int)((latched->disp_cnt >> 6) & 1);
    post->aa_enable = (int)((latched->disp_cnt >> 4) & 1);
}

#endif /* GX_REPLAY_COMMON_H */
