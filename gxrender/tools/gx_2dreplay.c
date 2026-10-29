/* gx_2dreplay.c -- composite DS frames from melonDS 2D captures and diff
 * against melonDS's final screens.
 *
 * Inputs per unit N (written by ref/melonds-2d-log.patch, see GX_TRACES.md):
 *   f%05u.2d  GX2D state image container (keyframe or delta vs the previous
 *             dumped unit; the chain is applied in ascending unit order)
 *   f%05u.fb  melonDS 3D layer (256*192 u32, 6-bit rgb + 5-bit alpha)
 *   f%05u.scr melonDS final composed screens (2 x 256*192 u32, 8-bit BGRA,
 *             buffer 0 then buffer 1 -- the Framebuffer[BackBuffer][0]/[1]
 *             layout)
 *
 * The tool renders each unit's frame through gx_2d (2D engine + compositor,
 * melonDS frame model) with the melonDS 3D layer as input, and reports
 * honest per-pixel diffs against f%05u.scr.
 *
 * Usage:
 *   gx_2dreplay <trace_dir> <out_dir> [--units A-B] [--ppm] [--csv file]
 *               [--quiet]
 *
 * With --ppm, per-unit PPMs <out>/f%05u_{s0,s1}.ppm are written for
 * differing units (and the matching golden as f%05u_{g0,g1}.ppm).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include "gx_2d.h"

#define FB_PIXELS (256 * 192)

static uint8_t *state_image;      /* persistent delta chain buffer */
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
    /* deltas are relative to the previously dumped unit's state */
    memcpy(state_next, state_image, GX2D_STATE_SIZE);
    if (flags & 1)
    {
        /* keyframe */
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

static int write_ppm(const char *path, const uint32_t *buf)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n256 192\n255\n");
    for (int i = 0; i < FB_PIXELS; i++)
    {
        uint8_t rgb[3] = {
            (uint8_t)(buf[i] & 0xFF),
            (uint8_t)((buf[i] >> 8) & 0xFF),
            (uint8_t)((buf[i] >> 16) & 0xFF)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}

typedef struct {
    long units;
    long long pixels;        /* units * 2 * FB_PIXELS */
    long long exact;
    long long diffpx;
    int maxdelta;            /* max per-channel delta */
    long long sumdelta;
} Totals;

static void diff_buffer(const uint32_t *rep, const uint32_t *gold, int npix,
                        Totals *t, int *buf_diff, int *buf_max)
{
    for (int i = 0; i < npix; i++)
    {
        uint32_t a = rep[i], b = gold[i];
        if (a == b)
        {
            t->exact++;
            continue;
        }
        t->diffpx++;
        (*buf_diff)++;
        int d;
        d = abs((int)(a & 0xFF) - (int)(b & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
        t->sumdelta += d;
        d = abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
        t->sumdelta += d;
        d = abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
        t->sumdelta += d;
    }
}

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_dir = NULL, *csv_path = NULL;
    int want_ppm = 0, quiet = 0;
    long range_a = -1, range_b = -1;

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--ppm")) want_ppm = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--units") && i + 1 < argc)
        {
            if (sscanf(argv[++i], "%ld-%ld", &range_a, &range_b) != 2)
            {
                fprintf(stderr, "gx_2dreplay: bad --units range\n");
                return 1;
            }
        }
        else if (!trace_dir) trace_dir = argv[i];
        else if (!out_dir) out_dir = argv[i];
        else
        {
            fprintf(stderr, "gx_2dreplay: unexpected argument %s\n", argv[i]);
            return 1;
        }
    }
    if (!trace_dir || !out_dir)
    {
        fprintf(stderr, "usage: gx_2dreplay <trace_dir> <out_dir> [--units A-B]"
                        " [--ppm] [--csv file] [--quiet]\n");
        return 1;
    }
    if (mkdir(out_dir, 0775) < 0 && errno != EEXIST)
    {
        fprintf(stderr, "gx_2dreplay: cannot create %s\n", out_dir);
        return 1;
    }

    state_image = malloc(GX2D_STATE_SIZE);
    state_next = malloc(GX2D_STATE_SIZE);
    if (!state_image || !state_next)
    {
        fprintf(stderr, "gx_2dreplay: out of memory\n");
        return 1;
    }
    memset(state_image, 0, GX2D_STATE_SIZE);

    /* discover units with .2d in ascending order */
    DIR *d = opendir(trace_dir);
    if (!d)
    {
        fprintf(stderr, "gx_2dreplay: cannot open %s\n", trace_dir);
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
            /* match exactly f%05ld.2d */
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
    /* sort ascending */
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
        if (csv) fprintf(csv, "unit,diffpx,maxdelta,exact_pct\n");
    }

    Totals t = {0, 0, 0, 0, 0, 0};
    long rendered = 0, missing = 0;

    for (long i = 0; i < n; i++)
    {
        long u = units[i];
        char p2d[1024], pfb[1024], pscr[1024];
        snprintf(p2d, sizeof(p2d), "%s/f%05ld.2d", trace_dir, u);
        snprintf(pfb, sizeof(pfb), "%s/f%05ld.fb", trace_dir, u);
        snprintf(pscr, sizeof(pscr), "%s/f%05ld.scr", trace_dir, u);

        if (load_2d_container(p2d) < 0)
        {
            fprintf(stderr, "gx_2dreplay: bad container %s\n", p2d);
            return 1;
        }
        /* chain: committed image advances only after a successful load */
        memcpy(state_image, state_next, GX2D_STATE_SIZE);

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

        uint32_t *out0 = malloc(FB_PIXELS * 8);
        uint32_t *out1 = out0 + FB_PIXELS;
        gx2d_render_frame(&st, fb, out0, out1);

        int diff0 = 0, diff1 = 0, max0 = 0, max1 = 0;
        t.units++;
        t.pixels += 2LL * FB_PIXELS;
        diff_buffer(out0, scr, FB_PIXELS, &t, &diff0, &max0);
        diff_buffer(out1, scr + FB_PIXELS, FB_PIXELS, &t, &diff1, &max1);
        rendered++;

        double pct = 100.0 * (double)(2 * FB_PIXELS - diff0 - diff1) /
                     (double)(2 * FB_PIXELS);
        if (!quiet)
            printf("unit %6ld: diff %5d px (%4d/%4d) max %2d  exact %.2f%%\n",
                   u, diff0 + diff1, diff0, diff1,
                   max0 > max1 ? max0 : max1, pct);
        if (csv)
            fprintf(csv, "%ld,%d,%d,%.4f\n", u, diff0 + diff1,
                    max0 > max1 ? max0 : max1, pct);

        if (want_ppm && (diff0 || diff1))
        {
            char q[1024];
            snprintf(q, sizeof(q), "%s/f%05ld_s0.ppm", out_dir, u);
            write_ppm(q, out0);
            snprintf(q, sizeof(q), "%s/f%05ld_s1.ppm", out_dir, u);
            write_ppm(q, out1);
            snprintf(q, sizeof(q), "%s/f%05ld_g0.ppm", out_dir, u);
            write_ppm(q, scr);
            snprintf(q, sizeof(q), "%s/f%05ld_g1.ppm", out_dir, u);
            write_ppm(q, scr + FB_PIXELS);
        }

        free(out0);
        free(fb);
        free(scr);
    }

    if (csv) fclose(csv);

    printf("---- gx_2dreplay totals ----\n");
    printf("units rendered: %ld (missing .scr: %ld)\n", rendered, missing);
    if (t.pixels)
    {
        printf("pixels: %lld  exact: %lld (%.4f%%)  differing: %lld (%.4f%%)\n",
               t.pixels, t.exact, 100.0 * (double)t.exact / (double)t.pixels,
               t.diffpx, 100.0 * (double)t.diffpx / (double)t.pixels);
        printf("max channel delta: %d  mean delta (diff px): %.2f\n",
               t.maxdelta,
               t.diffpx ? (double)t.sumdelta / (double)(t.diffpx * 3) : 0.0);
    }
    free(units);
    free(state_image);
    free(state_next);
    return 0;
}
