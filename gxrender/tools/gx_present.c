/* gx_present.c -- replay melonDS GX capture streams through the LIVE
 * gx_display runtime bridge (the same calls the recompiled runtime's IO
 * layer makes) and prove the whole display pipeline: GX FIFO traffic ->
 * 3D frame model -> compositor -> SDL2/GL window present (both screens)
 * or headless deterministic output.
 *
 * Inputs per unit N (see GX_TRACES.md):
 *   f%05u.gx   record stream: 01 <u32 word> (GXFIFO), 02 <u8 width>
 *              <u32 addr> <u32 val> (3D MMIO), 03 <u8 cmd> <u32 param>
 *              (direct port) -- fed via gxd_io_write / gxd_gx_fifo_write /
 *              gxd_gx_cmd exactly like the runtime will
 *   f%05u.2d   GX2D state container (keyframe/delta) -> gxd_load_2d_state
 *   f%05u.fb   melonDS 3D layer (feed3d=fb: used as-is via gxd_load_fb3d)
 *   f%05u.texvram / f%05u.texpal  texture snapshots -> gxd_load_tex
 *   f%05u.scr  melonDS final screens (golden reference for diffs)
 *
 * Usage:
 *   gx_present TRACE OUT [--units A-B] [--feed3d gx|fb] [--window]
 *       [--layout side|stack] [--scale N] [--win WxH] [--gl-3d]
 *       [--synth-bg0] [--shots-every N] [--shots a,b,c] [--ppm]
 *       [--csv FILE] [--quiet]
 *
 *   --feed3d gx  render the 3D layer from the .gx stream through the frame
 *                model (full pipeline; also writes OUT/f%05u.raw for
 *                byte-comparison with gx_replay)
 *   --feed3d fb  use the captured .fb layer (isolates compositor + present;
 *                totals are comparable with gx_2dreplay)
 *   --synth-bg0  write a minimal live 2D state via gxd_io_write (POWCNT1 +
 *                DISPCNT BG0) so the 3D layer shows when a trace has no
 *                .2d state (canonB windowed evidence)
 *   --shots-every N / --shots a,b,...  window screenshots (whole window incl.
 *                letterbox) + per-screen PPMs at those units
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>

#include "gx_display.h"
#include "gx_2d.h"

#define FB_PIXELS (256 * 192)

static uint8_t *state_image, *state_next;

static int load_2d_container(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[20];
    uint32_t ver, unit, flags, size;

    if (!f) return -1;
    if (fread(hdr, 1, 20, f) != 20 || memcmp(hdr, "GX2D", 4) != 0) {
        fclose(f);
        return -1;
    }
    memcpy(&ver, hdr + 4, 4);
    memcpy(&unit, hdr + 8, 4);
    memcpy(&flags, hdr + 12, 4);
    memcpy(&size, hdr + 16, 4);
    (void)unit;
    if (ver != 1 || size != GX2D_STATE_SIZE) {
        fclose(f);
        return -1;
    }
    memcpy(state_next, state_image, GX2D_STATE_SIZE);
    if (flags & 1) {
        if (fread(state_next, 1, size, f) != size) { fclose(f); return -1; }
    } else {
        uint32_t nruns = 0, r;
        if (fread(&nruns, 4, 1, f) != 1) { fclose(f); return -1; }
        for (r = 0; r < nruns; r++) {
            uint32_t off, len;
            if (fread(&off, 4, 1, f) != 1 || fread(&len, 4, 1, f) != 1 ||
                off + len > size) {
                fclose(f);
                return -1;
            }
            if (fread(state_next + off, 1, len, f) != len) {
                fclose(f);
                return -1;
            }
        }
    }
    fclose(f);
    return 0;
}

static void *load_file(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    void *buf;
    if (!f) return NULL;
    buf = malloc(want);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, want, f) != want) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return buf;
}

/* Feed one .gx record stream through the gxd IO surface. */
static int feed_gx(Gxd *g, const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t *data = NULL;
    size_t len = 0, cap = 0, off = 0;
    uint8_t buf[65536];
    size_t got;

    if (!f) return 0;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (len + got > cap) {
            cap = cap ? cap * 2 : 1 << 16;
            while (cap < len + got) cap *= 2;
            data = realloc(data, cap);
            if (!data) { fclose(f); return -1; }
        }
        memcpy(data + len, buf, got);
        len += got;
    }
    fclose(f);

    while (off < len) {
        uint8_t type = data[off++];
        if (type == 0x01 && off + 4 <= len) {
            uint32_t val;
            memcpy(&val, data + off, 4);
            off += 4;
            gxd_gx_fifo_write(g, val);
        } else if (type == 0x02 && off + 9 <= len) {
            uint8_t width = data[off];
            uint32_t addr, val;
            memcpy(&addr, data + off + 1, 4);
            memcpy(&val, data + off + 5, 4);
            off += 9;
            gxd_io_write(g, addr, val, width);
        } else if (type == 0x03 && off + 5 <= len) {
            uint8_t cmd = data[off];
            uint32_t val;
            memcpy(&val, data + off + 1, 4);
            off += 5;
            gxd_gx_cmd(g, cmd, val);
        } else {
            fprintf(stderr, "gx_present: bad record type %u in %s\n",
                    type, path);
            free(data);
            return -1;
        }
    }
    free(data);
    return 0;
}

typedef struct {
    long units;
    long long pixels;
    long long exact;
    long long diffpx;
    int maxdelta;
} Totals;

static void diff_buffer(const uint32_t *rep, const uint32_t *gold, int npix,
                        Totals *t, int *buf_diff, int *buf_max)
{
    int i;
    for (i = 0; i < npix; i++) {
        uint32_t a = rep[i], b = gold[i];
        int d;
        if (a == b) { t->exact++; continue; }
        t->diffpx++;
        (*buf_diff)++;
        d = abs((int)(a & 0xFF) - (int)(b & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
        d = abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
        d = abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
        if (d > *buf_max) *buf_max = d;
        if (d > t->maxdelta) t->maxdelta = d;
    }
}

int main(int argc, char **argv)
{
    const char *trace_dir = NULL, *out_dir = NULL, *csv_path = NULL;
    int feed3d = 0;                 /* 0 = auto, 1 = gx, 2 = fb */
    int windowed = 0, gl3d = 0, scale = 2, layout = GXD_LAYOUT_SIDE;
    int synth_bg0 = 0, want_ppm = 0, quiet = 0;
    int shots_every = 0, win_w = 0, win_h = 0;
    long shots[64];
    int n_shots = 0;
    long range_a = -1, range_b = -1;
    int i;

    state_image = malloc(GX2D_STATE_SIZE);
    state_next = malloc(GX2D_STATE_SIZE);
    if (!state_image || !state_next) {
        fprintf(stderr, "gx_present: out of memory\n");
        return 1;
    }
    memset(state_image, 0, GX2D_STATE_SIZE);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--window")) windowed = 1;
        else if (!strcmp(argv[i], "--gl-3d")) gl3d = 1;
        else if (!strcmp(argv[i], "--synth-bg0")) synth_bg0 = 1;
        else if (!strcmp(argv[i], "--ppm")) want_ppm = 1;
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "--feed3d") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "gx")) feed3d = 1;
            else if (!strcmp(m, "fb")) feed3d = 2;
            else { fprintf(stderr, "gx_present: bad --feed3d %s\n", m); return 2; }
        } else if (!strcmp(argv[i], "--layout") && i + 1 < argc) {
            if (!strcmp(argv[++i], "stack")) layout = GXD_LAYOUT_STACK;
            else layout = GXD_LAYOUT_SIDE;
        } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--win") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        } else if (!strcmp(argv[i], "--shots-every") && i + 1 < argc) {
            shots_every = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--shots") && i + 1 < argc) {
            const char *s = argv[++i];
            while (*s && n_shots < 64) {
                shots[n_shots++] = strtol(s, (char **)&s, 10);
                if (*s == ',') s++;
            }
        } else if (!strcmp(argv[i], "--csv") && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (!strcmp(argv[i], "--units") && i + 1 < argc) {
            if (sscanf(argv[++i], "%ld-%ld", &range_a, &range_b) != 2) {
                fprintf(stderr, "gx_present: bad --units range\n");
                return 2;
            }
        } else if (!trace_dir) trace_dir = argv[i];
        else if (!out_dir) out_dir = argv[i];
        else {
            fprintf(stderr, "gx_present: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!trace_dir || !out_dir) {
        fprintf(stderr, "usage: gx_present TRACE OUT [--units A-B] "
                        "[--feed3d gx|fb] [--window] [--gl-3d] "
                        "[--layout side|stack] [--scale N] [--win WxH] "
                        "[--synth-bg0] [--shots-every N] [--shots a,b] "
                        "[--ppm] [--csv FILE] [--quiet]\n");
        return 2;
    }
    if (mkdir(out_dir, 0775) < 0 && errno != EEXIST) {
        fprintf(stderr, "gx_present: cannot create %s\n", out_dir);
        return 1;
    }

    /* unit discovery: .2d-driven (canon2d) or .gx-driven (canonB) */
    {
        DIR *d = opendir(trace_dir);
        struct dirent *de;
        long cap = 4096, n = 0;
        long *units;
        int have_2d = 0;

        if (!d) {
            fprintf(stderr, "gx_present: cannot open %s\n", trace_dir);
            return 1;
        }
        units = malloc((size_t)cap * sizeof(long));
        while ((de = readdir(d)) != NULL) {
            long u;
            char suf;
            if (sscanf(de->d_name, "f%05ld.%c", &u, &suf) != 2) continue;
            if (suf == '2') have_2d = 1;
        }
        rewinddir(d);
        while ((de = readdir(d)) != NULL) {
            long u;
            char suf;
            char expect[32];
            if (sscanf(de->d_name, "f%05ld.%c", &u, &suf) != 2) continue;
            snprintf(expect, sizeof(expect), have_2d ? "f%05ld.2d" : "f%05ld.gx", u);
            if (strcmp(de->d_name, expect) != 0) continue;
            if (range_a >= 0 && (u < range_a || u > range_b)) continue;
            if (n == cap) {
                cap *= 2;
                units = realloc(units, (size_t)cap * sizeof(long));
            }
            units[n++] = u;
        }
        closedir(d);
        /* sort ascending */
        for (long k = 1; k < n; k++) {
            long v = units[k], j = k;
            while (j > 0 && units[j - 1] > v) {
                units[j] = units[j - 1];
                j--;
            }
            units[j] = v;
        }

        if (feed3d == 0) feed3d = have_2d ? 2 : 1;

        {
            GxdConfig cfg;
            Gxd *g;
            FILE *csv = NULL;
            Totals t = { 0, 0, 0, 0, 0 };
            long rendered = 0, missing_scr = 0;

            memset(&cfg, 0, sizeof(cfg));
            cfg.windowed = windowed;
            cfg.gl_3d = gl3d;
            cfg.scale = scale > 0 ? scale : 1;
            cfg.layout = layout;
            cfg.title = "gx_present (DS screens)";
            g = gxd_open(&cfg);
            if (!g) {
                fprintf(stderr, "gx_present: gxd_open failed\n");
                return 1;
            }
            if (windowed)
                printf("present window: %s, 3D=%s\n",
                       layout == GXD_LAYOUT_STACK ? "stacked" : "side-by-side",
                       gl3d ? "GL" : "software");

            if (csv_path) {
                csv = fopen(csv_path, "w");
                if (csv) fprintf(csv, "unit,diffpx,maxdelta,exact_pct\n");
            }

            if (synth_bg0) {
                /* minimal live 2D state via the io surface: screens +
                 * engines + 3D render on, engine A shows BG0 (the 3D
                 * layer) over a black backdrop */
                gxd_io_write(g, 0x04000304, 0x0207, 2);
                gxd_io_write(g, 0x04000000, 0x00000100, 4);
            }

            for (long k = 0; k < n; k++) {
                long u = units[k];
                char p[1024];
                int do_shot = 0;

                /* 2D state */
                snprintf(p, sizeof(p), "%s/f%05ld.2d", trace_dir, u);
                if (load_2d_container(p) == 0) {
                    memcpy(state_image, state_next, GX2D_STATE_SIZE);
                    gxd_load_2d_state(g, state_image);
                }

                /* 3D traffic */
                if (feed3d == 1) {
                    snprintf(p, sizeof(p), "%s/f%05ld.gx", trace_dir, u);
                    if (feed_gx(g, p) < 0) return 1;

                    snprintf(p, sizeof(p), "%s/f%05ld.texvram", trace_dir, u);
                    {
                        uint8_t *tv = load_file(p, 512 * 1024);
                        if (tv) {
                            uint16_t *tp;
                            snprintf(p, sizeof(p), "%s/f%05ld.texpal",
                                     trace_dir, u);
                            tp = load_file(p, 128 * 1024);
                            gxd_load_tex(g, tv, 512 * 1024, tp, 128 * 1024 / 2);
                            free(tv);
                            free(tp);
                        }
                    }
                    gxd_load_fb3d(g, NULL);
                } else {
                    uint32_t *fb;
                    snprintf(p, sizeof(p), "%s/f%05ld.fb", trace_dir, u);
                    fb = load_file(p, FB_PIXELS * 4);
                    if (fb) {
                        gxd_load_fb3d(g, fb);
                        gxd_frame(g);
                        free(fb);
                        gxd_load_fb3d(g, NULL);
                        goto after_frame;
                    }
                    gxd_load_fb3d(g, NULL);
                }
                gxd_frame(g);
            after_frame:

                if (feed3d == 1) {
                    snprintf(p, sizeof(p), "%s/f%05ld.raw", out_dir, u);
                    {
                        FILE *o = fopen(p, "wb");
                        if (o) {
                            fwrite(gxd_fb3d(g), 4, FB_PIXELS, o);
                            fclose(o);
                        }
                    }
                }

                /* diff vs melonDS golden screens */
                snprintf(p, sizeof(p), "%s/f%05ld.scr", trace_dir, u);
                {
                    uint32_t *scr = load_file(p, FB_PIXELS * 8);
                    if (scr) {
                        int diff0 = 0, diff1 = 0, max0 = 0, max1 = 0;
                        double pct;
                        t.units++;
                        t.pixels += 2LL * FB_PIXELS;
                        diff_buffer(gxd_screen(g, 0), scr, FB_PIXELS,
                                    &t, &diff0, &max0);
                        diff_buffer(gxd_screen(g, 1), scr + FB_PIXELS,
                                    FB_PIXELS, &t, &diff1, &max1);
                        pct = 100.0 * (double)(2 * FB_PIXELS - diff0 - diff1) /
                              (double)(2 * FB_PIXELS);
                        if (!quiet)
                            printf("unit %6ld: diff %5d px (%4d/%4d) max %2d"
                                   "  exact %.2f%%\n",
                                   u, diff0 + diff1, diff0, diff1,
                                   max0 > max1 ? max0 : max1, pct);
                        if (csv)
                            fprintf(csv, "%ld,%d,%d,%.4f\n", u, diff0 + diff1,
                                    max0 > max1 ? max0 : max1, pct);
                        if (want_ppm && (diff0 || diff1)) {
                            snprintf(p, sizeof(p), "%s/f%05ld_s0.ppm",
                                     out_dir, u);
                            gxd_write_screen_ppm(g, 0, p);
                            snprintf(p, sizeof(p), "%s/f%05ld_s1.ppm",
                                     out_dir, u);
                            gxd_write_screen_ppm(g, 1, p);
                        }
                        free(scr);
                    } else {
                        missing_scr++;
                    }
                }

                /* screenshots */
                if (shots_every > 0 && (u % shots_every) == 0) do_shot = 1;
                for (i = 0; i < n_shots; i++)
                    if (shots[i] == u) do_shot = 1;
                if (do_shot) {
                    if (windowed) {
                        snprintf(p, sizeof(p), "%s/shot_%05ld_win.ppm",
                                 out_dir, u);
                        if (gxd_write_window_ppm(g, p) == 0 && !quiet)
                            printf("window screenshot -> %s\n", p);
                    }
                    snprintf(p, sizeof(p), "%s/shot_%05ld_s0.ppm", out_dir, u);
                    gxd_write_screen_ppm(g, 0, p);
                    snprintf(p, sizeof(p), "%s/shot_%05ld_s1.ppm", out_dir, u);
                    gxd_write_screen_ppm(g, 1, p);
                }

                rendered++;
                if (windowed && gxd_should_quit(g)) break;
            }

            if (csv) fclose(csv);

            printf("---- gx_present totals ----\n");
            printf("units rendered: %ld (missing .scr: %ld)  feed3d=%s\n",
                   rendered, missing_scr, feed3d == 1 ? "gx" : "fb");
            if (t.pixels) {
                printf("pixels: %lld  exact: %lld (%.4f%%)  differing: %lld"
                       " (%.4f%%)  max channel delta: %d\n",
                       t.pixels, t.exact,
                       100.0 * (double)t.exact / (double)t.pixels,
                       t.diffpx,
                       100.0 * (double)t.diffpx / (double)t.pixels,
                       t.maxdelta);
            }
            printf("frames presented: %u  unhandled io: %u\n",
                   gxd_frame_count(g), gxd_unhandled_io(g));
            gxd_close(g);
        }
        free(units);
    }
    free(state_image);
    free(state_next);
    return 0;
}
