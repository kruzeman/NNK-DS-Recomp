/* ==========================================================================
 * nnk_mobiclip.inc — in-process MobiClip decoder + MODS container demuxer.
 *
 * Phase-5AH.  WHY: the movie HLE used to take its picture from
 * NNK_MOVIE_YUV / NNK_MOVIE_YUV2, i.e. from raw yuv420p dumps that only
 * existed on the build host.  On any other machine the HLE switched itself
 * off and the guest's own (unfixed) decoder painted macroblock garbage.
 * The runtime now decodes the picture itself, straight out of the ROM's
 * `data/movie/*.mods`, so `NNK_ROM_PATH` is the only input.
 *
 * This file is the SELF-CONTAINED support layer:
 *   1. a minimal stand-in for the little bit of FFmpeg's runtime the
 *      decoder touches (bit reader, exp-Golomb, canonical-VLC decode,
 *      frame pool) — no libav* is linked, no external process is spawned;
 *   2. the MODS container demuxer (52-byte header + per-frame records);
 *   3. the decode API the movie HLE calls (nnk_mobi_frame()).
 *
 * The codec body itself is nnk_mobiclip_dec.inc, a lightly adapted copy of
 * FFmpeg's libavcodec/mobiclip.c (LGPL-2.1+, authors credited there).  It is
 * compiled into the runtime TU; nothing is loaded at run time.
 *
 * MEASURED CONTAINER (op_u.mods / op_d.mods / s01..s14):
 *   0x00 "MODS"   0x04 "N3\n\0"   0x08 frames   0x0C width   0x10 height
 *   0x14..0x33 more header words (0x14 == 12 fps as a big-endian u32,
 *   0x20 == first-frame area size, 0x28 == data end, 0x2C == 14)
 *   0x34 first frame record.  A record is [u32 LE: length in 16-bit units,
 *   length = words << 16][payload], and consecutive records are separated by
 *   4 bytes of padding.  Verified against ffprobe packet pos/size for all
 *   1317 frames of op_u.mods (tools/mobiclip/mobi_selftest.c).
 * ========================================================================== */
#ifndef NNK_MOBICLIP_INC
#define NNK_MOBICLIP_INC

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

/* ---------------------------------------------------------------- compat -- */
#define av_cold
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
typedef int AVOnce;
#define AV_ONCE_INIT 0
#define AVERROR(e) (-(e))
#define AVERROR_INVALIDDATA (-1094995529)
#define AVERROR_ENOMEM (-12)
/* Phase-5AH: a rejected frame must say WHERE it was rejected — the runtime
 * only ever sees "this movie did not decode". */
#define MOBI_EINVAL() (fprintf(stderr, "mobiclip: invalid data at %s:%d\n", __func__, __LINE__), AVERROR_INVALIDDATA)

#define AV_PICTURE_TYPE_I 1
#define AV_PICTURE_TYPE_P 2
#define AV_FRAME_FLAG_KEY (1 << 1)
#define AVCOL_SPC_YCGCO 8
#define AVCOL_SPC_BT470BG 5
#define AV_PIX_FMT_YUV420P 0
#define AV_LOG_ERROR 16

#define av_assert0(x) do { if (!(x)) { fprintf(stderr, "mobiclip: assert %s\n", #x); abort(); } } while (0)

typedef struct AVFrame {
    uint8_t *data[4];
    int      linesize[4];
    int      width, height;
    int      pict_type;
    int      flags;
    int      _own[4];
} AVFrame;

typedef struct AVCodecContext {
    void *priv_data;
    int   width, height;
    int   pix_fmt;
    int   colorspace;
} AVCodecContext;

typedef struct AVPacket {
    uint8_t *data;
    int      size;
} AVPacket;

typedef struct BswapDSPContext {
    void (*bswap16_buf)(uint16_t *dst, const uint16_t *src, int len);
} BswapDSPContext;

static void nnk_mobi_bswap16_buf(uint16_t *dst, const uint16_t *src, int len) {
    int i;
    for (i = 0; i < len; i++) { unsigned v = src[i]; dst[i] = (uint16_t)((v >> 8) | (v << 8)); }
}
static void ff_bswapdsp_init(BswapDSPContext *c) { c->bswap16_buf = nnk_mobi_bswap16_buf; }

/* ---- bit reader: MSB-first over a byte buffer, zero past the end ------- */
typedef struct GetBitContext {
    const uint8_t *buf;
    int size_bits;
    int pos;
} GetBitContext;

static int init_get_bits8(GetBitContext *gb, const uint8_t *buf, int size_bytes) {
    if (size_bytes < 0 || size_bytes > (1 << 28)) return AVERROR_INVALIDDATA;
    gb->buf = buf;
    gb->size_bits = size_bytes * 8;
    gb->pos = 0;
    return 0;
}
static int get_bits_left(GetBitContext *gb) { return gb->size_bits - gb->pos; }

static unsigned get_bits(GetBitContext *gb, int n) {
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++) {
        int p = gb->pos;
        unsigned bit = 0;
        if (p < gb->size_bits) bit = (unsigned)((gb->buf[p >> 3] >> (7 - (p & 7))) & 1);
        gb->pos = p + 1;
        v = (v << 1) | bit;
    }
    return v;
}
static unsigned get_bits1(GetBitContext *gb) { return get_bits(gb, 1); }
/* sign-extended n-bit read (FFmpeg get_sbits) */
static int get_sbits(GetBitContext *gb, int n) {
    unsigned v = get_bits(gb, n);
    if (n > 0 && n < 32 && (v & (1u << (n - 1)))) v |= ~0u << n;
    return (int)v;
}

/* ---- exp-Golomb (same value mapping as FFmpeg's golomb.h) -------------- */
static unsigned get_ue_golomb(GetBitContext *gb) {
    int zeros = 0;
    while (zeros < 32 && !get_bits1(gb)) zeros++;
    if (zeros >= 32) return 0;
    return ((1u << zeros) - 1u) + get_bits(gb, zeros);
}
static unsigned get_ue_golomb_31(GetBitContext *gb) {
    int zeros = 0;
    while (zeros < 32 && !get_bits1(gb)) zeros++;
    if (zeros >= 32) return 0;
    return ((1u << zeros) - 1u) + get_bits(gb, zeros);
}
/* FFmpeg's mapping (golomb.h get_se_golomb / ff_se_golomb_vlc_code):
 *   k odd  -> +(k+1)/2      k even -> -k/2
 * i.e. 0->0, 1->+1, 2->-1, 3->+2, 4->-2.  (Verified against the reference
 * tables: prefix '01' (k=1) is +1, prefix '011' (k=4) is -2.) */
static int get_se_golomb(GetBitContext *gb) {
    int k = (int)get_ue_golomb(gb);
#ifdef NNK_MOBI_SE_ALT
    if (k & 1) return -(k + 1) >> 1;
    return k >> 1;
#else
    if (k & 1) return (k + 1) >> 1;
    return -(k >> 1);
#endif
}

/* ---- VLC (replaces FFmpeg's table builder + get_vlc2) ------------------
 * FFmpeg's ff_vlc_init_from_lengths() assigns codes in ARRAY ORDER: it keeps
 * a 32-bit accumulator `code` and, for every entry i with lens[i] > 0, gives
 * that symbol the top lens[i] bits of `code`, then advances
 * code += 1 << (32 - lens[i]) (a negative length consumes bits without
 * emitting a symbol, a zero length consumes nothing).  The result is a
 * prefix code, so decoding is a plain bit-wise walk of a binary tree.  This
 * is a bit-serial reader (movies are 12 fps; the cost is irrelevant next to
 * the guest emulation) instead of a 12-bit lookup table. */
#define MOBI_VLC_NODES 2048
typedef struct MobiVLC {
    int      nnodes;
    int16_t  child[MOBI_VLC_NODES][2];
    uint16_t sym[MOBI_VLC_NODES];        /* symbol + 1, 0 = not a leaf */
} MobiVLC;

static void mobi_vlc_init(MobiVLC *v, int n, const uint8_t *lens, int lens_stride,
                          const void *syms, int sym_size) {
    uint64_t code = 0;
    const uint8_t *sp = (const uint8_t *)syms;
    int i;
    memset(v, 0, sizeof *v);
    v->nnodes = 1;
    for (i = 0; i < n; i++) {
        int len = (int)lens[(size_t)i * (size_t)lens_stride];
        int node = 0, b;
        if (len > 0) {
            unsigned sym;
            uint32_t cw;
            const uint8_t *e = sp + (size_t)i * (size_t)sym_size;
            if (sym_size == 1)      sym = e[0];
            else if (sym_size == 2) sym = (unsigned)(e[0] | (e[1] << 8));
            else                    sym = (unsigned)(e[0] | (e[1] << 8) | (e[2] << 16) | ((unsigned)e[3] << 24));
            cw = (uint32_t)(code >> (32 - len));
            for (b = len - 1; b >= 0; b--) {
                int bit = (int)((cw >> b) & 1u);
                if (!v->child[node][bit]) {
                    if (v->nnodes >= MOBI_VLC_NODES) return;   /* table too big */
                    v->child[node][bit] = (int16_t)v->nnodes;
                    v->nnodes++;
                }
                node = v->child[node][bit];
            }
            v->sym[node] = (uint16_t)(sym + 1u);
        } else if (len == 0) {
            continue;
        } else {
            len = -len;
        }
        code += (uint64_t)1 << (32 - len);
    }
}
/* ff_vlc_init_tables_from_lengths() compatible signature for the codec body */
typedef struct VLCInitState { int dummy; } VLCInitState;
#define VLC_INIT_STATE(b) ((VLCInitState){0})
static MobiVLC *ff_vlc_init_tables_from_lengths(VLCInitState *st, int bits, int nb_codes,
                                                const void *lens, int lens_wrap,
                                                const void *syms_in, int sym_wrap,
                                                int sym_size, int flags, int flags2) {
    static MobiVLC pool[2 + 2 * 16];
    static int used;
    MobiVLC *v;
    (void)st; (void)bits; (void)flags; (void)flags2; (void)sym_wrap;
    if (used >= 2 + 2 * 16) return NULL;
    v = &pool[used++];
    mobi_vlc_init(v, nb_codes, (const uint8_t *)lens, lens_wrap, syms_in, sym_size);
    return v;
}
static int get_vlc2(GetBitContext *gb, const MobiVLC *v, int bits, int max_depth) {
    int node = 0, guard = 0;
    (void)bits; (void)max_depth;
    while (node >= 0 && !v->sym[node] && guard++ < 32) {
        int bit = (int)get_bits1(gb);
        node = v->child[node][bit];
    }
    if (node <= 0 || !v->sym[node]) return -1;
    return (int)v->sym[node] - 1;
}

/* ---- small math helpers (FFmpeg mathops.h equivalents) ----------------- */
static int av_clip(int a, int amin, int amax) {
    if (a < amin) return amin;
    if (a > amax) return amax;
    return a;
}
static unsigned av_clip_uint8(int a) {
    if (a & (~0xFF)) return (unsigned)((~a) >> 31);
    return (unsigned)a;
}
/* FFmpeg mathops.h mid_pred() — the motion-vector median predictor. */
static int mid_pred(int a, int b, int c) {
    if (a > b) {
        if (c > b) {
            if (a > c) b = c;
            else       b = a;
        }
    } else {
        if (b > c) {
            if (c > a) b = c;
            else       b = a;
        }
    }
    return b;
}
static const uint8_t ff_zigzag_direct[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* ---- memory / frame-pool stubs ---------------------------------------- */
#define PLANE_PAD 64
static void *av_calloc(size_t n, size_t s)          { return calloc(n, s); }
static void  av_freep(void *p)                      { void **pp = (void **)p; free(*pp); *pp = NULL; }
static void  av_log(void *a, int l, const char *fmt, ...) { (void)a; (void)l; (void)fmt; }
static void  ff_thread_once(int *once, void (*fn)(void)) { if (!*once) { *once = 1; fn(); } }

static AVFrame *av_frame_alloc(void) { return (AVFrame *)calloc(1, sizeof(AVFrame)); }

static void av_frame_unref(AVFrame *f) {
    int i;
    if (!f) return;
    for (i = 0; i < 4; i++) {
        if (f->_own[i]) free(f->data[i]);
        f->data[i] = NULL;
        f->linesize[i] = 0;
        f->_own[i] = 0;
    }
    f->width = f->height = 0;
    f->pict_type = 0;
    f->flags = 0;
}

static void av_frame_free(AVFrame **pf) {
    if (pf && *pf) { av_frame_unref(*pf); free(*pf); *pf = NULL; }
}

static int ff_reget_buffer(AVCodecContext *avctx, AVFrame *f, int flags) {
    int w = avctx->width, h = avctx->height, cw = w / 2, ch = h / 2;
    (void)flags;
    if (f->data[0] && f->width == w && f->height == h) return 0;
    av_frame_unref(f);
    f->width = w; f->height = h;
    f->linesize[0] = w; f->linesize[1] = cw; f->linesize[2] = cw;
    f->data[0] = (uint8_t *)calloc(1, (size_t)w * h + 2 * PLANE_PAD);     f->_own[0] = 1;
    f->data[1] = (uint8_t *)calloc(1, (size_t)cw * ch + 2 * PLANE_PAD);   f->_own[1] = 1;
    f->data[2] = (uint8_t *)calloc(1, (size_t)cw * ch + 2 * PLANE_PAD);   f->_own[2] = 1;
    if (!f->data[0] || !f->data[1] || !f->data[2]) return AVERROR_ENOMEM;
    return 0;
}

/* Shallow reference: the decoder owns the plane until that pool slot is
 * reused six frames later, and the caller copies it out before the next
 * decode, so no copy is needed here. */
static int av_frame_ref(AVFrame *dst, const AVFrame *src) {
    int i;
    av_frame_unref(dst);
    dst->width = src->width; dst->height = src->height;
    dst->pict_type = src->pict_type; dst->flags = src->flags;
    for (i = 0; i < 3; i++) {
        dst->data[i]     = src->data[i];
        dst->linesize[i] = src->linesize[i];
        dst->_own[i]     = 0;
    }
    return 0;
}

static void av_fast_padded_malloc(void *ptr, unsigned *size, size_t min_size) {
    uint8_t **p = (uint8_t **)ptr;
    size_t need = min_size + 2 * PLANE_PAD;
    if (*p && *size >= need) return;
    free(*p);
    *p = (uint8_t *)calloc(1, need);
    *size = (unsigned)(*p ? need : 0);
}

/* --------------------------------------------------------- MODS demuxer -- */
#define NNK_MODS_HDR 52

typedef struct NnkMods {
    const uint8_t *data;
    size_t         size;
    int            nframes, width, height;
    const uint8_t **frame;     /* packet pointer per frame (first byte of rec) */
    int            *fsize;     /* packet length in bytes */
} NnkMods;

static uint32_t nnk_mobi_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void nnk_mods_free(NnkMods *m) {
    free((void *)m->frame); free(m->fsize);
    memset(m, 0, sizeof *m);
}

/* Parse a MODS container from an in-memory image (the ROM slice).
 * Returns 0 on success, <0 when the container is not understood. */
static int nnk_mods_parse(NnkMods *m, const uint8_t *d, size_t n) {
    int i, nf;
    size_t pos;
    memset(m, 0, sizeof *m);
    if (!d || n < NNK_MODS_HDR + 4) return -1;
    if (memcmp(d, "MODS", 4) != 0) return -2;
    nf = (int)nnk_mobi_rd32(d + 8);
    m->width  = (int)nnk_mobi_rd32(d + 12);
    m->height = (int)nnk_mobi_rd32(d + 16);
    if (nf <= 0 || nf > 100000 || m->width <= 0 || m->height <= 0) return -3;
    if (m->width > 512 || m->height > 512) return -4;
    m->data = d; m->size = n; m->nframes = nf;
    m->frame = (const uint8_t **)calloc((size_t)nf, sizeof *m->frame);
    m->fsize = (int *)calloc((size_t)nf, sizeof *m->fsize);
    if (!m->frame || !m->fsize) { nnk_mods_free(m); return -5; }
    pos = NNK_MODS_HDR;
    for (i = 0; i < nf; i++) {
        uint32_t hdr;
        unsigned words, sz;
        if (pos + 4 > n) { nnk_mods_free(m); return -6; }
        hdr = nnk_mobi_rd32(d + pos);
        words = (unsigned)((hdr >> 16) & 0xFFFFu);
        sz = words * 4u;
        if (sz < 4u || pos + 4u + sz > n) { nnk_mods_free(m); return -7; }
        m->frame[i] = d + pos + 4;          /* payload follows the length word */
        m->fsize[i] = (int)sz;              /* ... and is `sz` bytes long        */
        pos += sz + 4u;                     /* record = 4-byte word + sz payload */
    }
    return 0;
}

/* ------------------------------------------------- ROM file lookup (FAT) ---
 * The movie containers live in the card image at data/movie/*.mods.  The
 * runtime already holds the whole ROM (`rom`/`rom_len`, the card model), so a
 * path lookup is a NITRO FNT walk plus the FAT entry — no guest FS call, no
 * file outside the image.  Path is ASCII, '/'-separated, case-insensitive. */
static int nnk_rom_ieq(const char *a, const char *b, int n) {
    int i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return 1;
}

/* Find one path component inside directory `dir` of the FNT.
 * Returns the file id (>= 0), the child directory id (sets *isdir), or -1. */
static int nnk_fnt_lookup(const uint8_t *rom, uint32_t fnt_off, uint32_t fnt_size,
                          int dir, const char *name, int namelen, int *isdir) {
    uint32_t ent_off, p, end;
    uint16_t first_id;
    if (dir < 0 || (size_t)fnt_off + 8u * ((size_t)dir + 1u) > (size_t)fnt_off + (size_t)fnt_size) return -1;
    ent_off = nnk_mobi_rd32(rom + fnt_off + 8u * (uint32_t)dir);
    first_id = (uint16_t)(rom[fnt_off + 8u * (uint32_t)dir + 4] |
                          (rom[fnt_off + 8u * (uint32_t)dir + 5] << 8));
    p = fnt_off + ent_off;
    end = fnt_off + fnt_size;
    if (p >= end) return -1;
    for (;;) {
        uint8_t ctrl;
        int len;
        if (p >= end) return -1;
        ctrl = rom[p++];
        if (ctrl == 0) return -1;
        len = ctrl & 0x7F;
        if (p + (uint32_t)len > end) return -1;
        /* NITRO FNT: bit7 SET marks a DIRECTORY entry (the ROM's own root
         * listing proves it: "data" is 0x84 = dir, 4 chars); the two bytes
         * after the name are the sub-directory id.  Bit7 clear = file, and
         * the entry consumes the next file id. */
        if (ctrl & 0x80) {                       /* directory entry */
            if (len == namelen && nnk_rom_ieq((const char *)rom + p, name, namelen)) {
                int did;
                if (p + (uint32_t)len + 2u > end) return -1;
                did = rom[p + (uint32_t)len] | (rom[p + (uint32_t)len + 1] << 8);
                *isdir = 1;
                return did & 0x0FFF;
            }
        } else {                                 /* file entry */
            if (len == namelen && nnk_rom_ieq((const char *)rom + p, name, namelen)) {
                *isdir = 0;
                return (int)first_id;
            }
            first_id++;
        }
        p += (uint32_t)len;
        if (ctrl & 0x80) p += 2u;                 /* directory id follows */
    }
}

/* Resolve `path` inside the card image.  Returns 0 and sets *out/*outlen when
 * the file exists; the returned pointer is inside `rom` (no copy). */
static int nnk_rom_file(const uint8_t *rom, size_t rom_len, const char *path,
                        const uint8_t **out, size_t *outlen) {
    uint32_t fnt_off, fnt_size, fat_off;
    int dir = 0;
    const char *s = path;
    if (!rom || rom_len < 0x60) return -1;
    if (memcmp(rom, "NTR-", 4) != 0 && memcmp(rom + 0x0C, "01", 2) != 0) {
        /* not fatal: the offsets below are what matter */
    }
    fnt_off  = nnk_mobi_rd32(rom + 0x040);
    fnt_size = nnk_mobi_rd32(rom + 0x044);
    fat_off  = nnk_mobi_rd32(rom + 0x048);
    if ((size_t)fnt_off + fnt_size > rom_len || (size_t)fat_off + 8u > rom_len) return -2;
    while (*s == '/') s++;
    for (;;) {
        const char *e = s;
        int isdir = 0, id, last;
        while (*e && *e != '/') e++;
        if (e == s) return -3;
        last = (*e == 0);
        id = nnk_fnt_lookup(rom, fnt_off, fnt_size, dir, s, (int)(e - s), &isdir);
        if (id < 0) return -4;
        if (!last) {
            if (!isdir) return -5;
            dir = id;
            s = e + 1;
            continue;
        }
        if (isdir) return -6;
        {
            uint32_t a, b;
            if ((size_t)fat_off + 8u * (uint32_t)id + 8u > rom_len) return -7;
            a = nnk_mobi_rd32(rom + fat_off + 8u * (uint32_t)id);
            b = nnk_mobi_rd32(rom + fat_off + 8u * (uint32_t)id + 4u);
            if (b <= a || (size_t)b > rom_len) return -8;
            *out = rom + a;
            *outlen = (size_t)(b - a);
        }
        return 0;
    }
}

/* ------------------------------------------ FNT directory walk (movie dir) --
 * Phase-5BD: the HLE must bind a stream to the container the GUEST opened,
 * not to the order in which codecs were registered.  The guest's own copy of
 * the container header is the identity: the MobiClip player object embeds the
 * first 48 bytes of the MODS header of the file it loaded (measured at
 * 0x022C9E54, i.e. player+4, in the in-game cutscene of Phase-5BC:
 * "MODS","N3\n\0",581,256x192, payload words 0x36a8/0x12301c/1 ==
 * data/movie/s01.mods byte for byte).  The 52-byte header's last word is the
 * FIRST FRAME RECORD LENGTH, which the player object overwrites with its codec
 * pointer, so identity is the 48 bytes before it.
 * These helpers enumerate `data/movie/` in the Nitro FNT and compare that
 * header against every container the card image really has. */
#define NNK_MODS_IDENT 48

/* Iterate one directory.  `idx` is the 0-based entry index; returns 1 and
 * fills name/id/isdir while entries remain, 0 at the terminator, -1 on a
 * malformed table.  File ids come from the directory's own first-file id
 * (Nitro FNT: u32 subtable offset, u16 first file id, u16 parent dir id), so
 * they are exact without a global counter. */
static int nnk_fnt_dir_at(const uint8_t *rom, uint32_t fnt_off, uint32_t fnt_size,
                          int did, int idx, char *name, int cap, int *fid, int *isdir) {
    uint32_t ent_off, p, end;
    uint16_t first_id;
    int seen = 0;
    if (did < 0 || (size_t)fnt_off + 8u * ((size_t)did + 1u) > (size_t)fnt_off + (size_t)fnt_size) return -1;
    ent_off = nnk_mobi_rd32(rom + fnt_off + 8u * (uint32_t)did);
    first_id = (uint16_t)(rom[fnt_off + 8u * (uint32_t)did + 4] |
                          (rom[fnt_off + 8u * (uint32_t)did + 5] << 8));
    p = fnt_off + ent_off;
    end = fnt_off + fnt_size;
    while (p < end) {
        uint8_t ctrl = rom[p++];
        int len, n;
        if (ctrl == 0) return 0;
        len = ctrl & 0x7F;
        if (p + (uint32_t)len > end || len >= cap) return -1;
        if (seen == idx) {
            n = len < cap - 1 ? len : cap - 1;
            memcpy(name, rom + p, (size_t)n);
            name[n] = 0;
            *isdir = (ctrl & 0x80) ? 1 : 0;
            if (!*isdir) *fid = (int)first_id;
            p += (uint32_t)len;
            if (ctrl & 0x80) {
                if (p + 2u > end) return -1;
                *fid = rom[p] | (rom[p + 1] << 8);
            }
            return 1;
        }
        p += (uint32_t)len;
        if (ctrl & 0x80) {
            if (p + 2u > end) return -1;
            p += 2u;
        } else {
            first_id++;
        }
        seen++;
    }
    return -1;
}

/* Resolve a directory path ("data/movie") to its FNT directory id. */
static int nnk_fnt_dir_find(const uint8_t *rom, uint32_t fnt_off, uint32_t fnt_size,
                            const char *path, int *out) {
    int dir = 0, isdir = 0, id;
    const char *s = path;
    while (*s == '/') s++;
    if (!*s) { *out = 0; return 0; }
    for (;;) {
        const char *e = s;
        while (*e && *e != '/') e++;
        id = nnk_fnt_lookup(rom, fnt_off, fnt_size, dir, s, (int)(e - s), &isdir);
        if (id < 0 || !isdir) return -1;
        dir = id;
        if (!*e) { *out = dir; return 0; }
        s = e + 1;
    }
}

static int nnk_mobi_ends_ieq(const char *s, const char *suf) {
    int ls = 0, lf = 0, i;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (i = 0; i < lf; i++)
        if (nnk_rom_ieq(s + ls - lf + i, suf + i, 1) != 1) return 0;
    return 1;
}

/* Match the guest's 48-byte MODS header against the containers the card image
 * actually contains under `data/movie/`.  Returns the number of matches;
 * on the first match *outpath receives "data/movie/<name>" and *nf the frame
 * count from that container's own header.  A miss or an ambiguous hit is the
 * caller's business — this function never picks a substitute. */
static int nnk_rom_movie_match(const uint8_t *rom, size_t rom_len, const uint8_t *hdr,
                               char *outpath, size_t outcap, int *nf) {
    uint32_t fnt_off, fnt_size, fat_off;
    int did, idx, fid = 0, isdir = 0, nmatch = 0;
    char name[96];
    if (!rom || rom_len < 0x60 || !hdr) return -1;
    fnt_off  = nnk_mobi_rd32(rom + 0x040);
    fnt_size = nnk_mobi_rd32(rom + 0x044);
    fat_off  = nnk_mobi_rd32(rom + 0x048);
    if ((size_t)fnt_off + fnt_size > rom_len) return -1;
    if (nnk_fnt_dir_find(rom, fnt_off, fnt_size, "data/movie", &did) < 0) return -1;
    for (idx = 0; nnk_fnt_dir_at(rom, fnt_off, fnt_size, did, idx, name, sizeof name,
                                 &fid, &isdir) == 1; idx++) {
        uint32_t a, b;
        if (isdir || !nnk_mobi_ends_ieq(name, ".mods")) continue;
        if ((size_t)fat_off + 8u * (uint32_t)fid + 8u > rom_len) continue;
        a = nnk_mobi_rd32(rom + fat_off + 8u * (uint32_t)fid);
        b = nnk_mobi_rd32(rom + fat_off + 8u * (uint32_t)fid + 4u);
        if (b <= a || (size_t)b > rom_len || (size_t)(b - a) < NNK_MODS_HDR) continue;
        if (memcmp(rom + a, hdr, NNK_MODS_IDENT) != 0) continue;
        nmatch++;
        if (nmatch == 1) {
            snprintf(outpath, outcap, "data/movie/%s", name);
            *nf = (int)nnk_mobi_rd32(rom + a + 8u);
        } else {
            fprintf(stderr, "movie: identity ALSO matches data/movie/%s (ambiguous)\n", name);
        }
    }
    return nmatch;
}

/* CRC-32 (bitwise; only used by the NNK_MOVIE_TRACE diagnostic). */
static uint32_t nnk_mobi_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

#include "nnk_mobiclip_dec.inc"

/* ------------------------------------------------------------- decode API --
 * One NnkMobiDec per movie stream.  Frames are decoded strictly in order
 * (MobiClip is inter-frame: a P frame predicts from up to six earlier
 * pictures), so presentation of frame N means "decode frames next..N".
 * A backwards request flushes and restarts at frame 0 — the runtime only
 * ever asks for 0,1,2,... within a session, so that path is the session
 * restart. */

typedef struct NnkMobiDec {
    int              opened;
    AVCodecContext   avctx;
    MobiClipContext *s;
    AVFrame          out;      /* shallow ref to the last decoded picture */
    int              next;     /* next frame index to decode */
    long             decoded;  /* frames decoded since open (measurement) */
    int              w, h;
} NnkMobiDec;

static int nnk_mobi_open(NnkMobiDec *d, int w, int h) {
    memset(d, 0, sizeof *d);
    if (w <= 0 || h <= 0 || (w & 15) || (h & 15)) return -1;
    d->avctx.width = w;
    d->avctx.height = h;
    d->s = (MobiClipContext *)calloc(1, sizeof(MobiClipContext));
    if (!d->s) return -2;
    d->avctx.priv_data = d->s;
    if (mobiclip_init(&d->avctx) < 0) { free(d->s); d->s = NULL; return -3; }
    d->w = w; d->h = h;
    d->next = 0;
    d->opened = 1;
    return 0;
}

static void nnk_mobi_close(NnkMobiDec *d) {
    if (!d->opened) return;
    mobiclip_close(&d->avctx);
    free(d->s);
    memset(d, 0, sizeof *d);
}

/* Decode up to frame `idx` and copy its three planes into the caller's
 * buffers (Y w*h, U w/2*h/2, V w/2*h/2, tightly packed).  Any of the plane
 * pointers may be NULL when that plane is not needed. */
static int nnk_mobi_frame(NnkMobiDec *d, const NnkMods *m, int idx,
                          uint8_t *y, uint8_t *u, uint8_t *v) {
    AVPacket pkt;
    int got, ret, i;
    if (!d->opened || !m || m->nframes <= 0) return -1;
    if (m->width != d->w || m->height != d->h) return -2;
    if (idx < 0) idx = 0;
    if (idx >= m->nframes) idx = m->nframes - 1;
    if (idx < d->next) {                       /* session restart */
        mobiclip_flush(&d->avctx);
        d->next = 0;
    }
    while (d->next <= idx) {
        pkt.data = (uint8_t *)m->frame[d->next];
        pkt.size = m->fsize[d->next];
        got = 0;
        av_frame_unref(&d->out);
        ret = mobiclip_decode(&d->avctx, &d->out, &got, &pkt);
        if (ret < 0 || !got) {
            fprintf(stderr, "mobiclip: decode failed at frame %d/%d ret=%d got=%d\n",
                    d->next, m->nframes, ret, got);
            return -3;
        }
        d->next++;
        d->decoded++;
    }
    if (!d->out.data[0]) return -4;
    if (y) for (i = 0; i < d->h; i++) memcpy(y + (size_t)i * d->w,
                                            d->out.data[0] + (size_t)i * d->out.linesize[0], (size_t)d->w);
    if (u) for (i = 0; i < d->h / 2; i++) memcpy(u + (size_t)i * (d->w / 2),
                                                 d->out.data[1] + (size_t)i * d->out.linesize[1], (size_t)d->w / 2);
    if (v) for (i = 0; i < d->h / 2; i++) memcpy(v + (size_t)i * (d->w / 2),
                                                 d->out.data[2] + (size_t)i * d->out.linesize[2], (size_t)d->w / 2);
    return 0;
}

#endif /* NNK_MOBICLIP_INC */
