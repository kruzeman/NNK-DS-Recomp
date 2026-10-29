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
 * Frame model (mirrors melonDS GPU3D.cpp / GPU3D_Soft.cpp): a "unit" is the
 * GX traffic between two VBlank() calls; render env regs latch at
 * SWAP_BUFFERS flushes; submitted polygons are retained (a flush with no
 * polygons keeps the previous set); textures are sampled from the unit's
 * flat memory snapshot; render order is melonDS YSort.  The model itself
 * lives in tools/gx_replay_common.h and is shared verbatim with
 * tools/gx_glreplay.c (OpenGL backend) so both replay streams identically.
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
#include "gx_replay_common.h"

#define FB_W 256
#define FB_H 192
#define TEXVRAM_SIZE (512 * 1024)
#define TEXPAL_SIZE (128 * 1024)

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

    RxModel m;
    rx_init(&m, colorbuf, depthbuf, FB_W, FB_H);

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

        int bad = rx_feed_unit(&m, data, len);
        if (bad) {
            fprintf(stderr, "unit %d: bad record type at offset %d\n", unit, bad);
            free(data);
            return 1;
        }
        free(data);
        units++;

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

            GxRasterEnv env;
            GxPostEnv post;
            GxTexMem tex;
            rx_build_env(&m, &env, &post, &tex, texvram, TEXVRAM_SIZE,
                         texpal, TEXPAL_SIZE / 2);

            rx_clear_frame(&sf, &m.latched, texvram, FB_W, FB_H);
            /* SWAP sort mode bit0=1: sort opaque only; bit0=0: Y-sort all
             * (translucent SortKeys carry 0x10000, GPU3D.cpp:2657-2671) */
            rx_build_render_order(&m);
            if (getenv("GX_POLY_DUMP")) {
                /* debug: dump the retained polygon set for this unit
                 * (env GX_POLY_DUMP=<prefix>, writes <prefix>.<unit>.txt) */
                char pdpath[1200];
                snprintf(pdpath, sizeof(pdpath), "%s.%d.txt",
                         getenv("GX_POLY_DUMP"), unit);
                FILE *pd = fopen(pdpath, "w");
                if (pd) {
                    for (i = 0; i < m.scratch.n; i++) {
                        const GxPolygon *p = &m.scratch.poly[i];
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
            for (i = 0; i < m.scratch.n; i++)
                gx_raster_polygon(&sf, &m.scratch.poly[i], &env);
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
            total_polys += m.scratch.n;
            if (!quiet)
                printf("unit %d: entries=%d swap=%d vtx=%d set=%d rendered=%d\n",
                       unit, m.tr.n_entries, m.tr.saw_swap, m.tr.saw_vtx,
                       m.current.n, m.scratch.n);
        }
        skip_render:;
    }

    printf("gx_replay: units=%ld rendered=%ld polys_drawn=%ld "
           "unknown_cmds=%u retained=%d\n"
           "  polys_submitted=%u culled_front=%u culled_back=%u "
           "clip_dropped=%u degenerate=%u\n"
           "  latched env: disp_cnt=%08x clear_attr1=%08x clear_attr2=%08x "
           "alpha_ref=%u clearz=%u\n",
           units, rendered, total_polys, m.gx.n_unknown_cmds, m.current.n,
           m.gx.n_polys, m.gx.n_culled_front, m.gx.n_culled_back,
           m.gx.n_drop_clip, m.gx.n_drop_degenerate,
           m.latched.disp_cnt, m.latched.clear_attr1, m.latched.clear_attr2,
           m.latched.alpha_ref_val,
           (unsigned)(((m.latched.clear_attr2 & 0x7FFF) * 0x200) + 0x1FF));
    return 0;
}
