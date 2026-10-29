/* gx_gl2dreplay.c -- replay the melonDS 2D state captures (f%05u.2d chain)
 * through the GPU compositor (src/gx_gl2d.c) and validate per-pixel
 * against BOTH references:
 *   - the software path (src/gx_2d.c)  -- GL-vs-SOFTWARE diff (primary)
 *   - the melonDS golden (f%05u.scr)   -- GL-vs-melonDS diff (secondary)
 *
 * The .2d delta-chain handling is identical to tools/gx_2dreplay.c (the
 * chain is relative to the previously DUMPED unit: run over a contiguous
 * range in one invocation).
 *
 * Usage: gx_gl2dreplay TRACE_DIR OUT_DIR [--units A-B] [--dump] [--csv FILE]
 *                       [--quiet]
 *                       [--save-state UNIT FILE] [--fixed-state FILE]
 * --dump writes the GL screens as f%05u_g{0,1}.raw (8-bit BGRA, .scr
 * layout) for external diffing.
 * --save-state UNIT FILE writes the exact render state image used for UNIT
 * (after its delta container is applied) to FILE.
 * --fixed-state FILE loads the render state from FILE and uses it for every
 * processed unit (containers are NOT applied) -- for single-unit bisecting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include "gx_2d.h"
#include "gx_gl2d.h"

#define FB_PIXELS (256 * 192)

static uint8_t *state_image;
static uint8_t *state_next;

static int load_2d_container(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[20];
    if (fread(hdr, 1, 20, f) != 20 || memcmp(hdr, "GX2D", 4) != 0)
    {
        fclose(f);
        return -1;
    }
    uint32_t ver, unit, flags, size;
    memcpy(&ver, hdr + 4, 4);
    memcpy(&unit, hdr + 8, 4);
    memcpy(&flags, hdr + 12, 4);
    memcpy(&size, hdr + 16, 4);
    (void)unit;
    if (ver != 1 || size != GX2D_STATE_SIZE)
    {
        fclose(f);
        return -1;
    }
    memcpy(state_next, state_image, GX2D_STATE_SIZE);
    if (flags & 1)
    {
        if (fread(state_next, 1, size, f) != size)
        {
            fclose(f);
            return -1;
        }
    }
    else
    {
        uint32_t nruns = 0;
        if (fread(&nruns, 4, 1, f) != 1)
        {
            fclose(f);
            return -1;
        }
        for (uint32_t r = 0; r < nruns; r++)
        {
            uint32_t off, len;
            if (fread(&off, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 ||
                off + len > size)
            {
                fclose(f);
                return -1;
            }
            if (fread(state_next + off, 1, len, f) != len)
            {
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

static uint32_t *load_fb(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t *buf = malloc(FB_PIXELS * 4);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 4, FB_PIXELS, f) != FB_PIXELS)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

static uint32_t *load_scr(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint32_t *buf = malloc(FB_PIXELS * 8);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 4, FB_PIXELS * 2, f) != FB_PIXELS * 2)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

typedef struct {
    long units;
    long long pixels;
    long long exact;
    long long diffpx;
    int maxdelta;
    long long sumdelta;
} Totals;

static void diff_buffer(const uint32_t *a, const uint32_t *b, int npix,
                        Totals *t, int *buf_diff, int *buf_max)
{
    for (int i = 0; i < npix; i++)
    {
        uint32_t va = a[i], vb = b[i];
        if (va == vb)
        {
            t->exact++;
            continue;
        }
        t->diffpx++;
        (*buf_diff)++;
        for (int sh = 0; sh < 24; sh += 8)
        {
            int d = abs((int)((va >> sh) & 0xFF) - (int)((vb >> sh) & 0xFF));
            if (d > *buf_max) *buf_max = d;
            if (d > t->maxdelta) t->maxdelta = d;
            t->sumdelta += d;
        }
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_dir = NULL, *csv_path = NULL;
    const char *fixed_state_path = NULL, *save_state_path = NULL;
    long save_state_unit = -1;
    int want_dump = 0, quiet = 0;
    long range_a = -1, range_b = -1;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--dump")) want_dump = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--fixed-state") && i + 1 < argc) fixed_state_path = argv[++i];
        else if (!strcmp(argv[i], "--save-state") && i + 2 < argc)
        {
            save_state_unit = atol(argv[++i]);
            save_state_path = argv[++i];
        }
        else if (!strcmp(argv[i], "--units") && i + 1 < argc)
        {
            if (sscanf(argv[++i], "%ld-%ld", &range_a, &range_b) != 2)
            {
                fprintf(stderr, "gx_gl2dreplay: bad --units range\n");
                return 1;
            }
        }
        else if (!trace_dir) trace_dir = argv[i];
        else if (!out_dir) out_dir = argv[i];
        else { fprintf(stderr, "unexpected argument %s\n", argv[i]); return 1; }
    }
    if (!trace_dir || !out_dir)
    {
        fprintf(stderr, "usage: gx_gl2dreplay <trace_dir> <out_dir> [--units A-B]"
                        " [--dump] [--csv file] [--quiet]\n");
        return 1;
    }
    if (mkdir(out_dir, 0777) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "gx_gl2dreplay: cannot create %s\n", out_dir);
        return 1;
    }

    GxGl2d *gl = gx_gl2d_open();
    if (!gl)
    {
        fprintf(stderr, "gx_gl2dreplay: GL init failed\n");
        return 1;
    }
    printf("gx_gl2dreplay: %s / %s\n", gx_gl2d_renderer(gl), gx_gl2d_version(gl));

    state_image = malloc(GX2D_STATE_SIZE);
    state_next = malloc(GX2D_STATE_SIZE);
    if (!state_image || !state_next)
    {
        fprintf(stderr, "gx_gl2dreplay: out of memory\n");
        return 1;
    }
    memset(state_image, 0, GX2D_STATE_SIZE);
    if (fixed_state_path)
    {
        FILE *fs = fopen(fixed_state_path, "rb");
        if (!fs || fread(state_image, 1, GX2D_STATE_SIZE, fs) != GX2D_STATE_SIZE)
        {
            fprintf(stderr, "gx_gl2dreplay: cannot read fixed state %s\n",
                    fixed_state_path);
            return 1;
        }
        fclose(fs);
    }

    DIR *d = opendir(trace_dir);
    if (!d)
    {
        fprintf(stderr, "gx_gl2dreplay: cannot open %s\n", trace_dir);
        return 1;
    }
    long cap = 4096, n = 0;
    long *units = malloc((size_t)cap * sizeof(long));
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
    {
        long u;
        char suf;
        if (sscanf(de->d_name, "f%05ld.%c", &u, &suf) == 2 && suf == '2')
        {
            char expect[32];
            snprintf(expect, sizeof(expect), "f%05ld.2d", u);
            if (strcmp(de->d_name, expect) != 0) continue;
            if (range_a >= 0 && (u < range_a || u > range_b)) continue;
            if (n == cap)
            {
                cap *= 2;
                units = realloc(units, (size_t)cap * sizeof(long));
            }
            units[n++] = u;
        }
    }
    closedir(d);
    for (long i = 1; i < n; i++)
    {
        long v = units[i], j = i;
        while (j > 0 && units[j - 1] > v) { units[j] = units[j - 1]; j--; }
        units[j] = v;
    }

    FILE *csv = NULL;
    if (csv_path)
    {
        csv = fopen(csv_path, "w");
        if (csv) fprintf(csv, "unit,soft_diffpx,soft_max,gold_diffpx,gold_max\n");
    }

    Totals tsw = {0, 0, 0, 0, 0, 0};
    Totals tgold = {0, 0, 0, 0, 0, 0};
    long rendered = 0, missing = 0;
    double total_ms = 0.0;

    for (long i = 0; i < n; i++)
    {
        long u = units[i];
        char p2d[1024], pfb[1024], pscr[1024];
        snprintf(p2d, sizeof(p2d), "%s/f%05ld.2d", trace_dir, u);
        snprintf(pfb, sizeof(pfb), "%s/f%05ld.fb", trace_dir, u);
        snprintf(pscr, sizeof(pscr), "%s/f%05ld.scr", trace_dir, u);

        if (!fixed_state_path)
        {
            if (load_2d_container(p2d) < 0)
            {
                fprintf(stderr, "gx_gl2dreplay: bad container %s\n", p2d);
                return 1;
            }
            memcpy(state_image, state_next, GX2D_STATE_SIZE);
        }
        if (u == save_state_unit && save_state_path)
        {
            FILE *o = fopen(save_state_path, "wb");
            if (o)
            {
                fwrite(state_image, 1, GX2D_STATE_SIZE, o);
                fclose(o);
                printf("gx_gl2dreplay: saved render state for unit %ld -> %s\n",
                       u, save_state_path);
            }
        }

        uint32_t *fb = load_fb(pfb);
        uint32_t *scr = load_scr(pscr);
        if (!scr)
        {
            missing++;
            free(fb);
            continue;
        }

        Gx2DState st;
        gx2d_state_load(&st, state_image);
        if (getenv("GX_GL2D_DEBUG"))
        {
            const Gx2DEngine *e2 = &st.eng[1];
            printf("dbg unit %ld: eng1 dispcnt=%08x bgcnt=%04x %04x %04x %04x "
                   "bright=%04x pal400=%04x enabled=%d%d swap=%d mode=%d\n",
                   u, e2->dispcnt, e2->bgcnt[0], e2->bgcnt[1], e2->bgcnt[2],
                   e2->bgcnt[3], st.masterbright[1],
                   st.palette[0x400] | (st.palette[0x401] << 8),
                   st.eng_enabled[0], st.eng_enabled[1], st.screen_swap,
                   (int)((e2->dispcnt >> 16) & 1));
        }

        uint32_t *sw0 = malloc(FB_PIXELS * 8);
        uint32_t *sw1 = sw0 + FB_PIXELS;
        uint32_t *gl0 = malloc(FB_PIXELS * 8);
        uint32_t *gl1 = gl0 + FB_PIXELS;
        gx2d_render_frame(&st, fb, sw0, sw1);

        double t0 = now_ms();
        if (!st.screens_enabled)
        {
            for (int k = 0; k < FB_PIXELS; k++) { gl0[k] = 0xFF000000u; gl1[k] = 0xFF000000u; }
        }
        else
        {
            uint32_t *bufa = malloc(FB_PIXELS * 4);
            uint32_t *bufb = malloc(FB_PIXELS * 4);
            gx_gl2d_render_screen(gl, &st, 0, fb, bufa);
            gx_gl2d_render_screen(gl, &st, 1, NULL, bufb);
            if (st.screen_swap)
            {
                memcpy(gl0, bufa, FB_PIXELS * 4);
                memcpy(gl1, bufb, FB_PIXELS * 4);
            }
            else
            {
                memcpy(gl0, bufb, FB_PIXELS * 4);
                memcpy(gl1, bufa, FB_PIXELS * 4);
            }
            free(bufa);
            free(bufb);
        }
        total_ms += now_ms() - t0;

        int d0 = 0, d1 = 0, m0 = 0, m1 = 0;
        tsw.units++;
        tsw.pixels += 2LL * FB_PIXELS;
        diff_buffer(gl0, sw0, FB_PIXELS, &tsw, &d0, &m0);
        diff_buffer(gl1, sw1, FB_PIXELS, &tsw, &d1, &m1);

        int g0d = 0, g1d = 0, g0m = 0, g1m = 0;
        tgold.units++;
        tgold.pixels += 2LL * FB_PIXELS;
        diff_buffer(gl0, scr, FB_PIXELS, &tgold, &g0d, &g0m);
        diff_buffer(gl1, scr + FB_PIXELS, FB_PIXELS, &tgold, &g1d, &g1m);

        if (!quiet)
            printf("unit %6ld: soft_diff %6d px (%5d/%5d) max %3d | "
                   "gold_diff %6d px max %3d\n",
                   u, d0 + d1, d0, d1, m0 > m1 ? m0 : m1,
                   g0d + g1d, g0m > g1m ? g0m : g1m);
        if (csv)
            fprintf(csv, "%ld,%d,%d,%d,%d\n", u, d0 + d1,
                    m0 > m1 ? m0 : m1, g0d + g1d, g0m > g1m ? g0m : g1m);

        if (want_dump)
        {
            char q[1024];
            FILE *o;
            snprintf(q, sizeof(q), "%s/f%05ld_s0.raw", out_dir, u);
            o = fopen(q, "wb");
            if (o) { fwrite(gl0, 4, FB_PIXELS, o); fclose(o); }
            snprintf(q, sizeof(q), "%s/f%05ld_s1.raw", out_dir, u);
            o = fopen(q, "wb");
            if (o) { fwrite(gl1, 4, FB_PIXELS, o); fclose(o); }
            snprintf(q, sizeof(q), "%s/f%05ld_w0.raw", out_dir, u);
            o = fopen(q, "wb");
            if (o) { fwrite(sw0, 4, FB_PIXELS, o); fclose(o); }
            snprintf(q, sizeof(q), "%s/f%05ld_w1.raw", out_dir, u);
            o = fopen(q, "wb");
            if (o) { fwrite(sw1, 4, FB_PIXELS, o); fclose(o); }
        }

        free(sw0);
        free(gl0);
        free(fb);
        free(scr);
        rendered++;
    }

    if (csv) fclose(csv);

    printf("---- gx_gl2dreplay totals (GL vs SOFTWARE) ----\n");
    printf("units: %ld (missing .scr: %ld)  renderer: %s\n",
           rendered, missing, gx_gl2d_renderer(gl));
    if (tsw.pixels)
    {
        printf("pixels: %lld  exact: %lld (%.4f%%)  differing: %lld (%.4f%%)\n",
               tsw.pixels, tsw.exact, 100.0 * (double)tsw.exact / (double)tsw.pixels,
               tsw.diffpx, 100.0 * (double)tsw.diffpx / (double)tsw.pixels);
        printf("max channel delta: %d\n", tsw.maxdelta);
    }
    printf("---- GL vs melonDS golden ----\n");
    if (tgold.pixels)
    {
        printf("pixels: %lld  exact: %lld (%.4f%%)  differing: %lld (%.4f%%)\n",
               tgold.pixels, tgold.exact,
               100.0 * (double)tgold.exact / (double)tgold.pixels,
               tgold.diffpx, 100.0 * (double)tgold.diffpx / (double)tgold.pixels);
        printf("max channel delta: %d\n", tgold.maxdelta);
    }
    if (rendered > 0)
        printf("perf: %ld frames (2 engines each) in %.1f ms = %.2f ms/frame\n",
               rendered, total_ms, total_ms / (double)rendered);
    gx_gl2d_close(gl);
    free(units);
    free(state_image);
    free(state_next);
    return 0;
}
