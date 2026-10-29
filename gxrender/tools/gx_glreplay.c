/* gx_glreplay.c -- replay melonDS GX capture traces through the gxrender
 * OpenGL engine (src/gx_gl3d.c) and produce per-unit .raw output in the
 * same packed layout as gx_replay (r | g<<8 | b<<16 | a<<24, 6-bit rgb +
 * 5-bit alpha), for per-pixel diffing against the software reference.
 *
 * The env latching / frame model is tools/gx_replay_common.h -- shared
 * verbatim with gx_replay.c so both replay streams identically.  Only the
 * render stage differs (software rasterizer vs the GL engine).
 *
 * Usage: gx_glreplay TRACE_DIR OUT_DIR [--start N] [--render A[-B]]...
 *                    [--ppm] [--all] [--quiet] [--present]
 *
 * --present also shows the frames in an SDL window (interactive runs).
 * Frames/sec and per-frame GL time are printed at exit (llvmpipe sanity).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gx_fifo.h"
#include "gx_raster.h"
#include "gx_tex.h"
#include "gx_gl3d.h"
#include "gx_replay_common.h"

#define FB_W 256
#define FB_H 192
#define TEXVRAM_SIZE (512 * 1024)
#define TEXPAL_SIZE (128 * 1024)

static uint8_t texvram[TEXVRAM_SIZE];
static uint16_t texpal[TEXPAL_SIZE / 2];
static uint32_t colorbuf[FB_W * FB_H];
static uint32_t depthbuf[FB_W * FB_H];
static uint32_t attrclear[FB_W * FB_H]; /* attr0 clear image (rx_clear_frame
                                         * writes color/depth/attr) */
static uint32_t readbuf[FB_W * FB_H];

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_dir = NULL;
    int want_ppm = 0, want_all = 0, quiet = 0, start_unit = 0, present = 0;
    int render_lo[8], render_hi[8], n_render = 0;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ppm")) want_ppm = 1;
        else if (!strcmp(argv[i], "--all")) want_all = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--present")) present = 1;
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
        fprintf(stderr, "usage: gx_glreplay TRACE_DIR OUT_DIR [--start N] [--render A[-B]]... [--ppm] [--all] [--quiet] [--present]\n");
        return 2;
    }

    GxGl3d *gl = gx_gl3d_open(present ? 2 : 1);
    if (!gl) {
        fprintf(stderr, "gx_glreplay: GL engine init failed\n");
        return 1;
    }
    printf("gx_glreplay: %s / %s / %s\n", gx_gl3d_vendor(gl),
           gx_gl3d_renderer(gl), gx_gl3d_version(gl));

    RxModel m;
    rx_init(&m, colorbuf, depthbuf, FB_W, FB_H);

    GxSurface sf = { .color = colorbuf, .depth = depthbuf, .attr = attrclear,
                     .w = FB_W, .h = FB_H };

    long units = 0, rendered = 0, total_polys = 0;
    double total_render_ms = 0.0;
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

            /* clear semantics shared with the software path: the CPU-side
             * clear fills color/depth/attr exactly like rx_clear_frame */
            sf.attr = attrclear;
            rx_clear_frame(&sf, &m.latched, texvram, FB_W, FB_H);
            rx_build_render_order(&m);

            double t0 = now_ms();
            gx_gl3d_load_clear(gl, colorbuf, depthbuf, attrclear);
            gx_gl3d_set_env(gl, &env, &post);
            for (i = 0; i < m.scratch.n; i++)
                gx_gl3d_draw_polygon(gl, &m.scratch.poly[i]);
            gx_gl3d_post_pass(gl);
            gx_gl3d_read_color(gl, readbuf);
            total_render_ms += now_ms() - t0;

            snprintf(path, sizeof(path), "%s/f%05d.raw", out_dir, unit);
            FILE *o = fopen(path, "wb");
            if (!o) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            fwrite(readbuf, 4, FB_W * FB_H, o);
            fclose(o);
            if (want_ppm) {
                GxSurface out = { .color = readbuf, .w = FB_W, .h = FB_H };
                snprintf(path, sizeof(path), "%s/f%05d.ppm", out_dir, unit);
                gx_write_ppm(path, &out);
            }
            if (present)
                gx_gl3d_present(gl);
            rendered++;
            total_polys += m.scratch.n;
            if (!quiet)
                printf("unit %d: entries=%d swap=%d vtx=%d set=%d rendered=%d\n",
                       unit, m.tr.n_entries, m.tr.saw_swap, m.tr.saw_vtx,
                       m.current.n, m.scratch.n);
        }
        skip_render:;
    }

    GxGl3dStats st;
    gx_gl3d_get_stats(gl, &st);
    printf("gx_glreplay: units=%ld rendered=%ld polys_drawn=%ld "
           "unknown_cmds=%u retained=%d\n",
           units, rendered, total_polys, m.gx.n_unknown_cmds, m.current.n);
    printf("  gl stats: polys=%ld textures=%ld flat=%ld degenerate=%ld\n",
           st.polys_drawn, st.textures_decoded, st.flat_polys,
           st.degenerate_polys);
    if (rendered > 0)
        printf("  perf: %ld frames in %.1f ms = %.2f ms/frame = %.2f frames/sec"
               " (llvmpipe sanity)\n",
               rendered, total_render_ms, total_render_ms / (double)rendered,
               1000.0 * (double)rendered / total_render_ms);
    gx_gl3d_close(gl);
    return 0;
}
