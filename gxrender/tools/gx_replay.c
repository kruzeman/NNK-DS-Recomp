/* gx_replay.c -- replay melonDS GX capture traces through the gxrender
 * software path and reproduce melonDS's frame model.
 *
 * Trace files (written by melonds-gxfifo-log.patch, see GX_TRACES.md):
 *   f%05u.gx      records: 01 <u32 word>                       GX FIFO word
 *                          02 <u8 width> <u32 addr> <u32 val>  3D MMIO write
 *                          03 <u8 cmd> <u32 param>             direct port
 *   f%05u.fb      melonDS 3D layer, 256*192 u32 (r|g<<8|b<<16|a<<24, 6-bit)
 *   f%05u.texvram 512 KiB flat texture VRAM
 *   f%05u.texpal  128 KiB flat texture palette RAM
 *
 * Frame model (mirrors melonDS GPU3D.cpp / GPU3D_Soft.cpp):
 *   - a "unit" is the GX traffic between two VBlank() calls
 *   - render env regs (DISP3DCNT/clear/toon/alpha-ref) are latched when a
 *     unit contains SWAP_BUFFERS (FlushRequest -> GPU3D::VBlank latch)
 *   - submitted polygons are retained; a flush with new polygons replaces
 *     the rendered set, a flush without polygons keeps the previous set
 *     (RenderPolygonRAM / RenderNumPolygons semantics)
 *   - textures are sampled from the unit's flat memory snapshot
 *   - polygon order at render: opaque polygons Y-sorted by SortKey
 *     ((ybot<<8)|ytop, melonDS SubmitPolygon), translucent after in
 *     submission order (SWAP sort mode bit 0 = sort opaque only)
 *
 * Output: OUT/f%05u.raw (same layout as the .fb dumps) and, with --ppm,
 * OUT/f%05u.ppm.  Only units that have a .fb dump are rendered unless
 * --all is given.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_fifo.h"
#include "gx_raster.h"
#include "gx_tex.h"

#define FB_W 256
#define FB_H 192
#define TEXVRAM_SIZE (512 * 1024)
#define TEXPAL_SIZE (128 * 1024)

/* melonDS GPU3D.cpp CmdNumParams (word-decode tracker needs it). */
static const uint8_t cmd_np[256] = {
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
} PolyList;

static PolyList pending;   /* submitted since the last flush */
static PolyList current;   /* set rendered every frame */
static PolyList scratch;   /* sort workspace */

static void list_push(PolyList *l, const GxPolygon *p)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->poly = realloc(l->poly, (size_t)l->cap * sizeof(GxPolygon));
        if (!l->poly) { fprintf(stderr, "gx_replay: out of memory\n"); exit(1); }
    }
    l->poly[l->n++] = *p;
}

static void retain_poly(void *user, const GxPolygon *poly)
{
    list_push((PolyList *)user, poly);
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
} EnvRegs;

/* Apply one MMIO write exactly like melonDS GPU3D::Write8/16/32. */
static void apply_reg(EnvRegs *e, uint32_t addr, int width, uint32_t val)
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
} Tracker;

static void tracker_entry(Tracker *t, uint8_t cmd, uint32_t param)
{
    if (cmd == 0x50) { t->saw_swap = 1; t->last_swap_param = param; }
    if (cmd >= 0x23 && cmd <= 0x28) t->saw_vtx = 1;
    t->n_entries++;
}

static void tracker_word(Tracker *t, uint32_t val)
{
    if (t->num == 0) {
        t->num = 4;
        t->cur = val;
        t->pc = 0;
        t->tp = cmd_np[val & 0xFF];
        if (t->tp > 0) return;
    } else {
        t->pc++;
    }
    for (;;) {
        if ((t->cur & 0xFF) || (t->num == 4 && t->cur == 0))
            tracker_entry(t, (uint8_t)(t->cur & 0xFF), val);
        if (t->pc >= t->tp) {
            t->cur >>= 8;
            t->num--;
            if (t->num == 0) break;
            t->pc = 0;
            t->tp = cmd_np[t->cur & 0xFF];
        }
        if (t->pc < t->tp) break;
    }
}

/* ---- frame clear (melonDS SoftRenderer3D::ClearBuffers, 1623-1710) ---- */

static void clear_frame(GxSurface *sf, const EnvRegs *live, const uint8_t *texvram)
{
    uint32_t clearz = ((live->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
    uint32_t polyid = live->clear_attr1 & 0x3F008000; /* clear polygon ID */

    if (live->disp_cnt & (1u << 14)) {
        /* clear-image mode: bitmap in texture VRAM at 40000h/60000h */
        uint8_t xoff = (live->clear_attr2 >> 16) & 0xFF;
        uint8_t yoff = (live->clear_attr2 >> 24) & 0xFF;
        int x, y;
        for (y = 0; y < FB_H; y++) {
            for (x = 0; x < FB_W; x++) {
                uint32_t off = (yoff << 9) + (xoff << 1);
                uint32_t val2 = texvram[0x40000 + off] | (texvram[0x40000 + off + 1] << 8);
                uint32_t val3 = texvram[0x60000 + off] | (texvram[0x60000 + off + 1] << 8);
                uint32_t r = (val2 << 1) & 0x3E; if (r) r++;
                uint32_t g = (val2 >> 4) & 0x3E; if (g) g++;
                uint32_t b = (val2 >> 9) & 0x3E; if (b) b++;
                uint32_t a = (val2 & 0x8000) ? 0x1F000000 : 0;
                sf->color[y * FB_W + x] = r | (g << 8) | (b << 16) | a;
                sf->depth[y * FB_W + x] = ((val3 & 0x7FFF) * 0x200) + 0x1FF;
                if (sf->attr)
                    sf->attr[y * FB_W + x] = polyid | (val3 & 0x8000);
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

static int poly_sortkey(const GxPolygon *p)
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

static int poly_translucent(const GxPolygon *p)
{
    uint32_t texfmt = (p->texparam >> 26) & 0x7;
    uint32_t alpha = (p->attr >> 16) & 0x1F;
    return (texfmt == 1 || texfmt == 6) || (alpha > 0 && alpha < 31);
}

static int cmp_sortkey(const void *a, const void *b)
{
    const GxPolygon *pa = a, *pb = b;
    return poly_sortkey(pa) - poly_sortkey(pb);
}

/* Build the render order: [opaque sorted by SortKey (stable)] [translucent
 * in submission order] -- mirrors GPU3D::VBlank's flush + YSort. */
static void build_render_order(int sort_opaque_only)
{
    int i;
    scratch.n = 0;
    for (i = 0; i < current.n; i++)
        if (!poly_translucent(&current.poly[i]))
            list_push(&scratch, &current.poly[i]);
    /* stable insertion sort by sortkey */
    for (i = 1; i < scratch.n; i++) {
        GxPolygon tmp = scratch.poly[i];
        int j = i - 1;
        while (j >= 0 && cmp_sortkey(&scratch.poly[j], &tmp) > 0) {
            scratch.poly[j + 1] = scratch.poly[j];
            j--;
        }
        scratch.poly[j + 1] = tmp;
    }
    int opaque_n = scratch.n;
    if (sort_opaque_only) {
        /* SWAP sort mode 1 (FlushAttributes bit 0): only the opaque
         * region is Y-sorted, translucents keep submission order
         * (GPU3D.cpp:2669-2671 sorts [0, NumOpaquePolygons)) */
        for (i = 0; i < current.n; i++)
            if (poly_translucent(&current.poly[i]))
                list_push(&scratch, &current.poly[i]);
    } else {
        /* SWAP sort mode 0: Y-sort the translucent region too.  melonDS
         * stable_sorts the whole list (GPU3D.cpp:2669-2671) but its
         * SortKey |= 0x10000 for translucent polygons
         * (GPU3D.cpp:1511-1512), so translucents always stay behind
         * every opaque polygon and are only ordered among themselves --
         * sorting the translucent region in place (bounded at opaque_n)
         * reproduces that exactly.  The previous loop started at
         * opaque_n+1 with an unbounded j: the first translucent polygon
         * was never inserted and later ones could bubble INTO the opaque
         * region whenever their (ybot<<8|ytop) key was lower than an
         * opaque polygon's key (no translucent bit in poly_sortkey). */
        for (i = 0; i < current.n; i++)
            if (poly_translucent(&current.poly[i]))
                list_push(&scratch, &current.poly[i]);
        for (i = opaque_n + 1; i < scratch.n; i++) {
            GxPolygon tmp = scratch.poly[i];
            int j = i - 1;
            while (j >= opaque_n && cmp_sortkey(&scratch.poly[j], &tmp) > 0) {
                scratch.poly[j + 1] = scratch.poly[j];
                j--;
            }
            scratch.poly[j + 1] = tmp;
        }
    }
}

/* ---- main ---- */

static uint8_t texvram[TEXVRAM_SIZE];
static uint16_t texpal[TEXPAL_SIZE / 2];
static uint32_t colorbuf[FB_W * FB_H];
static uint32_t depthbuf[FB_W * FB_H];
static uint32_t attrbuf[FB_W * FB_H];
static uint32_t colorbuf2[FB_W * FB_H]; /* AA second layer (persists across
                                         * frames like melonDS's buffers) */
static uint32_t depthbuf2[FB_W * FB_H];
static uint32_t attrbuf2[FB_W * FB_H];

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_dir = NULL;
    int want_ppm = 0, want_all = 0, quiet = 0, start_unit = 0;
    int render_lo[8], render_hi[8], n_render = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ppm")) want_ppm = 1;
        else if (!strcmp(argv[i], "--all")) want_all = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_unit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--render") && i + 1 < argc && n_render < 8) {
            const char *r = argv[++i];
            render_lo[n_render] = atoi(r);
            const char *dash = strchr(r, '-');
            render_hi[n_render] = dash ? atoi(dash + 1) : render_lo[n_render];
            n_render++;
        }
        else if (!trace_dir) trace_dir = argv[i];
        else if (!out_dir) out_dir = argv[i];
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (!trace_dir || !out_dir) {
        fprintf(stderr, "usage: gx_replay TRACE_DIR OUT_DIR [--start N] [--render A[-B]]... [--ppm] [--all] [--quiet]\n");
        return 2;
    }

    Gx gx;
    gx_init(&gx, colorbuf, depthbuf, FB_W, FB_H);
    gx.skip_software = 1;
    gx.draw_fn = retain_poly;
    gx.draw_user = &pending;

    EnvRegs live, latched;
    memset(&live, 0, sizeof(live));
    memset(&latched, 0, sizeof(latched));
    live.disp_cnt = latched.disp_cnt = 0x9;
    uint32_t latched_sort = 0; /* SWAP_BUFFERS param & 3 (FlushAttributes) */

    GxSurface sf = { .color = colorbuf, .depth = depthbuf, .attr = attrbuf,
                     .color2 = colorbuf2, .depth2 = depthbuf2, .attr2 = attrbuf2,
                     .w = FB_W, .h = FB_H };

    long units = 0, rendered = 0, total_polys = 0;
    for (int unit = start_unit;; unit++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/f%05d.gx", trace_dir, unit);
        FILE *f = fopen(path, "rb");
        if (!f) break;
        uint8_t *data = NULL;
        size_t len = 0, cap = 0;
        {
            uint8_t buf[65536];
            size_t got;
            while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
                if (len + got > cap) {
                    cap = cap ? cap * 2 : 1 << 16;
                    while (cap < len + got) cap *= 2;
                    data = realloc(data, cap);
                }
                memcpy(data + len, buf, got);
                len += got;
            }
        }
        fclose(f);

        Tracker tr;
        memset(&tr, 0, sizeof(tr));
        size_t off = 0;
        while (off < len) {
            uint8_t type = data[off++];
            if (type == 0x01 && off + 4 <= len) {
                uint32_t val;
                memcpy(&val, data + off, 4);
                off += 4;
                gx_fifo_write(&gx, val);
                tracker_word(&tr, val);
            } else if (type == 0x02 && off + 9 <= len) {
                uint8_t width = data[off];
                uint32_t addr, val;
                memcpy(&addr, data + off + 1, 4);
                memcpy(&val, data + off + 5, 4);
                off += 9;
                apply_reg(&live, addr, width, val);
                gx.zero_dot_w = live.zero_dot_w;
            } else if (type == 0x03 && off + 5 <= len) {
                uint8_t cmd = data[off];
                uint32_t val;
                memcpy(&val, data + off + 1, 4);
                off += 5;
                gx_cmd_entry(&gx, cmd, val);
                tracker_entry(&tr, cmd, val);
            } else {
                fprintf(stderr, "unit %d: bad record type %u at %zu\n", unit, type, off - 1);
                free(data);
                return 1;
            }
        }
        free(data);
        units++;

        if (tr.saw_swap) {
            latched = live;
            latched_sort = tr.last_swap_param & 3;
            if (pending.n > 0) {
                PolyList tmp = current;
                current = pending;
                pending = tmp;
                pending.n = 0;
            }
        }

        /* render output for units whose melonDS framebuffer was dumped */
        snprintf(path, sizeof(path), "%s/f%05d.fb", trace_dir, unit);
        int has_fb = 0;
        {
            FILE *fb = fopen(path, "rb");
            if (fb) { has_fb = 1; fclose(fb); }
        }
        if (want_all || has_fb) {
            if (n_render > 0) {
                int in_range = 0;
                for (i = 0; i < n_render; i++)
                    if (unit >= render_lo[i] && unit <= render_hi[i])
                        in_range = 1;
                if (!in_range)
                    goto skip_render;
            }
            snprintf(path, sizeof(path), "%s/f%05d.texvram", trace_dir, unit);
            FILE *tv = fopen(path, "rb");
            if (tv) {
                if (fread(texvram, 1, TEXVRAM_SIZE, tv) != TEXVRAM_SIZE)
                    fprintf(stderr, "unit %d: short texvram\n", unit);
                fclose(tv);
                snprintf(path, sizeof(path), "%s/f%05d.texpal", trace_dir, unit);
                FILE *tp = fopen(path, "rb");
                if (tp) {
                    if (fread(texpal, 1, TEXPAL_SIZE, tp) != TEXPAL_SIZE)
                        fprintf(stderr, "unit %d: short texpal\n", unit);
                    fclose(tp);
                }
            }

            GxTexMem tex = { texvram, TEXVRAM_SIZE, texpal, TEXPAL_SIZE / 2 };
            GxRasterEnv env;
            memset(&env, 0, sizeof(env));
            env.tex = &tex;
            env.toon = latched.toon_table;
            env.tex_enable = (latched.disp_cnt >> 0) & 1;
            env.highlight = (latched.disp_cnt >> 1) & 1;
            env.alpha_blend = (latched.disp_cnt >> 3) & 1;
            env.aa_enable = (int)((latched.disp_cnt >> 4) & 1);
            env.edge_enable = (int)((latched.disp_cnt >> 5) & 1);
            env.alpha_ref = (latched.disp_cnt & 4) ? (int)latched.alpha_ref_val : 0;

            /* post pass env: edge table + fog regs (melonDS latch,
             * GPU3D.cpp:2690-2701: FogOffset scaled *0x200, the 32-entry
             * fog table mapped into RenderFogDensityTable[34] as
             * [0]=reg[0], [1..32]=reg[0..31], [33]=reg[31]) */
            GxPostEnv post;
            memset(&post, 0, sizeof(post));
            post.edge = latched.edge_table;
            post.clear_attr = latched.clear_attr1 & 0x3F008000;
            post.clear_depth = ((latched.clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
            post.fog_color = latched.fog_color;
            post.fog_offset = latched.fog_offset * 0x200;
            post.fog_density[0] = latched.fog_density[0];
            memcpy(&post.fog_density[1], latched.fog_density, 32);
            post.fog_density[33] = latched.fog_density[31];
            post.fog_shift = (int)((latched.disp_cnt >> 8) & 0xF);
            post.edge_enable = (int)((latched.disp_cnt >> 5) & 1);
            post.fog_enable = (int)((latched.disp_cnt >> 7) & 1);
            post.fog_alpha_only = (int)((latched.disp_cnt >> 6) & 1);
            post.aa_enable = (int)((latched.disp_cnt >> 4) & 1);

            clear_frame(&sf, &latched, texvram);
            /* SWAP sort mode bit0=1: sort opaque only; bit0=0: Y-sort all
             * (translucent SortKeys carry 0x10000, GPU3D.cpp:2657-2671) */
            build_render_order((int)(latched_sort & 1));
            if (getenv("GX_POLY_DUMP")) {
                /* debug: dump the retained polygon set for this unit
                 * (env GX_POLY_DUMP=<prefix>, writes <prefix>.<unit>.txt) */
                char pdpath[1200];
                snprintf(pdpath, sizeof(pdpath), "%s.%d.txt",
                         getenv("GX_POLY_DUMP"), unit);
                FILE *pd = fopen(pdpath, "w");
                if (pd) {
                    for (i = 0; i < scratch.n; i++) {
                        const GxPolygon *p = &scratch.poly[i];
                        int j;
                        uint32_t a = p->attr;
                        fprintf(pd, "poly %d attr=%08x alpha=%u polyid=%u "
                                "shadow=%d mask=%d blendmode=%u nv=%d "
                                "texfmt=%u texparam=%08x\n",
                                i, a, (a >> 16) & 0x1F, (a >> 24) & 0x3F,
                                ((a & 0x30) == 0x30) && (((a >> 16) & 0x1F) != 0),
                                ((a & 0x3F000030) == 0x30),
                                (a >> 4) & 0x3, p->nv,
                                (p->texparam >> 26) & 0x7, p->texparam);
                        for (j = 0; j < p->nv; j++)
                            fprintf(pd, "  v%d sx=%d sy=%d depth=%08x w=%d\n",
                                    j, p->v[j].sx, p->v[j].sy,
                                    (uint32_t)p->v[j].depth, p->v[j].wnorm);
                    }
                    fclose(pd);
                }
            }
            for (i = 0; i < scratch.n; i++)
                gx_raster_polygon(&sf, &scratch.poly[i], &env);
            gx_post_pass(&sf, &post);

            snprintf(path, sizeof(path), "%s/f%05d.raw", out_dir, unit);
            FILE *o = fopen(path, "wb");
            if (!o) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            fwrite(colorbuf, 4, FB_W * FB_H, o);
            fclose(o);
            if (want_ppm) {
                snprintf(path, sizeof(path), "%s/f%05d.ppm", out_dir, unit);
                gx_write_ppm(path, &sf);
            }
            rendered++;
            total_polys += scratch.n;
            if (!quiet)
                printf("unit %d: entries=%d swap=%d vtx=%d set=%d rendered=%d\n",
                       unit, tr.n_entries, tr.saw_swap, tr.saw_vtx,
                       current.n, scratch.n);
        }
        skip_render:;
    }

    printf("gx_replay: units=%ld rendered=%ld polys_drawn=%ld "
           "unknown_cmds=%u retained=%d\n"
           "  polys_submitted=%u culled_front=%u culled_back=%u "
           "clip_dropped=%u degenerate=%u\n"
           "  latched env: disp_cnt=%08x clear_attr1=%08x clear_attr2=%08x "
           "alpha_ref=%u clearz=%u\n",
           units, rendered, total_polys, gx.n_unknown_cmds, current.n,
           gx.n_polys, gx.n_culled_front, gx.n_culled_back,
           gx.n_drop_clip, gx.n_drop_degenerate,
           latched.disp_cnt, latched.clear_attr1, latched.clear_attr2,
           latched.alpha_ref_val,
           (unsigned)(((latched.clear_attr2 & 0x7FFF) * 0x200) + 0x1FF));
    return 0;
}
