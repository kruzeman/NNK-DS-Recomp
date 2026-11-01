/* tools/mobiclip_selftest.c — host-side self-test for the in-runtime
 * MobiClip decoder (Phase-5AH).  NOT shipped, NOT linked into the runtime:
 * it exists to prove that tools/nnk_mobiclip.inc + tools/nnk_mobiclip_dec.inc
 * (the code the runtime compiles) decode a real .mods stream identically to
 * FFmpeg's decoder, and to measure decode cost.
 *
 *   build: cc -O2 -std=c11 -I tools tools/mobiclip_selftest.c -o out/mobi_selftest
 *   run:   out/mobi_selftest <file.mods> <out.yuv> <first> <last>
 *
 * The output is raw yuv420p, the same layout `ffmpeg -i file.mods -f rawvideo`
 * produces, so `cmp` / a correlation script can compare the two directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nnk_mobiclip.inc"

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(*n);
    if (!b || fread(b, 1, *n, f) != *n) { free(b); fclose(f); return NULL; }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    NnkMods m;
    NnkMobiDec dec;
    uint8_t *img, *y, *u, *v;
    size_t n;
    int first = 0, last = -1, i, rc;
    long total = 0;
    FILE *out;
    clock_t t0, t1;

    if (argc < 3) { fprintf(stderr, "usage: %s <file.mods> <out.yuv> [first [last]]\n", argv[0]); return 2; }
    if (argc > 3) first = atoi(argv[3]);
    if (argc > 4) last = atoi(argv[4]);
    img = slurp(argv[1], &n);
    if (!img) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    rc = nnk_mods_parse(&m, img, n);
    if (rc < 0) { fprintf(stderr, "mods parse failed rc=%d\n", rc); return 1; }
    printf("mods: %dx%d frames=%d size=%zu first_pkt=%d last_pkt=%d\n",
           m.width, m.height, m.nframes, n, m.fsize[0], m.fsize[m.nframes - 1]);
    if (last < 0 || last >= m.nframes) last = m.nframes - 1;
    if (nnk_mobi_open(&dec, m.width, m.height) < 0) { fprintf(stderr, "open failed\n"); return 1; }
    y = (uint8_t *)malloc((size_t)m.width * m.height);
    u = (uint8_t *)malloc((size_t)m.width * m.height / 4);
    v = (uint8_t *)malloc((size_t)m.width * m.height / 4);
    out = fopen(argv[2], "wb");
    t0 = clock();
    for (i = first; i <= last; i++) {
        rc = nnk_mobi_frame(&dec, &m, i, y, u, v);
        if (rc < 0) { fprintf(stderr, "frame %d failed rc=%d\n", i, rc); return 1; }
        if (first == 0 || i > first) { /* keep output aligned with first */ }
        fwrite(y, 1, (size_t)m.width * m.height, out);
        fwrite(u, 1, (size_t)m.width * m.height / 4, out);
        fwrite(v, 1, (size_t)m.width * m.height / 4, out);
        total += m.fsize[i];
    }
    t1 = clock();
    fclose(out);
    printf("decoded %d frames (bytes=%ld) in %.3f s -> %.2f ms/frame, decoded_total=%ld\n",
           last - first + 1, total, (double)(t1 - t0) / CLOCKS_PER_SEC,
           1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / (double)(last - first + 1),
           dec.decoded);
    nnk_mobi_close(&dec);
    nnk_mods_free(&m);
    free(img); free(y); free(u); free(v);
    return 0;
}
