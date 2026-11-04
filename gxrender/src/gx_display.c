/* gx_display.c — runtime display & input bridge (see gx_display.h).
 *
 * Live display state = one GX2D state image (the validated gx_2d input
 * layout) patched by guest register/VRAM writes + the melonDS 3D frame
 * model (env latching at SWAP_BUFFERS, retained polygon set) shared with
 * tools/gx_replay.c.  At gxd_frame() the 3D layer is rendered (software
 * rasterizer or GL backend), both screens are composited through gx_2d,
 * and the result is presented (SDL2 window + GL, two letterboxed screens)
 * or kept for headless dumps.
 *
 * Register decode follows melonDS verbatim: GPU2D.cpp Write8/Write16/
 * Write32 (2D engine registers), GPU3D.cpp Write8/16/32 (3D registers),
 * GPU.cpp MapVRAM_A..I / WriteVRAM_* (VRAM bank routing), GPU::SetPowerCnt
 * (POWCNT1).  All state is byte-deterministic (integer pipeline).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "gx_display.h"
#include "gx_types.h"
#include "gx_fifo.h"
#include "gx_raster.h"
#include "gx_tex.h"
#include "gx_2d.h"
#include "gx_gl.h"

#define FB_W 256
#define FB_H 192
#define FB_PIXELS (FB_W * FB_H)
#define TEXVRAM_SIZE (512 * 1024)
#define TEXPAL_SIZE (128 * 1024)

/* ---- GX2D state image offsets (gx_2d.h layout) ---- */
#define IMG_ENG(n)     ((n) * 0x100)
#define IMG_GLOBAL     0x200
#define IMG_PAL        0x400
#define IMG_OAM        0xC00
#define IMG_ABG        0x1400     /* 512K */
#define IMG_BBG        0x81400    /* 128K */
#define IMG_AOBJ       0xA1400    /* 256K */
#define IMG_BOBJ       0xE1400    /* 128K */
#define IMG_ABGEXT     0x101400   /* 32K */
#define IMG_BBGEXT     0x109400   /* 32K */
#define IMG_AOBJEXT    0x111400   /* 8K */
#define IMG_BOBJEXT    0x113400   /* 8K */
#define IMG_RAW        0x115400   /* 512K VRAM_A..D raw */

/* engine register block field offsets */
#define E_DISPCNT   0x00
#define E_BGCNT     0x04   /* [4] u16 */
#define E_BGXPOS    0x0C   /* [4] u16 */
#define E_BGYPOS    0x14   /* [4] u16 */
#define E_BGXREF    0x1C   /* [2] s32 */
#define E_BGYREF    0x24   /* [2] s32 */
#define E_BGXRELOAD 0x2C   /* [2] s32 */
#define E_BGYRELOAD 0x34   /* [2] s32 */
#define E_BGROTA    0x3C   /* [2] s16 */
#define E_BGROTB    0x40
#define E_BGROTC    0x44
#define E_BGROTD    0x48
#define E_WIN0      0x4C   /* [4] u8: x1, x2, y1, y2 */
#define E_WIN1      0x50
#define E_WINCNT    0x54   /* [4] u8 */
#define E_BGMOSAIC  0x58   /* [2] u8 */
#define E_OBJMOSAIC 0x5A   /* [2] u8 */
#define E_BLDCNT    0x5C   /* u16 */
#define E_BLDALPHA  0x5E   /* u16 */
#define E_EVA       0x60
#define E_EVB       0x61
#define E_EVY       0x62
#define E_WIN0ACT   0x63
#define E_WIN1ACT   0x64

/* global block fields */
#define G_MASTERBRIGHT 0x00 /* u16 x2 */
#define G_SCREENSEN    0x04
#define G_SWAP         0x05
#define G_ENGA         0x06
#define G_ENGB         0x07
#define G_VRAMCNT      0x08 /* [9] */
#define G_VRAMSTAT     0x11
#define G_CAPEN        0x12
#define G_CAPCNT       0x14 /* u32 */
#define G_DISPFIFO     0x18 /* 512 bytes */

/* Phase-5BO diagnostics counter (static: must not change sizeof(Gxd), the
 * savestate layout fingerprint hashes it). */
static unsigned long dbg_fifo_words, dbg_gx_cmds, dbg_gx_swaps;

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint8_t clamp16(uint32_t v) { return (uint8_t)(v > 16 ? 16 : v); }

/* =====================================================================
 * 3D render env (verbatim model of tools/gx_replay.c / melonDS GPU3D)
 * ===================================================================== */

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
    uint32_t zero_dot_w;
} EnvRegs;

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

/* ---- GX FIFO word tracker (detects SWAP_BUFFERS; cf. gx_replay.c) ---- */

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

typedef struct {
    uint32_t cur;
    int num, pc, tp;
    int saw_swap;
    uint32_t last_swap_param;
} Tracker;

static void tracker_entry(Tracker *t, uint8_t cmd, uint32_t param)
{
    if (cmd == 0x50) { t->saw_swap = 1; t->last_swap_param = param; }
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

/* ---- retained polygon lists (melonDS RenderPolygonRAM model) ---- */

typedef struct {
    GxPolygon *poly;
    int n, cap;
} PolyList;

static void list_push(PolyList *l, const GxPolygon *p)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->poly = realloc(l->poly, (size_t)l->cap * sizeof(GxPolygon));
        if (!l->poly) { fprintf(stderr, "gx_display: out of memory\n"); exit(1); }
    }
    l->poly[l->n++] = *p;
}

static int poly_sortkey(const GxPolygon *p)
{
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
static void build_render_order(PolyList *current, PolyList *scratch,
                               int sort_opaque_only)
{
    int i;
    scratch->n = 0;
    for (i = 0; i < current->n; i++)
        if (!poly_translucent(&current->poly[i]))
            list_push(scratch, &current->poly[i]);
    for (i = 1; i < scratch->n; i++) {
        GxPolygon tmp = scratch->poly[i];
        int j = i - 1;
        while (j >= 0 && cmp_sortkey(&scratch->poly[j], &tmp) > 0) {
            scratch->poly[j + 1] = scratch->poly[j];
            j--;
        }
        scratch->poly[j + 1] = tmp;
    }
    int opaque_n = scratch->n;
    for (i = 0; i < current->n; i++)
        if (poly_translucent(&current->poly[i]))
            list_push(scratch, &current->poly[i]);
    if (!sort_opaque_only) {
        for (i = opaque_n + 1; i < scratch->n; i++) {
            GxPolygon tmp = scratch->poly[i];
            int j = i - 1;
            while (j >= opaque_n && cmp_sortkey(&scratch->poly[j], &tmp) > 0) {
                scratch->poly[j + 1] = scratch->poly[j];
                j--;
            }
            scratch->poly[j + 1] = tmp;
        }
    }
}

/* ---- frame clear (melonDS SoftRenderer3D::ClearBuffers) ---- */

static void clear_frame(GxSurface *sf, const EnvRegs *live,
                        const uint8_t *texvram)
{
    uint32_t clearz = ((live->clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
    uint32_t polyid = live->clear_attr1 & 0x3F008000;

    if (live->disp_cnt & (1u << 14)) {
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
        polyid |= (live->clear_attr1 & 0x8000);
        gx_surface_clear(sf, r | (g << 8) | (b << 16) | (a << 24), clearz,
                         polyid);
    }
}

/* =====================================================================
 * Gxd instance
 * ===================================================================== */

#define VRAM_BANKS 9
static const uint32_t vram_bank_size[VRAM_BANKS] = {
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000
};

struct Gxd {
    /* config */
    int windowed, gl_3d, scale, layout;
    int win_w, win_h;

    /* 3D */
    Gx gx;
    GxSurface sf;
    uint32_t colorbuf[FB_PIXELS], depthbuf[FB_PIXELS];
    uint32_t attrbuf[FB_PIXELS], colorbuf2[FB_PIXELS];
    uint32_t depthbuf2[FB_PIXELS], attrbuf2[FB_PIXELS];
    uint8_t rgba_tmp[FB_PIXELS * 4];
    PolyList pending, current, scratch;
    EnvRegs live, latched;
    uint32_t latched_sort;
    Tracker tracker;

    /* texture memory (VRAMFlat_Texture / VRAMFlat_TexPal layout) */
    uint8_t texvram[TEXVRAM_SIZE];
    uint16_t texpal[TEXPAL_SIZE / 2];

    /* 2D live state (GX2D image) + VRAM backing */
    uint8_t *img;
    uint8_t vram[VRAM_BANKS][0x20000];
    uint8_t vramcnt[VRAM_BANKS];
    uint32_t map_lcdc;
    uint32_t map_abg[32], map_bbg[8], map_aobj[16], map_bobj[8];
    uint32_t map_abgext[4], map_bbgext[4];
    uint32_t map_aobjext, map_bobjext;
    uint32_t map_tex[4], map_texpal[8];
    uint32_t map_arm7[2];
    int flat_dirty;
    int bulk_win;          /* 1: image came from gxd_load_2d_state and its
                            * window latch bytes are authoritative */
    uint32_t powcnt;
    uint8_t wramcnt;       /* WRAMCNT (0x04000247) - not a VRAM bank (Phase-5Y) */
    uint32_t vram_drop;    /* VRAM window stores with no bank mapped (Phase-5Y) */

    /* screens */
    uint32_t screen[2][FB_PIXELS];
    const uint32_t *fb3d_override;

    /* input */
    uint32_t keymask;      /* active-low KEYINPUT shape */
    uint16_t keycnt;
    int touch_down, touch_x, touch_y;

    /* window / present */
    SDL_Window *win;
    SDL_GLContext pctx;
    GxGl *gl3d;
    GLuint scr_tex[2];
    int have_tex;
    uint8_t *win_shot;     /* back-buffer capture of the last presented
                            * frame (GL_FRONT reads are unreliable) */
    int win_shot_w, win_shot_h;
    int quit;
    unsigned frames;
    uint32_t unhandled;
};

static uint8_t *eng(Gxd *g, int n) { return g->img + IMG_ENG(n); }
static uint8_t *glob(Gxd *g) { return g->img + IMG_GLOBAL; }

static int eng_enabled(Gxd *g, int n)
{
    return glob(g)[n ? G_ENGB : G_ENGA] & 1;
}

/* =====================================================================
 * VRAM bank mapping (melonDS GPU::MapVRAM_A..I) + flat rebuild
 * ===================================================================== */

static void maps_clear(Gxd *g)
{
    memset(g->map_abg, 0, sizeof(g->map_abg));
    memset(g->map_bbg, 0, sizeof(g->map_bbg));
    memset(g->map_aobj, 0, sizeof(g->map_aobj));
    memset(g->map_bobj, 0, sizeof(g->map_bobj));
    memset(g->map_abgext, 0, sizeof(g->map_abgext));
    memset(g->map_bbgext, 0, sizeof(g->map_bbgext));
    memset(g->map_tex, 0, sizeof(g->map_tex));
    memset(g->map_texpal, 0, sizeof(g->map_texpal));
    memset(g->map_arm7, 0, sizeof(g->map_arm7));
    g->map_aobjext = g->map_bobjext = 0;
    g->map_lcdc = 0;
}

static void map_range(uint32_t *map, int base, int n, uint32_t bit, int set)
{
    int i;
    for (i = 0; i < n; i++)
        if (set) map[base + i] |= bit;
        else     map[base + i] &= ~bit;
}

static void remap_all(Gxd *g)
{
    int b;
    maps_clear(g);
    for (b = 0; b < VRAM_BANKS; b++) {
        uint8_t cnt = g->vramcnt[b];
        uint32_t bit = 1u << b;
        int ofs;
        if (!(cnt & 0x80)) continue;
        switch (b) {
        case 0: case 1: /* 128K: LCDC / ABG / AOBJ / texture */
            ofs = (cnt >> 3) & 0x3;
            switch (cnt & 0x3) {
            case 0: g->map_lcdc |= bit; break;
            case 1: map_range(g->map_abg, ofs * 8, 8, bit, 1); break;
            case 2: map_range(g->map_aobj, (ofs & 1) * 8, 8, bit, 1); break;
            case 3: g->map_tex[ofs] |= bit; break;
            }
            break;
        case 2: case 3: /* 128K: LCDC / ABG / ARM7 / texture / BBG+BOBJ */
            ofs = (cnt >> 3) & 0x7;
            switch (cnt & 0x7) {
            case 0: g->map_lcdc |= bit; break;
            case 1: map_range(g->map_abg, ofs * 8, 8, bit, 1); break;
            case 2:
                g->map_arm7[ofs & 1] |= bit;
                glob(g)[G_VRAMSTAT] |= (uint8_t)(1 << (b - 2));
                break;
            case 3: g->map_tex[ofs & 3] |= bit; break;
            case 4:
                map_range(b == 2 ? g->map_bbg : g->map_bobj, 0, 8, bit, 1);
                break;
            }
            break;
        case 4: /* 64K: LCDC / ABG / AOBJ / tex pal / ABG ext pal */
            switch (cnt & 0x7) {
            case 0: g->map_lcdc |= bit; break;
            case 1: map_range(g->map_abg, 0, 4, bit, 1); break;
            case 2: map_range(g->map_aobj, 0, 4, bit, 1); break;
            case 3: map_range(g->map_texpal, 0, 4, bit, 1); break;
            case 4: map_range(g->map_abgext, 0, 4, bit, 1); break;
            }
            break;
        case 5: case 6: { /* 16K: LCDC / ABG / AOBJ / tex pal / ABG ext /
                           * AOBJ ext (F/G "second half at +32K" quirk) */
            int base = ((cnt >> 3) & 1) + (((cnt >> 3) & 2) << 1);
            switch (cnt & 0x7) {
            case 0: g->map_lcdc |= bit; break;
            case 1:
                g->map_abg[base] |= bit;
                g->map_abg[base + 2] |= bit;
                break;
            case 2:
                g->map_aobj[base] |= bit;
                g->map_aobj[base + 2] |= bit;
                break;
            case 3: g->map_texpal[base] |= bit; break;
            case 4:
                g->map_abgext[(base & 1) << 1] |= bit;
                g->map_abgext[((base & 1) << 1) + 1] |= bit;
                break;
            case 5: g->map_aobjext |= bit; break;
            }
            break;
        }
        case 7: /* 32K: LCDC / BBG (mirrored +32K) / BBG ext pal */
            switch (cnt & 0x3) {
            case 0: g->map_lcdc |= bit; break;
            case 1:
                g->map_bbg[0] |= bit; g->map_bbg[1] |= bit;
                g->map_bbg[4] |= bit; g->map_bbg[5] |= bit;
                break;
            case 2: map_range(g->map_bbgext, 0, 4, bit, 1); break;
            }
            break;
        case 8: /* 16K: LCDC / BBG (mirrored +32K) / BOBJ / BOBJ ext pal */
            switch (cnt & 0x3) {
            case 0: g->map_lcdc |= bit; break;
            case 1:
                g->map_bbg[2] |= bit; g->map_bbg[3] |= bit;
                g->map_bbg[6] |= bit; g->map_bbg[7] |= bit;
                break;
            case 2: map_range(g->map_bobj, 0, 8, bit, 1); break;
            case 3: g->map_bobjext |= bit; break;
            }
            break;
        }
    }
    g->flat_dirty = 1;
}

/* Rebuild one flat view from the raw banks through a block map (melonDS
 * ReadVRAM_* semantics: OR of all banks mapped at each block, bank offset
 * = flat address masked by the bank size). */
static void flat_rebuild(uint8_t *dst, uint32_t dst_size,
                         const uint32_t *map, int nblk, uint32_t blk,
                         Gxd *g)
{
    int b, bank, i;
    memset(dst, 0, dst_size);
    for (b = 0; b < nblk; b++) {
        uint32_t mask = map[b];
        if (!mask) continue;
        for (bank = 0; bank < VRAM_BANKS; bank++) {
            const uint8_t *src;
            uint8_t *d;
            if (!(mask & (1u << bank))) continue;
            src = g->vram[bank] + (((uint32_t)b * blk) & (vram_bank_size[bank] - 1));
            d = dst + (uint32_t)b * blk;
            for (i = 0; i < (int)blk; i++)
                d[i] |= src[i];
        }
    }
}

static void flat_rebuild1(uint8_t *dst, uint32_t dst_size, uint32_t mask,
                          Gxd *g)
{
    int bank, i;
    memset(dst, 0, dst_size);
    for (bank = 0; bank < VRAM_BANKS; bank++) {
        if (!(mask & (1u << bank))) continue;
        for (i = 0; i < (int)dst_size; i++)
            dst[i] |= g->vram[bank][i];
    }
}

static void rebuild_flats(Gxd *g)
{
    uint8_t *img = g->img;
    flat_rebuild(img + IMG_ABG, 512 * 1024, g->map_abg, 32, 0x4000, g);
    flat_rebuild(img + IMG_BBG, 128 * 1024, g->map_bbg, 8, 0x4000, g);
    flat_rebuild(img + IMG_AOBJ, 256 * 1024, g->map_aobj, 16, 0x4000, g);
    flat_rebuild(img + IMG_BOBJ, 128 * 1024, g->map_bobj, 8, 0x4000, g);
    flat_rebuild(img + IMG_ABGEXT, 32 * 1024, g->map_abgext, 4, 0x2000, g);
    flat_rebuild(img + IMG_BBGEXT, 32 * 1024, g->map_bbgext, 4, 0x2000, g);
    flat_rebuild1(img + IMG_AOBJEXT, 8 * 1024, g->map_aobjext, g);
    flat_rebuild1(img + IMG_BOBJEXT, 8 * 1024, g->map_bobjext, g);
    flat_rebuild(g->texvram, TEXVRAM_SIZE, g->map_tex, 4, 0x20000, g);
    flat_rebuild((uint8_t *)g->texpal, TEXPAL_SIZE, g->map_texpal, 8, 0x4000, g);
    memcpy(img + IMG_RAW, g->vram[0], 0x20000);
    memcpy(img + IMG_RAW + 0x20000, g->vram[1], 0x20000);
    memcpy(img + IMG_RAW + 0x40000, g->vram[2], 0x20000);
    memcpy(img + IMG_RAW + 0x60000, g->vram[3], 0x20000);
    g->flat_dirty = 0;
}

/* One byte into a VRAM window (0x06000000-0x0689FFFF), routed like
 * melonDS WriteVRAM_ABG/BBG/AOBJ/BOBJ/LCDC. */
static void vram_write_byte(Gxd *g, uint32_t a, uint8_t v)
{
    const uint32_t *map = NULL;
    int idx = 0, bank;
    uint32_t rel;

    if (a >= 0x06000000 && a < 0x06200000) {
        rel = a - 0x06000000;
        map = g->map_abg;
        idx = (int)((rel >> 14) & 0x1F);
    } else if (a >= 0x06200000 && a < 0x06220000) {
        rel = a - 0x06200000;
        map = g->map_bbg;
        idx = (int)((rel >> 14) & 0x7);
    } else if (a >= 0x06400000 && a < 0x06440000) {
        rel = a - 0x06400000;
        map = g->map_aobj;
        idx = (int)((rel >> 14) & 0xF);
    } else if (a >= 0x06600000 && a < 0x06620000) {
        rel = a - 0x06600000;
        map = g->map_bobj;
        idx = (int)((rel >> 14) & 0x7);
    } else if (a >= 0x06800000 && a < 0x068A0000) {
        /* LCDC window: fixed bank layout, unconditional raw write (this is
         * how banks mapped to texture get uploaded) */
        static const struct { uint32_t base, size; } lcdc[VRAM_BANKS] = {
            { 0x06800000, 0x20000 }, { 0x06820000, 0x20000 },
            { 0x06840000, 0x20000 }, { 0x06860000, 0x20000 },
            { 0x06880000, 0x10000 }, { 0x06890000, 0x4000 },
            { 0x06894000, 0x4000 }, { 0x06898000, 0x8000 },
            { 0x0689C000, 0x4000 },
        };
        for (bank = 0; bank < VRAM_BANKS; bank++)
            if (a >= lcdc[bank].base && a < lcdc[bank].base + lcdc[bank].size) {
                g->vram[bank][a - lcdc[bank].base] = v;
                g->flat_dirty = 1;
                return;
            }
        return;
    } else {
        return;
    }

    {
        int hit = 0;
        for (bank = 0; bank < VRAM_BANKS; bank++)
            if (map[idx] & (1u << bank)) {
                g->vram[bank][rel & (vram_bank_size[bank] - 1)] = v;
                g->flat_dirty = 1;
                hit = 1;
                if (getenv("NNK_VRAM_TRACE") && rel >= 0x200u && rel < 0x600u)
                    fprintf(stderr, "vramw a=0x%08X rel=0x%X -> bank %d\n", a, rel, bank);
            }
        if (!hit) {
            /* Phase-5Y: no bank mapped in this window block - the store is
             * dropped (melonDS behaves the same, but a drop is invisible unless
             * counted, and a dropped OBJ tile upload renders as missing
             * glyphs). */
            g->vram_drop++;
            if (getenv("NNK_VRAM_TRACE"))
                fprintf(stderr, "vramdrop a=0x%08X rel=0x%X mapblock=0x%X n=%u\n",
                        a, rel, map[idx], g->vram_drop);
        }
    }
}

/* =====================================================================
 * 2D engine register decode (melonDS GPU2D::Write8/16/32 -> image bytes)
 * ===================================================================== */

static void eng_mask_dispcnt(Gxd *g, int n)
{
    if (n) wr32(eng(g, n) + E_DISPCNT, rd32(eng(g, n) + E_DISPCNT) & 0xC0B1FFF7u);
}

static void eng_set_ref(Gxd *g, int n, int xy, int idx, uint32_t val)
{
    uint8_t *E = eng(g, n);
    uint8_t *ref = E + (xy ? E_BGYREF : E_BGXREF) + idx * 4;
    uint8_t *rel = E + (xy ? E_BGYRELOAD : E_BGXRELOAD) + idx * 4;
    wr32(ref, val);
    wr32(rel, val);
}

static void eng_write16(Gxd *g, int n, uint32_t off, uint32_t val)
{
    uint8_t *E = eng(g, n);

    switch (off) {
    case 0x000:
        wr32(E + E_DISPCNT, (rd32(E + E_DISPCNT) & 0xFFFF0000u) | (val & 0xFFFF));
        eng_mask_dispcnt(g, n);
        return;
    case 0x002:
        wr32(E + E_DISPCNT, (rd32(E + E_DISPCNT) & 0x0000FFFFu) | (val << 16));
        eng_mask_dispcnt(g, n);
        return;
    case 0x010:
        if (!n) { /* GPU3D::SetRenderXPos: display-capture alignment only */ }
        break;
    }
    if (!eng_enabled(g, n)) return;

    switch (off) {
    case 0x008: case 0x00A: case 0x00C: case 0x00E:
        wr16(E + E_BGCNT + (off - 0x008), val);
        return;
    case 0x010: case 0x012: case 0x014: case 0x016:
    case 0x018: case 0x01A: case 0x01C: case 0x01E: {
        int k = (int)((off - 0x010) >> 1);      /* 0..7: X,Y,X,Y,... */
        int idx = k >> 1;
        wr16(E + (k & 1 ? E_BGYPOS : E_BGXPOS) + idx * 2, val);
        return;
    }
    case 0x020: case 0x022: case 0x024: case 0x026:
    case 0x030: case 0x032: case 0x034: case 0x036: {
        int bg = (off >> 4) & 1;
        int field = (int)((off & 0xF) >> 1);    /* 0..3: PA..PD */
        wr16(E + E_BGROTA + field * 4 + bg * 2, val);
        return;
    }
    case 0x028: case 0x02C: case 0x038: case 0x03C: {  /* X/Y ref low half */
        int xy = (off == 0x02C || off == 0x03C);
        int idx = (off >= 0x038);
        uint8_t *ref = E + (xy ? E_BGYREF : E_BGXREF) + idx * 4;
        eng_set_ref(g, n, xy, idx, (rd32(ref) & 0xFFFF0000u) | (val & 0xFFFF));
        return;
    }
    case 0x02A: case 0x02E: case 0x03A: case 0x03E: {  /* ref high half */
        int xy = (off == 0x02E || off == 0x03E);
        int idx = (off >= 0x03A);
        uint8_t *ref = E + (xy ? E_BGYREF : E_BGXREF) + idx * 4;
        uint32_t v = val;
        if (v & 0x0800) v |= 0xF000;            /* 28-bit sign extension */
        eng_set_ref(g, n, xy, idx, (rd32(ref) & 0x0000FFFFu) | (v << 16));
        return;
    }
    case 0x040: case 0x042: {                   /* WIN0H / WIN1H */
        uint8_t *W = E + (off == 0x040 ? E_WIN0 : E_WIN1);
        W[1] = (uint8_t)(val & 0xFF);           /* x2 (right) */
        W[0] = (uint8_t)(val >> 8);             /* x1 (left) */
        return;
    }
    case 0x044: case 0x046: {                   /* WIN0V / WIN1V */
        uint8_t *W = E + (off == 0x044 ? E_WIN0 : E_WIN1);
        W[3] = (uint8_t)(val & 0xFF);           /* y2 (bottom) */
        W[2] = (uint8_t)(val >> 8);             /* y1 (top) */
        return;
    }
    case 0x048:
        E[E_WINCNT + 0] = (uint8_t)(val & 0xFF);
        E[E_WINCNT + 1] = (uint8_t)(val >> 8);
        return;
    case 0x04A:
        E[E_WINCNT + 2] = (uint8_t)(val & 0xFF);
        E[E_WINCNT + 3] = (uint8_t)(val >> 8);
        return;
    case 0x04C:
        E[E_BGMOSAIC + 0] = (uint8_t)(val & 0xF);
        E[E_BGMOSAIC + 1] = (uint8_t)((val >> 4) & 0xF);
        E[E_OBJMOSAIC + 0] = (uint8_t)((val >> 8) & 0xF);
        E[E_OBJMOSAIC + 1] = (uint8_t)(val >> 12);
        return;
    case 0x050:
        wr16(E + E_BLDCNT, val & 0x3FFF);
        return;
    case 0x052:
        wr16(E + E_BLDALPHA, val & 0x1F1F);
        E[E_EVA] = clamp16(val & 0x1F);
        E[E_EVB] = clamp16((val >> 8) & 0x1F);
        return;
    case 0x054:
        E[E_EVY] = clamp16(val & 0x1F);
        return;
    }
}

static void eng_write8(Gxd *g, int n, uint32_t off, uint32_t val)
{
    uint8_t *E = eng(g, n);
    uint8_t v = (uint8_t)val;

    switch (off) {
    case 0x000: case 0x001: case 0x002: case 0x003: {
        uint32_t d = rd32(E + E_DISPCNT);
        d = (d & ~(0xFFu << (off * 8))) | ((uint32_t)v << (off * 8));
        wr32(E + E_DISPCNT, d);
        eng_mask_dispcnt(g, n);
        return;
    }
    case 0x010:
        if (!n) { /* SetRenderXPos */ }
        break;
    }
    if (!eng_enabled(g, n)) return;

    switch (off) {
    case 0x008: case 0x009: case 0x00A: case 0x00B:
    case 0x00C: case 0x00D: case 0x00E: case 0x00F: {
        uint8_t *p = E + E_BGCNT + (off - 0x008);
        *p = v;
        return;
    }
    case 0x010: case 0x011: case 0x012: case 0x013:
    case 0x014: case 0x015: case 0x016: case 0x017:
    case 0x018: case 0x019: case 0x01A: case 0x01B:
    case 0x01C: case 0x01D: case 0x01E: case 0x01F: {
        int k = (int)((off - 0x010) >> 1);
        int idx = k >> 1;
        uint8_t *p = E + (k & 1 ? E_BGYPOS : E_BGXPOS) + idx * 2 + (off & 1);
        *p = v;
        return;
    }
    case 0x040: case 0x041: case 0x042: case 0x043: {
        uint8_t *W = E + (off < 0x042 ? E_WIN0 : E_WIN1);
        int hi = off & 1;
        W[hi ? 0 : 1] = v;                      /* high byte = x1, low = x2 */
        return;
    }
    case 0x044: case 0x045: case 0x046: case 0x047: {
        uint8_t *W = E + (off < 0x046 ? E_WIN0 : E_WIN1);
        int hi = off & 1;
        W[hi ? 2 : 3] = v;                      /* high byte = y1, low = y2 */
        return;
    }
    case 0x048: case 0x049: case 0x04A: case 0x04B:
        E[E_WINCNT + (off - 0x048)] = v;
        return;
    case 0x04C:
        E[E_BGMOSAIC + 0] = v & 0xF;
        E[E_BGMOSAIC + 1] = (uint8_t)(v >> 4);
        return;
    case 0x04D:
        E[E_OBJMOSAIC + 0] = v & 0xF;
        E[E_OBJMOSAIC + 1] = (uint8_t)(v >> 4);
        return;
    case 0x050:
        E[E_BLDCNT + 0] = v;
        return;
    case 0x051:
        E[E_BLDCNT + 1] = v;
        return;
    case 0x052:
        E[E_BLDALPHA + 0] = v & 0x1F;
        E[E_EVA] = clamp16(v & 0x1F);
        return;
    case 0x053:
        E[E_BLDALPHA + 1] = v & 0x1F;
        E[E_EVB] = clamp16(v & 0x1F);
        return;
    case 0x054:
        E[E_EVY] = clamp16(v & 0x1F);
        return;
    }
}

static void eng_write32(Gxd *g, int n, uint32_t off, uint32_t val)
{
    switch (off) {
    case 0x000:
        wr32(eng(g, n) + E_DISPCNT, val);
        eng_mask_dispcnt(g, n);
        return;
    case 0x028: case 0x02C: case 0x038: case 0x03C: {
        int xy = (off == 0x02C || off == 0x03C);
        int idx = (off >= 0x038);
        uint32_t v = val;
        if (v & 0x08000000u) v |= 0xF0000000u;  /* 28-bit sign extension */
        eng_set_ref(g, n, xy, idx, v);
        return;
    }
    }
    if (!eng_enabled(g, n)) return;
    eng_write16(g, n, off, val & 0xFFFF);
    eng_write16(g, n, off + 2, val >> 16);
}

/* =====================================================================
 * MMIO surface
 * ===================================================================== */

static void gxd_write_3d(Gxd *g, uint32_t addr, uint32_t val, int width)
{
    apply_reg(&g->live, addr, width, val);
    g->gx.zero_dot_w = g->live.zero_dot_w;
}

/* ---- Phase-5BQ: GXFIFO producer tag ------------------------------------
 * Phase-5BP proved the renderer faithful and the *operand words* wrong: the
 * guest submits MTX_MULT_4x3 (0x19, matrix mode 0) with twelve all-zero
 * parameter words once per object.  To decide whether the guest computes the
 * zeros, or the store/DMA path drops them, every FIFO word is stamped with
 * the guest pc that produced it (the runtime's `last_pc`, injected through
 * gxd_note_pc from the prelude's io_write32/16/8).  Statics only: the Gxd
 * struct (which the savestate layout hashes) is untouched.
 *
 *   NNK_GX_FIFOPC=n          print the first n FIFO words of a frame
 *   NNK_GX_FIFOPC_FRAME=f    ... only for frame f (unset: every frame)
 */
static uint32_t gx_prod_pc;
static uint32_t prod_ring_pc[64];
static uint32_t prod_ring_word[64];
static unsigned prod_ring_n;
static long prod_lim = -1;      /* NNK_GX_FIFOPC, -1 = not read yet */
static long prod_frame = -2;    /* NNK_GX_FIFOPC_FRAME, -2 = not read yet */
static unsigned prod_printed;

void gxd_note_pc(uint32_t pc) { gx_prod_pc = pc; }

void gxd_fifo_prod_dump(void)
{
    unsigned i, n = prod_ring_n < 64u ? prod_ring_n : 64u;
    for (i = 0; i < n; i++) {
        unsigned k = (prod_ring_n - n + i) & 63u;
        fprintf(stderr, "fiforing pc=%08x word=%08x\n",
                prod_ring_pc[k], prod_ring_word[k]);
    }
}

static void fifo_prod_note(const Gxd *g, uint32_t word)
{
    if (prod_lim < 0) {
        const char *e = getenv("NNK_GX_FIFOPC");
        const char *f = getenv("NNK_GX_FIFOPC_FRAME");
        prod_lim = (e && e[0]) ? (long)strtoul(e, NULL, 0) : 0;
        prod_frame = (f && f[0]) ? (long)strtoul(f, NULL, 0) : -1;
    }
    prod_ring_pc[prod_ring_n & 63u] = gx_prod_pc;
    prod_ring_word[prod_ring_n & 63u] = word;
    prod_ring_n++;
    if (prod_lim > 0 && (prod_frame < 0 || (long)g->frames == prod_frame) &&
        prod_printed < (unsigned)prod_lim) {
        prod_printed++;
        fprintf(stderr, "fifopc frame=%u #%u pc=%08x word=%08x\n",
                g->frames, prod_printed, gx_prod_pc, word);
    }
}

void gxd_gx_fifo_write(Gxd *g, uint32_t word)
{
    dbg_fifo_words++;
    fifo_prod_note(g, word);
    tracker_word(&g->tracker, word);
    gx_fifo_write(&g->gx, word);
}

void gxd_gx_cmd(Gxd *g, uint8_t cmd, uint32_t param)
{
    dbg_gx_cmds++;
    if (cmd == 0x50) dbg_gx_swaps++;
    tracker_entry(&g->tracker, cmd, param);
    gx_cmd_entry(&g->gx, cmd, param);
}

/* ---- Phase-5BL: GXSTAT (0x04000600) -------------------------------------
 * The guest polls this word before every GX submission
 * (0x0200985C..6C: `ldr r0,[0x04000600]; and r0,#0x07000000; lsr #24;
 * tst r0,#2; beq` — "wait until the command FIFO is less than half full").
 * Before this, the runtime had no READ path for the register: io_read32 fell
 * into `default: return 0`, so bit 25 never appeared and the guest spun
 * forever (Phase-5BK §3: 99.6 % of the post-skip trace, 795,481 hits at each
 * of the five loop PCs, and the instruction after the loop never executed).
 *
 * Composition follows melonDS GPU3D::Read32 case 0x04000600 exactly:
 *
 *   GXStat | (PosMatrixStackPointer & 0x1F) << 8
 *          | (ProjMatrixStackPointer & 0x1) << 13
 *          | fifolevel << 16
 *          | (fifolevel < 128) << 25      "FIFO less than half full"
 *          | (fifolevel == 0)  << 26      "FIFO empty"
 *
 * where GXStat holds only the CPU-owned bits (30-31 IRQ condition, 15 stack
 * error, 14/0 pending push-pop/test, 27 commands queued) and fifolevel comes
 * from THIS renderer's own FIFO (gx_fifo_level) — not a constant, so the
 * flags keep telling the truth after the first poll.
 *
 * Bits we cannot derive honestly: bit 1 (box-test found the box/position in
 * the volume) and the test result registers stay 0 because BOX_TEST/POS_TEST/
 * VEC_TEST are stubs in gx_fifo.c (no geometry test is performed).  Bit 15
 * (matrix stack error) is modelled only as far as the CPU's own clear-on-
 * write of bit 15; gx_fifo's push/pop does not flag overflow/underflow. */
static uint32_t gxstat_word(const Gxd *g)
{
    uint32_t lvl = gx_fifo_level(&g->gx);
    return g->gx.gxstat
         | ((g->gx.pos_sp & 0x1Fu) << 8)
         | ((g->gx.proj_sp & 0x1u) << 13)
         | ((lvl & 0xFFu) << 16)
         | (lvl < 128u ? (1u << 25) : 0u)
         | (lvl == 0u   ? (1u << 26) : 0u);
}

/* melonDS GPU3D::Write32/16/8 case 0x04000600: bit 15 clears the matrix-stack
 * error and resets the projection (and texture) stack pointer; bits 30-31
 * latch the GXFIFO IRQ condition; every other bit is read-only status. */
static void gxstat_write(Gxd *g, uint32_t off, uint32_t val, int width)
{
    uint32_t mask = (width == 4) ? 0xFFFFFFFFu : (width == 2 ? 0xFFFFu : 0xFFu);
    uint32_t w = (val & mask) << (off * 8);
    uint32_t m = mask << (off * 8);

    if (w & 0x8000u) {
        g->gx.gxstat &= ~0x8000u;
        g->gx.proj_sp = 0;
        g->gx.tex_sp = 0;      /* melonDS: TexMatrixStackPointer = 0 (CHECKME) */
    }
    g->gx.gxstat = (g->gx.gxstat & ~(m & 0xC0000000u)) | (w & m & 0xC0000000u);
}

/* melonDS GPU3D::CheckFIFOIRQ: GXStat bits 30-31 select when the GXFIFO IRQ
 * line is asserted — 1 = FIFO less than half full, 2 = FIFO empty, 0 = off.
 * The line is LEVEL-sensitive: it is re-evaluated on every GXFIFO write, on
 * every GXSTAT write and on every IF acknowledge (melonDS calls
 * CheckFIFOIRQ from exactly those three places), so the guest's submission
 * path can chain chunks: handler feeds a block, FIFO drains inside the write,
 * line re-asserts, handler runs again, ... and the handler disables IE when
 * its queue is empty. */
int gxd_gxfifo_irq(const Gxd *g)
{
    uint32_t lvl = gx_fifo_level(&g->gx);
    switch (g->gx.gxstat >> 30) {
    case 1: return lvl < 128u;
    case 2: return lvl == 0u;
    default: return 0;
    }
}

static unsigned long dbg_fifo_widths[4];
static void gfx_write(Gxd *g, uint32_t addr, uint32_t val, int width)
{
    if (addr >= 0x04000400 && addr < 0x04000440) {
        dbg_fifo_widths[width & 3]++;
        if (width != 4 && dbg_fifo_widths[width & 3] < 12)
            fprintf(stderr, "gxdbg: FIFO write width=%d addr=%08x val=%08x\n",
                    width, addr, val);
        gxd_gx_fifo_write(g, val);
        return;
    }
    if (addr >= 0x04000440 && addr < 0x040005CC) {
        dbg_fifo_widths[width & 3]++;
        if (width != 4 && dbg_fifo_widths[width & 3] < 12)
            fprintf(stderr, "gxdbg: CMD write width=%d addr=%08x val=%08x\n",
                    width, addr, val);
        gxd_gx_cmd(g, (uint8_t)((addr & 0x1FC) >> 2), val);
        return;
    }
    if (addr >= 0x04000600 && addr < 0x04000604) {
        gxstat_write(g, addr - 0x04000600u, val, width);
        return;
    }
    gxd_write_3d(g, addr, val, width);
}

static void global_write(Gxd *g, uint32_t addr, uint32_t val, int width)
{
    uint8_t *gl = glob(g);
    switch (addr) {
    case 0x04000304: {                          /* POWCNT1 */
        if (width == 1) {
            int sh = (addr & 3) * 8;
            g->powcnt = (g->powcnt & ~(0xFFu << sh)) | ((val & 0xFF) << sh);
        } else if (width == 2) {
            g->powcnt = (g->powcnt & 0xFFFF0000u) | (val & 0xFFFF);
        } else {
            g->powcnt = val;
        }
        gl[G_SCREENSEN] = (uint8_t)((g->powcnt >> 0) & 1);
        gl[G_SWAP]      = (uint8_t)((g->powcnt >> 15) & 1);
        gl[G_ENGA]      = (uint8_t)((g->powcnt >> 1) & 1);
        gl[G_ENGB]      = (uint8_t)((g->powcnt >> 9) & 1);
        return;
    }
    case 0x04000240: case 0x04000241: case 0x04000242: case 0x04000243:
    case 0x04000244: case 0x04000245: case 0x04000246: case 0x04000247:
    case 0x04000248: case 0x04000249: {
        /* Phase-5Y: the register layout is NOT a flat 0x240+bank table.
         * melonDS NDS.cpp ARM9Write8/16/32:  0x240..0x246 = VRAMCNT_A..G,
         * 0x247 = WRAMCNT (no VRAM bank!), 0x248 = VRAMCNT_H,
         * 0x249 = VRAMCNT_I.  The previous flat `a - 0x240` model put H at
         * 0x247 and I at 0x248, so the guest's WRAMCNT store (0x03) became
         * VRAMCNT_H, the real H store (0x82) landed on I and the real I store
         * (0x83) was dropped — which un-mapped the engine-B *BG extended
         * palette* bank and left the boss/dialog 2D layer with no colours. */
        static const int slot[10] = { 0, 1, 2, 3, 4, 5, 6, -1, 7, 8 };
        if (width == 1) {
            int b = slot[(addr - 0x04000240) & 0xF];
            if (b >= 0) g->vramcnt[b] = (uint8_t)(val & 0xFF);
            else        g->wramcnt = (uint8_t)(val & 0xFF);
        } else if (width == 2 && !(addr & 1) &&
                   addr >= 0x04000240 && addr <= 0x04000248) {
            int i;
            for (i = 0; i < 2; i++) {
                int b = slot[(addr - 0x04000240 + i) & 0xF];
                uint8_t v = (uint8_t)(val >> (8 * i));
                if (b >= 0) g->vramcnt[b] = v;
                else        g->wramcnt = v;
            }
        } else if (width == 4 &&
                   (addr == 0x04000240 || addr == 0x04000244 || addr == 0x04000248)) {
            int i;
            for (i = 0; i < 4; i++) {
                int b = slot[(addr - 0x04000240 + i) & 0xF];
                uint8_t v = (uint8_t)(val >> (8 * i));
                if (b >= 0) g->vramcnt[b] = v;
                else        g->wramcnt = v;
            }
        } else {
            int i;
            for (i = 0; i < width; i++) gxd_io_write(g, addr + (uint32_t)i,
                                                     (val >> (8 * i)) & 0xFF, 1);
            return;
        }
        if (getenv("NNK_VRC_TRACE"))
            fprintf(stderr, "vrc W%d 0x%08X 0x%08X -> %02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                    width, addr, val, g->vramcnt[0], g->vramcnt[1], g->vramcnt[2],
                    g->vramcnt[3], g->vramcnt[4], g->vramcnt[5], g->vramcnt[6],
                    g->vramcnt[7], g->vramcnt[8]);
        memcpy(gl + G_VRAMCNT, g->vramcnt, VRAM_BANKS);
        gl[G_VRAMSTAT] = 0;
        remap_all(g);
        return;
    }
    case 0x04000132:                            /* KEYCNT */
        if (width == 1) {
            if (addr & 1) g->keycnt = (uint16_t)((g->keycnt & 0x00FF) | (val << 8));
            else         g->keycnt = (uint16_t)((g->keycnt & 0xFF00) | (val & 0xFF));
        } else if (width == 2) {
            g->keycnt = (uint16_t)val;
        } else {
            g->keycnt = (uint16_t)val;
        }
        return;
    case 0x04000064:                            /* DISPCAPCNT */
        if (width == 2) {
            uint32_t c = rd32(gl + G_CAPCNT);
            if (addr == 0x04000064) c = (c & 0xFFFF0000u) | (val & 0xFFFF);
            else                    c = (c & 0x0000FFFFu) | (val << 16);
            wr32(gl + G_CAPCNT, c);
        } else {
            wr32(gl + G_CAPCNT, val);
        }
        gl[G_CAPEN] = (uint8_t)((rd32(gl + G_CAPCNT) >> 31) & 1);
        return;
    case 0x04000066:
        wr32(gl + G_CAPCNT, (rd32(gl + G_CAPCNT) & 0x0000FFFFu) | (val << 16));
        gl[G_CAPEN] = (uint8_t)((rd32(gl + G_CAPCNT) >> 31) & 1);
        return;
    case 0x0400006C:                            /* MASTER_BRIGHT A */
        wr16(gl + G_MASTERBRIGHT, val & 0xFFFF);
        return;
    case 0x0400106C:                            /* MASTER_BRIGHT B */
        wr16(gl + G_MASTERBRIGHT + 2, val & 0xFFFF);
        return;
    }
    g->unhandled++;
}

void gxd_io_write(Gxd *g, uint32_t addr, uint32_t val, int width)
{
    if (width < 1) width = 1;
    if (width > 4) width = 4;

    /* 2D engine register blocks.  NOTE: DISP3DCNT (0x04000060) lives INSIDE
     * the engine A block but is a 3D register (melonDS GPU3D::Write16); the
     * GPU-level registers (DISPCAPCNT, MASTER_BRIGHT) are not per-engine. */
    if (addr >= 0x04000000 && addr < 0x04000070) {
        uint32_t off = addr - 0x04000000;
        if (off == 0x60) {
            gxd_write_3d(g, addr, val, width);
            return;
        }
        if (off == 0x64 || off == 0x66 || off == 0x6C) {
            global_write(g, addr, val, width);
            return;
        }
        g->bulk_win = 0;
        if (width == 1) eng_write8(g, 0, off, val);
        else if (width == 2) eng_write16(g, 0, off, val & 0xFFFF);
        else {
            if (off + 4 <= 0x70) eng_write32(g, 0, off, val);
            else { eng_write16(g, 0, off, val & 0xFFFF); }
        }
        return;
    }
    if (addr >= 0x04001000 && addr < 0x04001070) {
        uint32_t off = addr - 0x04001000;
        if (off == 0x6C) {
            global_write(g, addr, val, width);
            return;
        }
        g->bulk_win = 0;
        if (width == 1) eng_write8(g, 1, off, val);
        else if (width == 2) eng_write16(g, 1, off, val & 0xFFFF);
        else {
            if (off + 4 <= 0x70) eng_write32(g, 1, off, val);
            else { eng_write16(g, 1, off, val & 0xFFFF); }
        }
        return;
    }
    /* palette RAM / OAM */
    if (addr >= 0x05000000 && addr < 0x05000800) {
        uint32_t off = addr - 0x05000000;
        uint32_t n = (uint32_t)width;
        if (off + n > 0x800) n = 0x800 - off;
        memcpy(g->img + IMG_PAL + off, &val, n); /* little-endian host */
        return;
    }
    if (addr >= 0x07000000 && addr < 0x07000800) {
        uint32_t off = addr - 0x07000000;
        uint32_t n = (uint32_t)width;
        if (off + n > 0x800) n = 0x800 - off;
        memcpy(g->img + IMG_OAM + off, &val, n);
        return;
    }
    /* VRAM windows */
    if (addr >= 0x06000000 && addr < 0x068A0000) {
        int i;
        for (i = 0; i < width; i++)
            vram_write_byte(g, addr + (uint32_t)i, (uint8_t)(val >> (8 * i)));
        return;
    }
    /* 3D / GX / global display regs */
    if ((addr >= 0x04000060 && addr < 0x04000070) ||
        (addr >= 0x04000304 && addr < 0x04000400) ||
        (addr >= 0x04000400 && addr < 0x04000620) ||
        addr == 0x04000240) {
        if (addr >= 0x04000304 && addr < 0x04000320) {
            global_write(g, addr, val, width);
            return;
        }
        if (addr >= 0x04000400 && addr < 0x04000620) {
            gfx_write(g, addr, val, width);
            return;
        }
        if (addr >= 0x04000320) {
            gxd_write_3d(g, addr, val, width);
            return;
        }
        /* 0x04000060 DISP3DCNT / 0x04000064 DISPCAPCNT handled below/above */
    }
    if (addr == 0x04000060) { gxd_write_3d(g, addr, val, width); return; }
    if (addr == 0x04000064 || addr == 0x04000066 ||
        addr == 0x0400006C || addr == 0x0400106C ||
        addr == 0x04000304 ||
        (addr >= 0x04000240 && addr <= 0x0400024E) ||
        addr == 0x04000132) {
        global_write(g, addr, val, width);
        return;
    }
    g->unhandled++;
}

uint32_t gxd_io_read(Gxd *g, uint32_t addr, int width)
{
    switch (addr) {
    case 0x04000130: {                          /* KEYINPUT */
        uint32_t ki = (g->keymask & 0x3FF) | (((g->keymask >> 10) & 3) << 16);
        if (width == 4) return ki;
        if (width == 1) return (addr & 1) ? (ki >> 8) & 0xFF : ki & 0xFF;
        return ki & 0xFFFF;
    }
    case 0x04000132:
        return g->keycnt;
    case 0x0400024E:
        return glob(g)[G_VRAMSTAT];
    case 0x04000600: case 0x04000601: case 0x04000602: case 0x04000603: {
        /* Phase-5BL: GXSTAT, byte-addressable like melonDS GPU3D::Read8/16/32
         * (0x04000600 low byte, 0x04000601 stack pointers, 0x04000602 FIFO
         * level / flags in the low byte, 0x04000603 bits 24-31 + level>>8). */
        uint32_t w = gxstat_word(g);
        unsigned off = addr - 0x04000600u;
        if (width == 4)   return w;
        if (width == 2)   return (w >> (off * 8)) & 0xFFFFu;
        return (w >> (off * 8)) & 0xFFu;
    }
    }
    g->unhandled++;
    return 0;
}

uint32_t gxd_unhandled_io(const Gxd *g) { return g->unhandled; }
unsigned gxd_frame_count(const Gxd *g) { return g->frames; }

/* =====================================================================
 * Input
 * ===================================================================== */

void gxd_set_keymask(Gxd *g, uint32_t active_low_mask)
{
    g->keymask = active_low_mask & 0xFFF;   /* 10 keys + lid/debug bits */
}

void gxd_key(Gxd *g, int keybit, int down)
{
    uint32_t bit = (keybit == GXD_KEY_LID) ? (1u << 10) : (1u << (keybit & 15));
    if (down) g->keymask &= ~bit;
    else      g->keymask |= bit;
    g->keymask &= 0xFFF;
}

uint32_t gxd_keymask(const Gxd *g) { return g->keymask; }

void gxd_touch(Gxd *g, int down, int x, int y)
{
    g->touch_down = down ? 1 : 0;
    g->touch_x = x < 0 ? 0 : (x > 255 ? 255 : x);
    g->touch_y = y < 0 ? 0 : (y > 191 ? 191 : y);
}

void gxd_get_touch(const Gxd *g, int *down, int *x, int *y)
{
    if (down) *down = g->touch_down;
    if (x) *x = g->touch_x;
    if (y) *y = g->touch_y;
}

/* =====================================================================
 * 3D layer render
 * ===================================================================== */

static void retain_poly(void *user, const GxPolygon *poly)
{
    list_push((PolyList *)user, poly);
}

static int gx_dbg_writer[256 * 192];
static unsigned gx_dbg_writer_col[256 * 192];

static void render_3d_sw(Gxd *g)
{
    GxRasterEnv env;
    GxPostEnv post;

    memset(&env, 0, sizeof(env));
    env.tex = &g->gx.texmem;
    env.toon = g->latched.toon_table;
    env.tex_enable = (g->latched.disp_cnt >> 0) & 1;
    env.highlight = (g->latched.disp_cnt >> 1) & 1;
    env.alpha_blend = (g->latched.disp_cnt >> 3) & 1;
    env.aa_enable = (int)((g->latched.disp_cnt >> 4) & 1);
    env.edge_enable = (int)((g->latched.disp_cnt >> 5) & 1);
    env.alpha_ref = (g->latched.disp_cnt & 4) ? (int)g->latched.alpha_ref_val : 0;

    memset(&post, 0, sizeof(post));
    post.edge = g->latched.edge_table;
    post.clear_attr = g->latched.clear_attr1 & 0x3F008000;
    post.clear_depth = ((g->latched.clear_attr2 & 0x7FFF) * 0x200) + 0x1FF;
    post.fog_color = g->latched.fog_color;
    post.fog_offset = g->latched.fog_offset * 0x200;
    post.fog_density[0] = g->latched.fog_density[0];
    memcpy(&post.fog_density[1], g->latched.fog_density, 32);
    post.fog_density[33] = g->latched.fog_density[31];
    post.fog_shift = (int)((g->latched.disp_cnt >> 8) & 0xF);
    post.edge_enable = (int)((g->latched.disp_cnt >> 5) & 1);
    post.fog_enable = (int)((g->latched.disp_cnt >> 7) & 1);
    post.fog_alpha_only = (int)((g->latched.disp_cnt >> 6) & 1);
    post.aa_enable = (int)((g->latched.disp_cnt >> 4) & 1);

    clear_frame(&g->sf, &g->latched, g->texvram);
    build_render_order(&g->current, &g->scratch, (int)(g->latched_sort & 1));
    {
        int i;
        unsigned long long px0 = gx_ras_pixels_total;
        unsigned zero_px = 0;
        const char *dbg = getenv("NNK_GX_RASTDBG");
        const char *polydbg = getenv("NNK_GX_POLYDBG");
        int polydump = 0;

        if (polydbg && polydbg[0]) {
            unsigned long want = strtoul(polydbg, NULL, 0);
            unsigned long lo = g->frames, hi = g->frames;
            const char *colon = strchr(polydbg, ':');
            if (colon) {
                lo = want;
                hi = strtoul(colon + 1, NULL, 0);
            } else hi = lo = want;
            if (g->frames >= lo && g->frames <= hi) {
                polydump = 1;
                fprintf(stderr, "polydbg f=%u n=%d begin\n", g->frames,
                        g->scratch.n);
            }
        }

        for (i = 0; i < g->scratch.n; i++) {
            const GxPolygon *p = &g->scratch.poly[i];
            unsigned long long px;
            gx_raster_polygon(&g->sf, p, &env);
            px = gx_ras_last_pixels;
            if (polydump) {
                size_t a;
                for (a = 0; a < (size_t)g->sf.w * g->sf.h; a++) {
                    if (g->sf.color[a] != gx_dbg_writer_col[a]) {
                        gx_dbg_writer_col[a] = g->sf.color[a];
                        gx_dbg_writer[a] = i;
                    }
                }
            }
            if (polydump) {
                int k;
                fprintf(stderr, "polydbg f=%u i=%d nv=%d facing=%d attr=%08x "
                                "tex=%08x wbuf=%d px=%llu", g->frames, i,
                        p->nv, p->facing_view, p->attr, p->texparam,
                        p->wbuffer, px);
                for (k = 0; k < p->nv && k < 8; k++)
                    fprintf(stderr, " v%d=(%d,%d|z=%d,w=%d|c=%d,%d,%d|st=%d,%d)",
                            k, p->v[k].sx, p->v[k].sy, p->v[k].depth,
                            p->v[k].wnorm, p->v[k].cr, p->v[k].cg, p->v[k].cb,
                            p->v[k].s, p->v[k].t);
                fprintf(stderr, "\n");
            }
            if (!dbg || !dbg[0] || px) continue;
            zero_px++;
            if (zero_px <= 6) {
                int k;
                fprintf(stderr, "rastdbg f=%u 3dslot=%d/%d submitted to "
                                "fill_ras_vertex but wrote 0 pixels: attr=%08x "
                                "nv=%d wbuf=%d facing=%d",
                        g->frames, i + 1, g->scratch.n, p->attr, p->nv,
                        p->wbuffer, p->facing_view);
                for (k = 0; k < p->nv && k < 6; k++)
                    fprintf(stderr, " v%d=(%d,%d|z=%d,w=%d,c=%d,%d,%d)", k,
                            p->v[k].sx, p->v[k].sy, p->v[k].depth,
                            p->v[k].wnorm, p->v[k].cr, p->v[k].cg, p->v[k].cb);
                fprintf(stderr, "\n");
            }
        }
        if (dbg && dbg[0])
            fprintf(stderr, "rastdbg f=%u polys=%d zero_px=%u pixels=%llu "
                            "fb3d_nonblack=%s\n",
                    g->frames, g->scratch.n, zero_px,
                    gx_ras_pixels_total - px0,
                    (gx_ras_pixels_total - px0) ? "yes" : "no");
    }
    if (getenv("NNK_GX_POLYDBG") && getenv("NNK_GX_POLYDBG")[0]) {
        const char *polydbg = getenv("NNK_GX_POLYDBG");
        unsigned long want = strtoul(polydbg, NULL, 0);
        unsigned long lo = want, hi = want;
        const char *colon = strchr(polydbg, ':');
        if (colon) hi = strtoul(colon + 1, NULL, 0);
        if (g->frames >= lo && g->frames <= hi) {
            static const int xs[] = { 0, 2, 5, 10, 20, 40, 60, 80, 100, 150, 200, 250 };
            int y, k;
            for (y = 16; y <= 104; y++) {
                fprintf(stderr, "wmap f=%u y=%3d", g->frames, y);
                for (k = 0; k < (int)(sizeof xs / sizeof xs[0]); k++) {
                    size_t a = (size_t)y * g->sf.w + xs[k];
                    fprintf(stderr, " x%d=%d/%06x", xs[k], gx_dbg_writer[a],
                            g->sf.color[a] & 0xFFFFFF);
                }
                fprintf(stderr, "\n");
            }
        }
    }
    gx_post_pass(&g->sf, &post);
}

static void render_3d_gl(Gxd *g)
{
    uint32_t ca = g->latched.clear_attr1;
    uint32_t r = (ca << 1) & 0x3E; if (r) r++;
    uint32_t gg = (ca >> 4) & 0x3E; if (gg) gg++;
    uint32_t b = (ca >> 9) & 0x3E; if (b) b++;
    float clear_alpha = ((ca >> 16) & 0x1F) / 31.0f;
    int i;

    gx_gl_begin_frame_a(g->gl3d, r | (gg << 8) | (b << 16), clear_alpha);
    build_render_order(&g->current, &g->scratch, (int)(g->latched_sort & 1));
    for (i = 0; i < g->scratch.n; i++)
        gx_gl_draw_polygon(g->gl3d, &g->scratch.poly[i], &g->gx.texmem);
    gx_gl_read_rgba(g->gl3d, g->rgba_tmp);
    for (i = 0; i < FB_PIXELS; i++) {
        const uint8_t *p = &g->rgba_tmp[i * 4];
        g->colorbuf[i] = (uint32_t)(p[0] >> 2) |
                         ((uint32_t)(p[1] >> 2) << 8) |
                         ((uint32_t)(p[2] >> 2) << 16) |
                         ((uint32_t)(p[3] >> 3) << 24);
    }
}

/* =====================================================================
 * Window present (SDL2 + GL: two letterboxed screens)
 * ===================================================================== */

static void scr_rects(const Gxd *g, int rect[2][4])
{
    int nat_w = (g->layout == GXD_LAYOUT_SIDE) ? 512 : 256;
    int nat_h = (g->layout == GXD_LAYOUT_SIDE) ? 192 : 384;
    int ww = g->win_w, wh = g->win_h;
    int s_num = ww * 1024 / nat_w, s_den = wh * 1024 / nat_h;
    int s = s_num < s_den ? s_num : s_den;      /* fixed 10.0 scale */
    int cw = (nat_w * s) / 1024, ch = (nat_h * s) / 1024;
    int ox = (ww - cw) / 2, oy = (wh - ch) / 2;
    int sw = (256 * s) / 1024, sh = (192 * s) / 1024;

    rect[0][0] = ox;
    rect[0][1] = oy;
    rect[0][2] = sw;
    rect[0][3] = sh;
    if (g->layout == GXD_LAYOUT_SIDE) {
        rect[1][0] = ox + sw;
        rect[1][1] = oy;
    } else {
        rect[1][0] = ox;
        rect[1][1] = oy + sh;
    }
    rect[1][2] = sw;
    rect[1][3] = sh;
}

static void upload_screen(Gxd *g, int which)
{
    /* screens are melonDS ExpandColor output: 32-bit BGRA */
    glBindTexture(GL_TEXTURE_2D, g->scr_tex[which]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FB_W, FB_H,
                    GL_BGRA, GL_UNSIGNED_BYTE, g->screen[which]);
}

static void present(Gxd *g)
{
    int rect[2][4];
    int i;

    SDL_GL_MakeCurrent(g->win, g->pctx);
    scr_rects(g, rect);

    glViewport(0, 0, g->win_w, g->win_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, g->win_w, g->win_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    for (i = 0; i < 2; i++) {
        float x0 = (float)rect[i][0], y0 = (float)rect[i][1];
        float x1 = x0 + rect[i][2], y1 = y0 + rect[i][3];
        upload_screen(g, i);
        glBindTexture(GL_TEXTURE_2D, g->scr_tex[i]);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(x0, y0);
        glTexCoord2f(1, 0); glVertex2f(x1, y0);
        glTexCoord2f(1, 1); glVertex2f(x1, y1);
        glTexCoord2f(0, 1); glVertex2f(x0, y1);
        glEnd();
    }

    /* capture the composed window image from the BACK buffer before the
     * swap (glReadBuffer(GL_FRONT) after swap is unreliable) */
    {
        size_t n = (size_t)g->win_w * g->win_h * 4;
        uint8_t *cap = (uint8_t *)realloc(g->win_shot, n);
        if (cap) {
            g->win_shot = cap;
            g->win_shot_w = g->win_w;
            g->win_shot_h = g->win_h;
            glReadPixels(0, 0, g->win_w, g->win_h, GL_RGBA, GL_UNSIGNED_BYTE,
                         g->win_shot);
        }
    }
    SDL_GL_SwapWindow(g->win);
}

static const struct { SDL_Scancode sc; int key; } keymap[] = {
    { SDL_SCANCODE_X, GXD_KEY_A },
    { SDL_SCANCODE_Z, GXD_KEY_B },
    { SDL_SCANCODE_RSHIFT, GXD_KEY_SELECT },
    { SDL_SCANCODE_RETURN, GXD_KEY_START },
    { SDL_SCANCODE_RIGHT, GXD_KEY_RIGHT },
    { SDL_SCANCODE_LEFT, GXD_KEY_LEFT },
    { SDL_SCANCODE_UP, GXD_KEY_UP },
    { SDL_SCANCODE_DOWN, GXD_KEY_DOWN },
    { SDL_SCANCODE_S, GXD_KEY_R },
    { SDL_SCANCODE_A, GXD_KEY_L },
};

static void touch_from_window(Gxd *g, int mx, int my, int down)
{
    int rect[2][4];
    scr_rects(g, rect);
    if (mx >= rect[1][0] && mx < rect[1][0] + rect[1][2] &&
        my >= rect[1][1] && my < rect[1][1] + rect[1][3] && rect[1][2] > 0) {
        int x = (mx - rect[1][0]) * FB_W / rect[1][2];
        int y = (my - rect[1][1]) * FB_H / rect[1][3];
        gxd_touch(g, down, x, y);
    } else if (!down) {
        g->touch_down = 0;
    }
}

static void pump_events(Gxd *g)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            g->quit = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            size_t k;
            int down = (ev.type == SDL_KEYDOWN);
            for (k = 0; k < sizeof(keymap) / sizeof(keymap[0]); k++)
                if (keymap[k].sc == ev.key.keysym.scancode)
                    gxd_key(g, keymap[k].key, down);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT)
                touch_from_window(g, ev.button.x, ev.button.y,
                                  ev.type == SDL_MOUSEBUTTONDOWN);
            break;
        case SDL_MOUSEMOTION:
            if (ev.motion.state & SDL_BUTTON_LMASK)
                touch_from_window(g, ev.motion.x, ev.motion.y, 1);
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                SDL_GL_GetDrawableSize(g->win, &g->win_w, &g->win_h);
            }
            break;
        }
    }
}

/* =====================================================================
 * Open / close / frame
 * ===================================================================== */

Gxd *gxd_open(const GxdConfig *cfg)
{
    static const GxdConfig def = { 0, 0, 1, GXD_LAYOUT_SIDE, 0, 0, NULL };
    Gxd *g;

    if (!cfg) cfg = &def;
    g = (Gxd *)calloc(1, sizeof(Gxd));
    if (!g) return NULL;

    g->windowed = cfg->windowed;
    g->gl_3d = cfg->gl_3d;
    g->scale = cfg->scale > 0 ? cfg->scale : 1;
    g->layout = cfg->layout;

    g->img = (uint8_t *)calloc(1, GX2D_STATE_SIZE);
    if (!g->img) { free(g); return NULL; }

    gx_init(&g->gx, g->colorbuf, g->depthbuf, FB_W, FB_H);
    g->sf.color = g->colorbuf;
    g->sf.depth = g->depthbuf;
    g->sf.attr = g->attrbuf;
    g->sf.color2 = g->colorbuf2;
    g->sf.depth2 = g->depthbuf2;
    g->sf.attr2 = g->attrbuf2;
    g->sf.w = FB_W;
    g->sf.h = FB_H;
    g->gx.skip_software = 1;
    g->gx.draw_fn = retain_poly;
    g->gx.draw_user = &g->pending;
    gx_set_texture_mem(&g->gx, g->texvram, TEXVRAM_SIZE, g->texpal,
                       TEXPAL_SIZE / 2);

    g->live.disp_cnt = g->latched.disp_cnt = 0x9;
    g->keymask = 0x3FF;                     /* no keys pressed (active-low) */
    g->powcnt = 0;
    glob(g)[G_VRAMSTAT] = 0;

    if (g->windowed) {
        int nat_w = (g->layout == GXD_LAYOUT_SIDE) ? 512 : 256;
        int nat_h = (g->layout == GXD_LAYOUT_SIDE) ? 192 : 384;
        int ww = cfg->win_w > 0 ? cfg->win_w : nat_w * g->scale;
        int wh = cfg->win_h > 0 ? cfg->win_h : nat_h * g->scale;

        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "gx_display: SDL_Init: %s\n", SDL_GetError());
            free(g->img);
            free(g);
            return NULL;
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        g->win = SDL_CreateWindow(cfg->title ? cfg->title : "gxrender",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  ww, wh,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                                  SDL_WINDOW_RESIZABLE);
        if (!g->win) {
            fprintf(stderr, "gx_display: SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            free(g->img);
            free(g);
            return NULL;
        }
        g->pctx = SDL_GL_CreateContext(g->win);
        if (!g->pctx) {
            fprintf(stderr, "gx_display: SDL_GL_CreateContext: %s\n",
                    SDL_GetError());
            SDL_DestroyWindow(g->win);
            SDL_Quit();
            free(g->img);
            free(g);
            return NULL;
        }
        SDL_GL_MakeCurrent(g->win, g->pctx);
        SDL_GL_GetDrawableSize(g->win, &g->win_w, &g->win_h);

        glGenTextures(2, g->scr_tex);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, g->scr_tex[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FB_W, FB_H, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
        }
        g->have_tex = 1;

        if (g->gl_3d) {
            g->gl3d = gx_gl_open_external(g->win, g->pctx);
            if (!g->gl3d)
                fprintf(stderr, "gx_display: GL 3D backend unavailable, "
                                "falling back to software 3D\n");
        }
    }
    return g;
}

void gxd_close(Gxd *g)
{
    if (!g) return;
    if (g->gl3d) gx_gl_close(g->gl3d);
    if (g->have_tex) glDeleteTextures(2, g->scr_tex);
    if (g->pctx) SDL_GL_DeleteContext(g->pctx);
    if (g->win) SDL_DestroyWindow(g->win);
    if (g->windowed) SDL_Quit();
    free(g->win_shot);
    free(g->pending.poly);
    free(g->current.poly);
    free(g->scratch.poly);
    free(g->img);
    free(g);
}

/* ---- Phase-5Y: full 2D state dump (differential analysis) ----------------
 * NNK_GX_STATE_DUMP=<path> NNK_GX_STATE_DUMP_FRAME=<n> writes, at the start of
 * presentation frame <n> (after the flat views are rebuilt and the window
 * latch is applied, i.e. exactly the state gx2d_render_frame is about to
 * consume), a raw image:
 *   +0x000000  0x195400  the GX2D state image (g->img: engine regs A/B,
 *                        global block, palette, OAM, VRAMFlat_* views)
 *   +0x195400  0x120000  the nine raw VRAM banks, 0x20000 bytes each
 * so the exact bytes the renderer saw can be decoded offline (regs, tile
 * data, palette, OAM) without another rebuild.  One file per dump; the
 * dump is written once and then disabled. */
static void state_dump(Gxd *g)
{
    static int done;
    const char *p, *f;
    char path[512];
    FILE *fp;
    int b;
    if (done) return;
    p = getenv("NNK_GX_STATE_DUMP");
    f = getenv("NNK_GX_STATE_DUMP_FRAME");
    if (!p || !p[0]) return;
    if (f && f[0] && (unsigned)strtoul(f, NULL, 0) != g->frames) return;
    done = 1;
    snprintf(path, sizeof path, "%s", p);
    fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "gx: state dump: cannot write %s\n", path); return; }
    fwrite(g->img, 1, IMG_RAW + 0x80000u, fp);
    for (b = 0; b < VRAM_BANKS; b++) fwrite(g->vram[b], 1, 0x20000, fp);
    fclose(fp);
    fprintf(stderr, "gx: state dump frame=%u -> %s (powcnt=%08x vramcnt=%02x%02x%02x%02x%02x%02x%02x%02x%02x)\n",
            g->frames, path, g->powcnt, g->vramcnt[0], g->vramcnt[1], g->vramcnt[2],
            g->vramcnt[3], g->vramcnt[4], g->vramcnt[5], g->vramcnt[6],
            g->vramcnt[7], g->vramcnt[8]);
}

/* ---- Phase-5BO debug: 3D layer dump ------------------------------------
 * NNK_GX_FB3D=<path> NNK_GX_FB3D_FRAME=<n>: at presentation frame <n>, after
 * the 3D render and before the 2D compositor consumes it, write the 3D
 * color buffer as a PPM plus the latched 3D env and the FIFO/raster stats.
 * This is the only way to tell "the 3D layer is black" from "the 2D layer
 * covering it is empty": the composed screens mix both. */
static void dbg_fb3d(const Gxd *g)
{
    static int done;
    const char *p = getenv("NNK_GX_FB3D");
    const char *f = getenv("NNK_GX_FB3D_FRAME");
    FILE *fp;
    int i;
    if (done || !p || !p[0]) return;
    if (f && f[0] && (unsigned)strtoul(f, NULL, 0) != g->frames) return;
    done = 1;
    fp = fopen(p, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", FB_W, FB_H);
        for (i = 0; i < FB_PIXELS; i++) {
            uint32_t c = g->colorbuf[i];
            uint8_t rgb[3] = {
                (uint8_t)((c & 0x3F) << 2),
                (uint8_t)(((c >> 8) & 0x3F) << 2),
                (uint8_t)(((c >> 16) & 0x3F) << 2)
            };
            fwrite(rgb, 1, 3, fp);
        }
        fclose(fp);
    }
    fprintf(stderr,
            "gx: fb3d frame=%u -> %s saw_swap=%d polys current=%d pending=%d "
            "submitted=%u drawn=%u verts=%u dropped(clip=%u degen=%u cullf=%u cullb=%u) "
            "unknown_cmds=%u disp3dcnt=%08x clear=%08x/%08x "
            "fogcol=%08x fogofs=%d alpharef=%d zerodot=%08x\n",
            g->frames, p, g->tracker.saw_swap, g->current.n, g->pending.n,
            g->gx.n_polys, g->gx.n_polys_drawn, g->gx.n_verts,
            g->gx.n_drop_clip, g->gx.n_drop_degenerate,
            g->gx.n_culled_front, g->gx.n_culled_back, g->gx.n_unknown_cmds,
            g->latched.disp_cnt, g->latched.clear_attr1, g->latched.clear_attr2,
            g->latched.fog_color, (int)g->latched.fog_offset,
            (int)g->latched.alpha_ref_val, g->latched.zero_dot_w);
    fprintf(stderr,
            "gx: fifo-write widths w1=%lu w2=%lu w4=%lu zeromult_skipped=%u\n",
            dbg_fifo_widths[1], dbg_fifo_widths[2], dbg_fifo_widths[0],
            gx_dbg_zero_mult());
    {
        int c, first = 1;
        fprintf(stderr, "gx: unknown-cmd histogram:");
        for (c = 0; c < 256; c++) {
            int n = gx_dbg_unknown_hist((unsigned)c);
            if (!n) continue;
            fprintf(stderr, "%s %02x=%d", first ? "" : ",", c, n);
            first = 0;
        }
        fprintf(stderr, "\n");
    }
}

/* Phase-5BO: per-frame GX submission log (diagnostic, static counters only --
 * nothing here may change sizeof(Gxd): the savestate layout fingerprint hashes
 * it and the operator's state must keep loading). */
static void dbg_gxlog(const Gxd *g)
{
    static long prev_frame = -1;
    static unsigned long pw, pc, ps, pp, pd, pv, pcl, pdeg, pcf, pcb;
    const char *e = getenv("NNK_GX_GXLOG");
    long from = 0, to = 0;
    if (!e || !e[0]) return;
    if (sscanf(e, "%ld:%ld", &from, &to) != 2) return;
    if ((long)g->frames < from || (long)g->frames > to) return;
    if (!pw && prev_frame < 0) { pw = pc = ps = pp = pd = pv = 0;
                                 pcl = pdeg = pcf = pcb = 0; }
    fprintf(stderr, "gxlog f=%u words=%lu cmds=%lu swaps=%lu pending=%d current=%d "
                    "polys=%lu drawn=%lu verts=%lu dropclip=%lu dropdegen=%lu "
                    "cullf=%lu cullb=%lu\n",
            g->frames, dbg_fifo_words - pw, dbg_gx_cmds - pc, dbg_gx_swaps - ps,
            g->pending.n, g->current.n,
            (unsigned long)g->gx.n_polys - pp,
            (unsigned long)g->gx.n_polys_drawn - pd,
            (unsigned long)g->gx.n_verts - pv,
            (unsigned long)(g->gx.n_drop_clip - pcl),
            (unsigned long)(g->gx.n_drop_degenerate - pdeg),
            (unsigned long)(g->gx.n_culled_front - pcf),
            (unsigned long)(g->gx.n_culled_back - pcb));
    prev_frame = (long)g->frames;
    pw = dbg_fifo_words; pc = dbg_gx_cmds; ps = dbg_gx_swaps;
    pp = g->gx.n_polys; pd = g->gx.n_polys_drawn; pv = g->gx.n_verts;
    pcl = g->gx.n_drop_clip; pdeg = g->gx.n_drop_degenerate;
    pcf = g->gx.n_culled_front; pcb = g->gx.n_culled_back;
}

/* Phase-5BO: per-frame resolved 2D layer dump.  NNK_GX_2DINFO=<frame>[,<frame>]
 * prints the engine/BG model (see gx2d_diag_dump) at the listed presented
 * frames; "NNK_GX_2DINFO=3350" is the operator-save frame. */
static void dbg_2dinfo(Gxd *g, const Gx2DState *st)
{
    static int parsed;
    static long frames[8];
    static int nframes;
    const char *e;
    int i;
    if (!parsed)
    {
        parsed = 1;
        e = getenv("NNK_GX_2DINFO");
        if (e && e[0])
        {
            const char *p = e;
            while (*p && nframes < 8)
            {
                frames[nframes++] = strtol(p, (char **)&p, 0);
                if (*p == ',') p++;
            }
        }
    }
    for (i = 0; i < nframes; i++)
        if ((long)g->frames == frames[i])
        {
            gx2d_diag_dump(st, g->frames);
            return;
        }
}

static void latch_unit(Gxd *g)
{
    /* SWAP_BUFFERS latches the env and swaps the retained polygon set
     * (melonDS GPU3D::VBlank). */
    if (g->tracker.saw_swap) {
        g->latched = g->live;
        g->latched_sort = g->tracker.last_swap_param & 3;
        if (g->pending.n > 0) {
            PolyList tmp = g->current;
            g->current = g->pending;
            g->pending = tmp;
            g->pending.n = 0;
        }
    }
    /* the unit's command tracker ends here -- gx_replay uses one Tracker
     * per unit (tools/gx_replay.c), so the word-decode state resets at
     * every unit boundary; match that exactly for byte-identical output */
    memset(&g->tracker, 0, sizeof(g->tracker));
}

void gxd_advance(Gxd *g)
{
    latch_unit(g);
}

void gxd_frame(Gxd *g)
{
    /* 0. Phase-5BU: report the polygon counters of the frame that just ended */
    gx_dbg_pstat_dump();

    /* 1. frame model: SWAP_BUFFERS latch + retained-set swap */
    latch_unit(g);

    /* 2. 3D layer */
    if (g->windowed && g->gl3d) render_3d_gl(g);
    else                        render_3d_sw(g);

    /* 3. 2D state: flat views + window latch */
    dbg_fb3d(g);
    if (g->flat_dirty) rebuild_flats(g);
    if (!g->bulk_win) {
        /* melonDS GPU2D::UpdateWindows(0) at scanline 0 (the captured .2d
         * images already include this effect in their latch bytes) */
        int n;
        for (n = 0; n < 2; n++) {
            uint8_t *E = eng(g, n);
            uint8_t act0 = E[E_WIN0ACT], act1 = E[E_WIN1ACT];
            if (E[E_WIN0 + 3] == 0)      act0 &= (uint8_t)~0x1;
            else if (E[E_WIN0 + 2] == 0) act0 |= 0x1;
            if (E[E_WIN1 + 3] == 0)      act1 &= (uint8_t)~0x1;
            else if (E[E_WIN1 + 2] == 0) act1 |= 0x1;
            E[E_WIN0ACT] = act0;
            E[E_WIN1ACT] = act1;
        }
    }

    /* 4. compose both screens */
    {
        Gx2DState st;
        const uint32_t *fb;
        state_dump(g);
        gx2d_state_load(&st, g->img);
        if (g->fb3d_override)
            fb = g->fb3d_override;
        else if (!g->bulk_win && !(g->powcnt & 4))
            fb = NULL;              /* 3D rendering engine off (POWCNT1) */
        else
            fb = g->colorbuf;
        gx2d_render_frame(&st, fb, g->screen[0], g->screen[1]);
        dbg_2dinfo(g, &st);
    }

    /* 5. present + input */
    dbg_gxlog(g);
    if (g->windowed) {
        pump_events(g);
        present(g);
    }
    g->frames++;
    gx_dbg_frame_set(g->frames);   /* Phase-5BT: tag commands of the next frame */
}

int gxd_should_quit(const Gxd *g) { return g->quit; }

/* =====================================================================
 * Output access + trace hooks
 * ===================================================================== */

const uint32_t *gxd_screen(const Gxd *g, int which)
{
    return g->screen[which ? 1 : 0];
}

const uint32_t *gxd_fb3d(const Gxd *g) { return g->colorbuf; }

int gxd_write_screen_ppm(const Gxd *g, int which, const char *path)
{
    /* the screens are melonDS ExpandColor output: 32-bit BGRA; PPM wants
     * RGB.  (gx2d_write_ppm keeps its historical byte order -- pinned by
     * tests/golden/scene2d.ppm -- so we do not reuse it here.) */
    const uint32_t *buf = g->screen[which ? 1 : 0];
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (i = 0; i < FB_PIXELS; i++) {
        uint8_t rgb[3] = {
            (uint8_t)((buf[i] >> 16) & 0xFF),   /* r */
            (uint8_t)((buf[i] >> 8) & 0xFF),    /* g */
            (uint8_t)(buf[i] & 0xFF)            /* b */
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

int gxd_write_window_ppm(Gxd *g, const char *path)
{
    FILE *f;
    int x, y;

    if (!g->windowed || !g->win || !g->win_shot) return -1;
    f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", g->win_shot_w, g->win_shot_h);
    for (y = g->win_shot_h - 1; y >= 0; y--) {   /* GL reads bottom-up */
        for (x = 0; x < g->win_shot_w; x++) {
            const uint8_t *p = &g->win_shot[((size_t)y * g->win_shot_w + x) * 4];
            fwrite(p, 1, 3, f);
        }
    }
    fclose(f);
    return 0;
}

/* ---- full-state snapshot (Phase-5AQ: savestates) ----
 *
 * A savestate must restore the display model, not just the guest RAM: every
 * byte the compositor renders comes from state owned here (VRAM banks, the
 * 2D register image, the retained 3D polygon set, the latched render env,
 * the composed screen buffers).  The guest's writes reached this state
 * through gxd_io_write/gxd_gx_*, and a restore does NOT replay them, so the
 * state has to travel in the blob.
 *
 * Format (little-endian):
 *   u32 magic 'GDX1' | u32 version(1) | u32 sizeof(Gxd) | u32 GX2D_STATE_SIZE
 *   u32 n_pending | u32 n_current | u32 n_scratch | u32 sizeof(GxPolygon)
 *   then the Gxd struct as a byte image with every HOST-OWNED pointer field
 *   zeroed (win, pctx, gl3d, scr_tex, win_shot, img, fb3d_override, the three
 *   PolyList arrays, gx.draw_fn/draw_user, gx.surface.*, gx.env.tex/toon,
 *   gx.texmem.vram/pal), then the img bytes, then the three polygon arrays.
 *   The host fields are rebound from the LIVE instance on load — a state
 *   never carries a host pointer, so it is portable between processes and
 *   unaffected by ASLR.  Everything else (including padding) is written from
 *   a zeroed copy, so a state file is byte-reproducible.
 *
 * gxd_state_layout_fp() is the build fingerprint of that layout: the state
 * container refuses a blob produced by a different one. */

#define GXD_STATE_VERSION 1u
#define GXD_STATE_MAGIC 0x31584447u /* 'GDX1' little-endian */

typedef struct {
    uint32_t magic, version;
    uint32_t gxd_size, img_size;
    uint32_t n_pending, n_current, n_scratch, poly_size;
} GxdStateHdr;

uint32_t gxd_state_layout_fp(void)
{
    /* FNV-1a over the layout-defining numbers only: a change in any of them
     * (struct sizes, the 2D image size, the polygon record) means a state
     * written by another build cannot be restored safely. */
    uint32_t v[8];
    uint32_t h = 2166136261u;
    int i;
    v[0] = (uint32_t)sizeof(Gxd);
    v[1] = (uint32_t)sizeof(Gx);
    v[2] = (uint32_t)GX2D_STATE_SIZE;
    v[3] = (uint32_t)sizeof(GxPolygon);
    v[4] = (uint32_t)VRAM_BANKS;
    v[5] = (uint32_t)TEXVRAM_SIZE;
    v[6] = (uint32_t)TEXPAL_SIZE;
    v[7] = (uint32_t)FB_PIXELS;
    for (i = 0; i < 8; i++) {
        h ^= v[i];
        h *= 16777619u;
    }
    return h;
}

/* Allocate the scratch copy in which host pointers are blanked.  malloc'd
 * once; the saver is called at most a few times per run. */
static Gxd *state_scratch(void)
{
    static Gxd *z;
    if (!z) z = (Gxd *)calloc(1, sizeof(Gxd));
    return z;
}

size_t gxd_state_size(const Gxd *g)
{
    if (!g) return 0;
    return sizeof(GxdStateHdr) + sizeof(Gxd) + GX2D_STATE_SIZE +
           (size_t)(g->pending.n + g->current.n + g->scratch.n) * sizeof(GxPolygon);
}

int gxd_state_save(const Gxd *g, uint8_t *buf, size_t cap)
{
    GxdStateHdr h;
    Gxd *z;
    size_t need, off;
    if (!g || !buf) return -1;
    need = gxd_state_size(g);
    if (cap < need) return -2;
    z = state_scratch();
    if (!z) return -3;
    memcpy(z, g, sizeof *z);
    /* host-owned / self-referential pointers: never written to a file */
    z->img = NULL;
    z->win = NULL;
    z->pctx = NULL;
    z->gl3d = NULL;
    memset(z->scr_tex, 0, sizeof z->scr_tex);
    z->have_tex = 0;
    z->win_shot = NULL;
    z->fb3d_override = NULL;
    z->pending.poly = NULL;
    z->current.poly = NULL;
    z->scratch.poly = NULL;
    z->gx.draw_fn = NULL;
    z->gx.draw_user = NULL;
    memset(&z->gx.surface, 0, sizeof z->gx.surface);
    memset(&z->sf, 0, sizeof z->sf);   /* points into colorbuf/depthbuf too */
    z->gx.env.tex = NULL;
    z->gx.env.toon = NULL;
    z->gx.texmem.vram = NULL;
    z->gx.texmem.pal = NULL;

    memset(&h, 0, sizeof h);
    h.magic = GXD_STATE_MAGIC;
    h.version = GXD_STATE_VERSION;
    h.gxd_size = (uint32_t)sizeof(Gxd);
    h.img_size = (uint32_t)GX2D_STATE_SIZE;
    h.n_pending = (uint32_t)g->pending.n;
    h.n_current = (uint32_t)g->current.n;
    h.n_scratch = (uint32_t)g->scratch.n;
    h.poly_size = (uint32_t)sizeof(GxPolygon);

    off = 0;
    memcpy(buf + off, &h, sizeof h);                 off += sizeof h;
    memcpy(buf + off, z, sizeof *z);                 off += sizeof *z;
    memcpy(buf + off, g->img, GX2D_STATE_SIZE);      off += GX2D_STATE_SIZE;
    memcpy(buf + off, g->pending.poly, (size_t)g->pending.n * sizeof(GxPolygon));
    off += (size_t)g->pending.n * sizeof(GxPolygon);
    memcpy(buf + off, g->current.poly, (size_t)g->current.n * sizeof(GxPolygon));
    off += (size_t)g->current.n * sizeof(GxPolygon);
    memcpy(buf + off, g->scratch.poly, (size_t)g->scratch.n * sizeof(GxPolygon));
    off += (size_t)g->scratch.n * sizeof(GxPolygon);
    return (int)off;
}

/* Replace list contents from the blob; capacity grows to n. */
static int state_list_load(PolyList *l, const uint8_t *src, uint32_t n)
{
    if (n == 0) { l->n = 0; return 0; }
    if (n > l->cap || !l->poly) {
        GxPolygon *p = (GxPolygon *)realloc(l->poly, (size_t)n * sizeof *p);
        if (!p) return -1;
        l->poly = p;
        l->cap = (int)n;
    }
    memcpy(l->poly, src, (size_t)n * sizeof(GxPolygon));
    l->n = (int)n;
    return 0;
}

int gxd_state_load(Gxd *g, const uint8_t *buf, size_t len)
{
    GxdStateHdr h;
    Gxd tmp;
    const uint8_t *p;
    size_t off;
    uint32_t np, nc, ns;
    if (!g || !buf || len < sizeof h) return -1;
    memcpy(&h, buf, sizeof h);
    if (h.magic != GXD_STATE_MAGIC || h.version != GXD_STATE_VERSION) return -2;
    if (h.gxd_size != sizeof(Gxd) || h.img_size != GX2D_STATE_SIZE ||
        h.poly_size != sizeof(GxPolygon)) return -3;
    np = h.n_pending; nc = h.n_current; ns = h.n_scratch;
    if (len != sizeof h + sizeof(Gxd) + GX2D_STATE_SIZE +
               (size_t)(np + nc + ns) * sizeof(GxPolygon)) return -4;

    p = buf + sizeof h;
    memcpy(&tmp, p, sizeof tmp);
    off = sizeof h + sizeof tmp;

    /* Rebind everything the host owns.  A state never carries these. */
    tmp.windowed = g->windowed;
    tmp.gl_3d = g->gl_3d;
    tmp.scale = g->scale;
    tmp.layout = g->layout;
    tmp.win_w = g->win_w;
    tmp.win_h = g->win_h;
    tmp.img = g->img;
    tmp.win = g->win;
    tmp.pctx = g->pctx;
    tmp.gl3d = g->gl3d;
    memcpy(tmp.scr_tex, g->scr_tex, sizeof tmp.scr_tex);
    tmp.have_tex = g->have_tex;
    tmp.win_shot = g->win_shot;
    tmp.win_shot_w = g->win_shot_w;
    tmp.win_shot_h = g->win_shot_h;
    tmp.fb3d_override = g->fb3d_override;
    tmp.gx.draw_fn = g->gx.draw_fn;
    tmp.gx.draw_user = g->gx.draw_user;
    tmp.gx.surface = g->gx.surface;
    tmp.sf = g->sf;                    /* pointers into the live buffers */
    tmp.gx.env.tex = &tmp.gx.texmem;
    tmp.gx.env.toon = tmp.gx.toon;
    tmp.gx.texmem.vram = tmp.texvram;
    tmp.gx.texmem.pal = tmp.texpal;
    tmp.pending.poly = g->pending.poly;
    tmp.pending.cap = g->pending.cap;
    tmp.pending.n = 0;
    tmp.current.poly = g->current.poly;
    tmp.current.cap = g->current.cap;
    tmp.current.n = 0;
    tmp.scratch.poly = g->scratch.poly;
    tmp.scratch.cap = g->scratch.cap;
    tmp.scratch.n = 0;

    /* The freshly allocated instance (g) keeps its host bindings + poly
     * capacities; adopt the restored state into it. */
    {
        GxPolygon *pp = g->pending.poly; int ppc = g->pending.cap;
        GxPolygon *cp = g->current.poly; int cpc = g->current.cap;
        GxPolygon *sp = g->scratch.poly; int spc = g->scratch.cap;
        *g = tmp;
        g->pending.poly = pp; g->pending.cap = ppc;
        g->current.poly = cp; g->current.cap = cpc;
        g->scratch.poly = sp; g->scratch.cap = spc;
    }

    memcpy(g->img, buf + off, GX2D_STATE_SIZE);
    off += GX2D_STATE_SIZE;
    if (state_list_load(&g->pending, buf + off, np)) return -5;
    off += (size_t)np * sizeof(GxPolygon);
    if (state_list_load(&g->current, buf + off, nc)) return -5;
    off += (size_t)nc * sizeof(GxPolygon);
    if (state_list_load(&g->scratch, buf + off, ns)) return -5;

    /* Flat VRAM/2D views are derived from vramcnt + the raw banks: rebuild
     * them rather than trusting a stale flat image. */
    g->flat_dirty = 1;
    g->bulk_win = 0;
    return 0;
}

void gxd_load_2d_state(Gxd *g, const uint8_t *image)
{
    memcpy(g->img, image, GX2D_STATE_SIZE);
    memcpy(g->vramcnt, g->img + IMG_GLOBAL + G_VRAMCNT, VRAM_BANKS);
    memcpy(g->vram[0], g->img + IMG_RAW, 0x20000);
    memcpy(g->vram[1], g->img + IMG_RAW + 0x20000, 0x20000);
    memcpy(g->vram[2], g->img + IMG_RAW + 0x40000, 0x20000);
    memcpy(g->vram[3], g->img + IMG_RAW + 0x60000, 0x20000);
    remap_all(g);
    g->flat_dirty = 0;      /* the image's flat views are authoritative */
    g->bulk_win = 1;
}

void gxd_load_tex(Gxd *g, const uint8_t *texvram, uint32_t vram_size,
                  const uint16_t *texpal, uint32_t pal_count)
{
    if (texvram && vram_size)
        memcpy(g->texvram, texvram,
               vram_size < TEXVRAM_SIZE ? vram_size : TEXVRAM_SIZE);
    if (texpal && pal_count)
        memcpy(g->texpal, texpal,
               (size_t)(pal_count < TEXPAL_SIZE / 2 ? pal_count : TEXPAL_SIZE / 2) * 2);
    g->gx.texmem.vram = g->texvram;
    g->gx.texmem.pal = g->texpal;
}

void gxd_load_fb3d(Gxd *g, const uint32_t *fb)
{
    g->fb3d_override = fb;
}
