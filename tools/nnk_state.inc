/* tools/nnk_state.inc — Phase-5AQ: savestates (F5 save / F9 load).
 *
 * Spliced into nnk_main.c by tools/split_dispatch.py right after the other
 * runtime fragments, so it sees every runtime static: the CPU register file
 * and banked r13/r14/SPSR, the guest memory window, the IO/PPU/VRAMCNT model,
 * the card/timer/DMA/IPC shims, the SMC generation + band tables, the input
 * script cursor, the movie HLE cursors — and the display model through the
 * gxd_state_* snapshot the renderer gained for this phase.
 *
 * WHAT IS CARRIED
 *   The field table is GENERATED from the effective prelude at build time
 *   (tools/nnks_gen_state_fields.py -> nnk_state_fields.h), so a static added
 *   by a later phase is carried automatically instead of silently diverging
 *   a restored run.  Region list (each with offset+length+CRC32 in the file):
 *     io        CPU + flags + banked regs + CP15 + counters, DISPCNT/DISPSTAT,
 *               IE/IF/IME, timers, DMA, EXMEMCNT/POWCNT, DIV/SQRT, VRAMCNT,
 *               IRQ script cursor, ARM7 IPC sync/FIFO/event state, gba_openbus
 *     smc       generation/band dirty tables, per-band generations, re-decode
 *               counters, sticky T-bit state, conflict-detection counters
 *     card      card command latch, address/length/position/FIFO, scheduling
 *               clock, SPI/ROM control and the DRQ/poll bookkeeping
 *     input     effective key mask + EXTKEYIN, the parsed frame script and its
 *               cursor, the injection schedule and the recorder counters
 *     movie     the movie HLE stream count/codec cursor
 *     moviestreams  per-stream {codec,cursor,index,frames,presents,shortr,rom}
 *     display   gxd frame counter + last VCOUNT edge
 *     gxblob    the whole display model: VRAM banks, 2D register image, palette,
 *               OAM, latched 3D env, retained polygon lists, composed screens
 *     ram       the 24 MiB guest window (main RAM 4 MiB + the mirror + the
 *               shared WRAM at 0x03000000, exactly the regions melonDS zeroes)
 *     itcm/dtcm the TCM images
 *
 * NOT CARRIED (host-owned; rebound or re-derived on load — a state must never
 * contain a host address, or it would break under ASLR / a new process):
 *   gxd (SDL/GL handle), gxd_shot, rom, smc_img, smc_seg[].data,
 *   smc_seen_key/smc_seen_form, nnk_in_log, nnk_rec, storewatch_fp,
 *   p5t_str[].fp, p5t_str[].dec (Mobiclip reference frames — re-decoded from
 *   frame 0 on load, which is deterministic), p5t_buf (decode scratch).
 *   Run configuration (NNK_STEP_LIMIT, probe/trace addresses, shot cadence) is
 *   deliberately NOT restored: it belongs to the running process, not the
 *   emulated machine.
 *
 * FILE FORMAT (little-endian, SAVESTATES.md has the byte layout)
 *   "NNKS" | version | header_size | region_count | build_fp |
 *   rom_size u64 | rom_hdr_crc | arm9_crc | record_step u64 |
 *   record_frames u64 | payload_size u64 | payload_crc | header_crc
 *   region_count x { name[16] | offset u64 | length u64 | crc32 | rsv }
 *   payload
 *   The identity block (rom_size, rom_hdr_crc, arm9_crc, build_fp) is checked
 *   BEFORE anything is applied; a mismatch is refused with a message naming
 *   the field, never applied silently.  header_crc covers the header bytes
 *   before it, payload_crc covers every region byte, each region carries its
 *   own CRC32.
 *
 * ENVIRONMENT
 *   NNK_SAVESTATE=<path>          boot straight from a state (no boot replay)
 *   NNK_SAVESTATE_DIR=<dir>       default state directory (else dirname(argv[0]))
 *   NNK_SAVESTATE_SAVE_AT=<step>  write a state at that step (and KEEP RUNNING;
 *                                 the run's own step limit still ends it)
 *   NNK_SAVESTATE_SAVE=<path>     path for SAVE_AT (else <dir>/nnk_save1.nnks)
 *   NNK_STATE_HASH_AT=<step>      print the canonical state hash at that step
 *   NNK_GX=window: F5 saves, F9 loads (path printed on both)
 */
#ifndef NNK_STATE_INC
#define NNK_STATE_INC

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NNKS_VERSION 1u
#define NNKS_PATH_MAX 512
#define NNKS_MAXREGIONS 16

/* ---- checksums ---------------------------------------------------------- */

static uint32_t nnks_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t nnks_fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ---- field table (generated) -------------------------------------------- */

typedef struct {
    const char *name;
    uint32_t len;
    uint8_t *p;
} NnkStateField;

#define NNKS_F(x) { #x, (uint32_t)sizeof(x), (uint8_t *)&(x) }

/* The PC the dispatch loop is about to execute.  It is a local in run(), so
 * the per-step hook mirrors it here (nnks_tick(pc)) and run() takes its entry
 * PC from nnks_entry_pc() — that is what lets a restored state CONTINUE at the
 * saved instruction instead of restarting at ENTRY_CONST. */
static uint32_t nnks_pc = ENTRY_CONST;

#include "nnk_state_fields.h"

/* ---- movie stream cursors ------------------------------------------------ */

typedef struct {
    uint32_t codec, cursor, rom;
    int32_t index, frames, presents, shortr;
} NnkStateMovie;

/* ---- container ---------------------------------------------------------- */

typedef struct {
    char     magic[4];       /* "NNKS" */
    uint32_t version;
    uint32_t header_size;    /* sizeof(NnkStateHdr) + region_count * sizeof(NnkStateRegion) */
    uint32_t region_count;
    uint32_t build_fp;
    uint64_t rom_size;
    uint32_t rom_hdr_crc;
    uint32_t arm9_crc;
    uint64_t record_step;
    uint64_t record_frames;
    uint64_t payload_size;
    uint32_t payload_crc;
    uint32_t header_crc;     /* over every byte of the header before this field */
} NnkStateHdr;

typedef struct {
    char     name[16];
    uint64_t offset;
    uint64_t length;
    uint32_t crc;
    uint32_t rsv;
} NnkStateRegion;

#define NNKS_OFF_HDRCRC (sizeof(NnkStateHdr) - sizeof(uint32_t))

/* Region kinds: 0 = generated field table, 1 = display blob, 2 = movie
 * stream cursor table. */
typedef struct {
    const char *name;
    uint32_t kind;
    const NnkStateField *f;
    uint32_t n;
} NnkStateRegionDef;

#define NNKS_ARRAY(a) (a), (uint32_t)(sizeof(a) / sizeof((a)[0]))

static const NnkStateRegionDef nnks_regions[] = {
    { "io",           0, NNKS_ARRAY(nnks_fields_io) },
    { "smc",          0, NNKS_ARRAY(nnks_fields_smc) },
    { "card",         0, NNKS_ARRAY(nnks_fields_card) },
#ifdef NNK_GX
    { "input",        0, NNKS_ARRAY(nnks_fields_input) },
#else
    { "input",        0, NULL, 0 },
#endif
    { "movie",        0, NNKS_ARRAY(nnks_fields_movie) },
    { "moviestreams", 2, NULL, 0 },
#ifdef NNK_GX
    { "display",      0, NNKS_ARRAY(nnks_fields_display) },
    { "gxblob",       1, NULL, 0 },
#else
    { "display",      0, NULL, 0 },
    { "gxblob",       1, NULL, 0 },
#endif
    { "ram",          0, NNKS_ARRAY(nnks_fields_ram) },
    { "itcm",         0, NNKS_ARRAY(nnks_fields_itcm) },
    { "dtcm",         0, NNKS_ARRAY(nnks_fields_dtcm) },
};

#define NNKS_NREGIONS (sizeof(nnks_regions) / sizeof(nnks_regions[0]))

/* ---- run configuration -------------------------------------------------- */

static int         nnks_ready;                     /* boot hook ran */
static int         nnks_resync;                    /* a load happened; the run loop must adopt nnks_pc */
static int         nnks_boot_load_done;             /* a state was applied at boot */

static uint32_t nnks_entry_pc(void) { return nnks_pc; }

static char        nnks_dir[NNKS_PATH_MAX] = ".";
static char        nnks_path[NNKS_PATH_MAX] = "nnk_save1.nnks";
static uint64_t    nnks_save_at;
static uint64_t    nnks_hash_at;
static int         nnks_save_done;
static int         nnks_hash_done;
static char        nnks_save_path[NNKS_PATH_MAX];
static uint64_t    nnks_rec_step;                  /* step of the last save */
static uint32_t    nnks_rec_build_fp;

static void nnks_boot(int argc, char **argv);      /* forward: UI key path */

/* gxd_frames exists only in NNK_GX builds; the state carries it there and 0
 * elsewhere (a non-GX build has no display model by design). */
static unsigned nnks_frames(void)
{
#ifdef NNK_GX
    return gxd_frames;
#else
    return 0;
#endif
}

/* ---- identity ----------------------------------------------------------- */

static uint32_t nnks_build_fp(void)
{
    uint32_t h = 2166136261u;
    size_t i, j;
    if (nnks_rec_build_fp) return nnks_rec_build_fp;
    for (i = 0; i < NNKS_NREGIONS; i++) {
        h ^= nnks_fnv(nnks_regions[i].name, strlen(nnks_regions[i].name));
        h *= 16777619u;
        for (j = 0; j < nnks_regions[i].n; j++) {
            const NnkStateField *f = &nnks_regions[i].f[j];
            h ^= nnks_fnv(f->name, strlen(f->name));
            h *= 16777619u;
            h ^= f->len;
            h *= 16777619u;
        }
    }
#ifdef NNK_GX
    h ^= gxd_state_layout_fp();
    h *= 16777619u;
#endif
    nnks_rec_build_fp = h;
    return h;
}

/* The ROM identity comes from the card image attached in main() (Phase-5AJ);
 * it is what a state must match to be applicable. */
static uint64_t nnks_rom_size(void) { return (uint64_t)rom_len; }
static uint32_t nnks_rom_hdr_crc(void) { return nnks_crc32(rom, rom_len < 0x200 ? rom_len : 0x200); }
static uint32_t nnks_arm9_crc(void)
{
    if (!rom || rom_len < 0x40) return 0;
    {
        uint32_t off = (uint32_t)rom[0x20] | ((uint32_t)rom[0x21] << 8) |
                       ((uint32_t)rom[0x22] << 16) | ((uint32_t)rom[0x23] << 24);
        uint32_t len = (uint32_t)rom[0x2C] | ((uint32_t)rom[0x2D] << 8) |
                       ((uint32_t)rom[0x2E] << 16) | ((uint32_t)rom[0x2F] << 24);
        if ((uint64_t)off + len > (uint64_t)rom_len) return 0;
        return nnks_crc32(rom + off, len);
    }
}

/* ---- region payload ----------------------------------------------------- */

/* Bytes one region occupies in the payload; 0 for a region that is empty in
 * this build (e.g. the gx regions in a non-NNK_GX build). */
static size_t nnks_region_len(uint32_t idx)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    size_t n = 0;
    uint32_t i;
    switch (r->kind) {
    case 0:
        for (i = 0; i < r->n; i++) n += r->f[i].len;
        return n;
    case 1:
#if defined(NNK_GX)
        return gxd ? gxd_state_size(gxd) : 0;
#else
        return 0;
#endif
    case 2:
        return (size_t)p5t_nstr * sizeof(NnkStateMovie);
    }
    return 0;
}

static void nnks_region_store(uint32_t idx, uint8_t *dst)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    uint32_t i;
    size_t off = 0;
    switch (r->kind) {
    case 0:
        for (i = 0; i < r->n; i++) {
            memcpy(dst + off, r->f[i].p, r->f[i].len);
            off += r->f[i].len;
        }
        /* The hook runs AFTER `++steps` but BEFORE the instruction executes, so
         * the recorded step counter is written as steps-1: the restored run's
         * own increment then reproduces exactly the same count for a single
         * execution of that instruction. */
        if (strcmp(r->name, "io") == 0) {
            size_t so = 0;
            for (i = 0; i < r->n; i++) {
                if (strcmp(r->f[i].name, "steps") == 0) {
                    uint64_t v = steps - 1;
                    memcpy(dst + so, &v, sizeof v);
                    break;
                }
                so += r->f[i].len;
            }
        }
        return;
    case 1:
#if defined(NNK_GX)
        if (gxd) {
            int w = gxd_state_save(gxd, dst, gxd_state_size(gxd));
            if (w < 0) fprintf(stderr, "state: display snapshot failed (%d)\n", w);
        }
#endif
        return;
    case 2: {
        NnkStateMovie *m = (NnkStateMovie *)dst;
        for (i = 0; i < (uint32_t)p5t_nstr; i++) {
            memset(&m[i], 0, sizeof m[i]);
            m[i].codec    = p5t_str[i].codec;
            m[i].cursor   = p5t_str[i].cursor;
            m[i].rom      = (uint32_t)p5t_str[i].rom;
            m[i].index    = (int32_t)p5t_str[i].index;
            m[i].frames   = (int32_t)p5t_str[i].frames;
            m[i].presents = (int32_t)p5t_str[i].presents;
            m[i].shortr   = (int32_t)p5t_str[i].shortr;
        }
        return;
    }
    }
}

/* Re-derive the Mobiclip reference frames for a restored stream.  The codec is
 * inter-frame, so its internal state after N presents is exactly `decode
 * frames 0..N` — replaying that is deterministic and cheap (the state is
 * restored from the same ROM) and avoids storing host pointers. */
static void nnks_movie_resync(void)
{
    int i;
    for (i = 0; i < p5t_nstr; i++) {
        P5tStream *s = &p5t_str[i];
        if (!s->rom) {
            if (s->index >= 0)
                fprintf(stderr, "state: warning: movie stream %d is a %s A/B source;"
                                " its file cursor is not restorable\n", i,
                        "yuv");
            continue;
        }
        if (!s->dec.opened || s->index <= 0) continue;
        /* Rebuild the decoder instead of rewinding it: mobiclip_flush() does
         * not clear every reference-frame slot, and a decoder rewound in
         * place then replays with a corrupt lower half of the picture
         * (measured, window F9: rows 102..191 differed from the continued
         * run while a fresh boot load of the same state was byte-identical).
         * A fresh nnk_mobi_open() + sequential decode 0..index-1 is exactly
         * the path a boot load takes. */
        nnk_mobi_close(&s->dec);
        if (nnk_mobi_open(&s->dec, s->mods.width, s->mods.height) < 0) {
            fprintf(stderr, "state: warning: movie stream %d decoder reopen"
                            " failed\n", i);
            continue;
        }
        if (nnk_mobi_frame(&s->dec, &s->mods, (int)(s->index - 1),
                           NULL, NULL, NULL) < 0)
            fprintf(stderr, "state: warning: movie stream %d re-decode to frame"
                            " %ld failed\n", i, s->index);
    }
}

static int nnks_region_load(uint32_t idx, const uint8_t *src, size_t len)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    uint32_t i;
    size_t off = 0;
    switch (r->kind) {
    case 0:
        if (len != nnks_region_len(idx)) return -1;
        for (i = 0; i < r->n; i++) {
            if (off + r->f[i].len > len) return -1;
            memcpy(r->f[i].p, src + off, r->f[i].len);
            off += r->f[i].len;
        }
        return 0;
    case 1:
#if defined(NNK_GX)
        if (len && gxd) {
            int rc = gxd_state_load(gxd, src, len);
            if (rc < 0) {
                fprintf(stderr, "state: display state rejected (gxd rc=%d)\n", rc);
                return -1;
            }
        }
#endif
        return 0;
    case 2: {
        const NnkStateMovie *m = (const NnkStateMovie *)src;
        uint32_t n;
        if (len % sizeof(NnkStateMovie)) return -1;
        n = (uint32_t)(len / sizeof(NnkStateMovie));
        if (n > (uint32_t)P5T_MAXSTREAM) return -1;
        /* An EMPTY stream table carries no decoder state: leave the lazily
         * initialised p5t_state exactly as the `movie` region restored it
         * (0 = not yet initialised).  Calling p5t_init() here turned the saved
         * 0 into 3, so a restored run's state hash differed from the original
         * run's at the same step although every byte of RAM/VRAM/ITCM matched. */
        if (n == 0) return 0;
        if (!p5t_state) p5t_init();
        if (p5t_state < 2) {
            fprintf(stderr, "state: %u movie stream(s) in the state but the"
                            " movie HLE is off in this process\n", n);
            return -1;
        }
        /* The `movie` region restored p5t_nstr BEFORE this table, and a fresh
         * process (NNK_SAVESTATE= boot load) has no open stream yet.  Rebuild
         * the missing streams from the saved codecs in saved order:
         * p5t_stream() re-opens the same container from the same card image
         * and initialises the decoder at frame 0, and the cursors below plus
         * nnks_movie_resync() then replay the decode deterministically.  Only
         * an EMPTY table is rebuilt — live streams whose codec disagrees with
         * the state are refused instead of being rebound under the guest. */
        if (p5t_nstr && !p5t_str[0].codec) {
            memset(p5t_str, 0, sizeof p5t_str);
            p5t_nstr = 0;
        }
        for (i = (uint32_t)p5t_nstr; i < n; i++) {
            if (!p5t_stream(m[i].codec)) {
                fprintf(stderr, "state: cannot recreate movie stream %u"
                                " (codec 0x%08X)\n", i, m[i].codec);
                return -1;
            }
        }
        if (p5t_nstr != (int)n) return -1;
        for (i = 0; i < (uint32_t)p5t_nstr; i++) {
            if (m[i].codec != p5t_str[i].codec) {
                fprintf(stderr, "state: movie stream %u codec 0x%08X != 0x%08X\n",
                        i, m[i].codec, p5t_str[i].codec);
                return -1;
            }
            p5t_str[i].cursor   = m[i].cursor;
            p5t_str[i].rom      = (int)m[i].rom;
            p5t_str[i].index    = (long)m[i].index;
            p5t_str[i].frames   = (long)m[i].frames;
            p5t_str[i].presents = (long)m[i].presents;
            p5t_str[i].shortr   = (long)m[i].shortr;
        }
        nnks_movie_resync();
        return 0;
    }
    }
    return -1;
}

/* ---- save --------------------------------------------------------------- */

static int nnks_write_file(const char *path)
{
    size_t hdrlen = sizeof(NnkStateHdr) + NNKS_NREGIONS * sizeof(NnkStateRegion);
    size_t total = 0, off;
    uint8_t *buf;
    NnkStateHdr h;
    NnkStateRegion regs[NNKS_MAXREGIONS];
    FILE *fp;
    char tmp[NNKS_PATH_MAX + 8];
    int i, n;
    size_t psize;

    if (NNKS_NREGIONS > NNKS_MAXREGIONS) return -1;
    for (i = 0; i < (int)NNKS_NREGIONS; i++) total += nnks_region_len((uint32_t)i);
    psize = total;
    buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return -1;
    off = 0;
    for (i = 0; i < (int)NNKS_NREGIONS; i++) {
        size_t len = nnks_region_len((uint32_t)i);
        nnks_region_store((uint32_t)i, buf + off);
        memset(&regs[i], 0, sizeof regs[i]);
        snprintf(regs[i].name, sizeof regs[i].name, "%s", nnks_regions[i].name);
        regs[i].offset = (uint64_t)off;
        regs[i].length = (uint64_t)len;
        regs[i].crc = nnks_crc32(buf + off, len);
        off += len;
    }

    memset(&h, 0, sizeof h);
    memcpy(h.magic, "NNKS", 4);
    h.version = NNKS_VERSION;
    h.header_size = (uint32_t)hdrlen;
    h.region_count = (uint32_t)NNKS_NREGIONS;
    h.build_fp = nnks_build_fp();
    h.rom_size = nnks_rom_size();
    h.rom_hdr_crc = nnks_rom_hdr_crc();
    h.arm9_crc = nnks_arm9_crc();
    h.record_step = steps;
    h.record_frames = nnks_frames();
    h.payload_size = (uint64_t)psize;
    h.payload_crc = nnks_crc32(buf, psize);
    h.header_crc = nnks_crc32((const uint8_t *)&h, NNKS_OFF_HDRCRC);

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) { free(buf); return -1; }
    n = 0;
    n += (int)fwrite(&h, 1, sizeof h, fp);
    n += (int)fwrite(regs, 1, NNKS_NREGIONS * sizeof(NnkStateRegion), fp);
    if (psize) n += (int)fwrite(buf, 1, psize, fp);
    if (fclose(fp) != 0) { free(buf); return -1; }
    if ((size_t)n != hdrlen + psize) { free(buf); return -1; }
    remove(path);
    if (rename(tmp, path) != 0) { free(buf); return -1; }
    free(buf);
    fprintf(stderr, "state: saved %s (%zu bytes: header %zu + payload %zu,"
                    " step=%llu frame=%u build=0x%08X rom=%llu crc=0x%08X)\n",
            path, hdrlen + psize, hdrlen, psize,
            (unsigned long long)steps, nnks_frames(), h.build_fp,
            (unsigned long long)h.rom_size, h.rom_hdr_crc);
    nnks_rec_step = steps;
    return 0;
}

/* Canonical payload hash — the SAME bytes nnks_write_file() writes for the
 * payload (region order is the table order), so the value is comparable
 * between a continued run and a restored run. */
static uint32_t nnks_payload_hash(size_t *len_out, uint32_t *crc_out)
{
    size_t total = 0, off = 0;
    uint8_t *buf;
    uint32_t h;
    int i;
    for (i = 0; i < (int)NNKS_NREGIONS; i++) total += nnks_region_len((uint32_t)i);
    buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) { if (len_out) *len_out = 0; if (crc_out) *crc_out = 0; return 0; }
    for (i = 0; i < (int)NNKS_NREGIONS; i++) {
        size_t len = nnks_region_len((uint32_t)i);
        nnks_region_store((uint32_t)i, buf + off);
        if (getenv("NNKS_HASH_REGIONS"))
            fprintf(stderr, "state region[%d] %-14s len=%zu crc=0x%08X\n",
                    i, nnks_regions[i].name, len, nnks_crc32(buf + off, len));
        off += len;
    }
    h = nnks_fnv(buf, total);
    if (len_out) *len_out = total;
    if (crc_out) *crc_out = nnks_crc32(buf, total);
    free(buf);
    return h;
}

/* ---- load --------------------------------------------------------------- */

/* Returns 0 on success, 1 on an identity/format refusal (the run continues
 * unchanged), -1 on an I/O or allocation error. */
static int nnks_load_file(const char *path)
{
    FILE *fp;
    NnkStateHdr h;
    NnkStateRegion regs[NNKS_MAXREGIONS];
    uint8_t *payload = NULL;
    long fsz;
    int i, rc = 0;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "state: cannot open %s\n", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (fsz = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        fprintf(stderr, "state: cannot size %s\n", path);
        return -1;
    }
    if ((size_t)fsz < sizeof h || fread(&h, 1, sizeof h, fp) != sizeof h) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: file is too short to hold a header\n", path);
        return 1;
    }
    if (memcmp(h.magic, "NNKS", 4) != 0) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: bad magic (not an NNKS savestate)\n", path);
        return 1;
    }
    if (h.version != NNKS_VERSION) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: format version %u, this build writes %u\n",
                path, h.version, NNKS_VERSION);
        return 1;
    }
    if (h.header_crc != nnks_crc32((const uint8_t *)&h, NNKS_OFF_HDRCRC)) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: header CRC mismatch (corrupt or"
                        " from another format)\n", path);
        return 1;
    }
    if (h.build_fp != nnks_build_fp()) {
        /* Phase-5AS: a state recorded by one build must not be applied by
         * another by default (the field layout can differ).  A bug
         * REPRODUCER, however, has to survive a rebuild: the operator's
         * session state was written by build 0x0768D9EB and every fix
         * rebuilds the binary.  NNK_SAVESTATE_IGNORE_BUILD=1 accepts the
         * file anyway and says so, loudly, on every load.  Only the build
         * fingerprint is relaxed: magic, version, header/payload/region
         * CRCs, region names+lengths and the ROM identity stay strict, so a
         * genuinely incompatible layout is still refused. */
        const char *ib = getenv("NNK_SAVESTATE_IGNORE_BUILD");
        if (ib && ib[0] == '1') {
            fprintf(stderr, "state: WARNING: NNK_SAVESTATE_IGNORE_BUILD=1 accepts %s"
                            " written by build 0x%08X on build 0x%08X -- reproduce-only,"
                            " never for a round trip\n",
                    path, h.build_fp, nnks_build_fp());
        } else {
            fclose(fp);
            fprintf(stderr, "state: REFUSED %s: build fingerprint 0x%08X != 0x%08X"
                            " (state was written by a different binary/layout)\n",
                    path, h.build_fp, nnks_build_fp());
            return 1;
        }
    }
    if (h.rom_size != nnks_rom_size()) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: ROM size %llu B != %llu B\n", path,
                (unsigned long long)h.rom_size,
                (unsigned long long)nnks_rom_size());
        return 1;
    }
    if (h.rom_hdr_crc != nnks_rom_hdr_crc() || h.arm9_crc != nnks_arm9_crc()) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: ROM identity mismatch (header"
                        " crc 0x%08X/0x%08X, arm9 crc 0x%08X/0x%08X)\n", path,
                h.rom_hdr_crc, nnks_rom_hdr_crc(), h.arm9_crc, nnks_arm9_crc());
        return 1;
    }
    if (h.region_count != (uint32_t)NNKS_NREGIONS ||
        h.header_size != (uint32_t)(sizeof h + h.region_count * sizeof(NnkStateRegion))) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: %u regions / header %u B do not match"
                        " this build (%u / %zu B)\n", path, h.region_count,
                h.header_size, (unsigned)NNKS_NREGIONS,
                sizeof h + NNKS_NREGIONS * sizeof(NnkStateRegion));
        return 1;
    }
    if (fread(regs, 1, h.region_count * sizeof(NnkStateRegion), fp) !=
        h.region_count * sizeof(NnkStateRegion) ||
        (size_t)fsz != h.header_size + h.payload_size) {
        fclose(fp);
        fprintf(stderr, "state: REFUSED %s: truncated file (want %llu B)\n", path,
                (unsigned long long)(h.header_size + h.payload_size));
        return 1;
    }
    payload = (uint8_t *)malloc(h.payload_size ? (size_t)h.payload_size : 1);
    if (!payload || fread(payload, 1, (size_t)h.payload_size, fp) != h.payload_size) {
        if (payload) free(payload);
        fclose(fp);
        fprintf(stderr, "state: cannot read payload from %s\n", path);
        return -1;
    }
    fclose(fp);
    if (h.payload_crc != nnks_crc32(payload, (size_t)h.payload_size)) {
        free(payload);
        fprintf(stderr, "state: REFUSED %s: payload CRC mismatch (corrupt file)\n", path);
        return 1;
    }

    /* Verify EVERY region before touching the machine: a half-applied state
     * would be worse than a refused one.  Field/display regions have a
     * compile-time length; the movie stream table is the one state-sized
     * region (its length follows the saved stream count), so it is checked
     * for shape here and rebuilt in nnks_region_load. */
    for (i = 0; i < (int)NNKS_NREGIONS; i++) {
        size_t want;
        if (strncmp(regs[i].name, nnks_regions[i].name, sizeof regs[i].name) != 0 ||
            regs[i].offset + regs[i].length > h.payload_size) {
            free(payload);
            fprintf(stderr, "state: REFUSED %s: region %d layout mismatch ('%s'"
                            " vs '%s')\n", path, i, regs[i].name,
                    nnks_regions[i].name);
            return 1;
        }
        if (nnks_regions[i].kind == 2) {
            if (regs[i].length % sizeof(NnkStateMovie) != 0 ||
                regs[i].length / sizeof(NnkStateMovie) > (uint64_t)P5T_MAXSTREAM) {
                free(payload);
                fprintf(stderr, "state: REFUSED %s: region '%s' has %llu bytes,"
                                " not a stream table\n", path, regs[i].name,
                        (unsigned long long)regs[i].length);
                return 1;
            }
        } else {
            want = nnks_region_len((uint32_t)i);
            if (regs[i].length != (uint64_t)want) {
                free(payload);
                fprintf(stderr, "state: REFUSED %s: region '%s' is %llu bytes,"
                                " this build writes %zu\n", path, regs[i].name,
                        (unsigned long long)regs[i].length, want);
                return 1;
            }
        }
        if (regs[i].crc != nnks_crc32(payload + regs[i].offset, (size_t)regs[i].length)) {
            free(payload);
            fprintf(stderr, "state: REFUSED %s: region '%s' CRC mismatch\n",
                    path, regs[i].name);
            return 1;
        }
    }
    if (h.record_step > steps) {
        fprintf(stderr, "state: warning: state was recorded at step %llu, this"
                        " run is at %llu (loading anyway)\n",
                (unsigned long long)h.record_step, (unsigned long long)steps);
    }
    for (i = 0; i < (int)NNKS_NREGIONS; i++) {
        if (nnks_region_load((uint32_t)i, payload + regs[i].offset,
                             (size_t)regs[i].length) != 0) {
            rc = -1;
            break;
        }
    }
    free(payload);
    if (rc != 0) {
        fprintf(stderr, "state: REFUSED %s: a region could not be applied\n", path);
        return rc;
    }
    fprintf(stderr, "state: loaded %s (%lld bytes, identity OK: rom %llu B"
                    " hdr_crc=0x%08X arm9_crc=0x%08X build=0x%08X;"
                    " restored step=%llu frame=%u)\n",
            path, (long long)fsz, (unsigned long long)h.rom_size,
            h.rom_hdr_crc, h.arm9_crc, h.build_fp,
            (unsigned long long)steps, nnks_frames());
    fprintf(stderr, "state: resume pc=0x%08X r0=%08X r3=%08X r13=%08X r14=%08X\n",
            nnks_pc, r[0], r[3], r[13], r[14]);
    nnks_resync = 1;        /* the dispatch loop adopts nnks_pc on its next step */
    return 0;
}

/* ---- per-step / boot / UI hooks ----------------------------------------- */

static void nnks_tick(uint32_t *pc_io)
{
    if (nnks_resync) {          /* an F9 load happened since the last step */
        *pc_io = nnks_pc;
        nnks_resync = 0;
    }
    nnks_pc = *pc_io;
    if (!nnks_ready) return;
    if (nnks_save_at && !nnks_save_done && steps >= nnks_save_at) {
        nnks_save_done = 1;
        nnks_write_file(nnks_save_path);
    }
    if (nnks_hash_at && !nnks_hash_done && steps >= nnks_hash_at) {
        size_t len = 0;
        uint32_t crc = 0;
        uint32_t h = nnks_payload_hash(&len, &crc);
        nnks_hash_done = 1;
        fprintf(stderr, "state hash: step=%llu fnv=0x%08X crc=0x%08X bytes=%zu"
                        " build=0x%08X\n",
                (unsigned long long)steps, h, crc, len, nnks_build_fp());
    }
}

/* F5 / F9 (see tools/nnk_input.inc's SDL filter). */
static void nnk_state_ui_key(int fkey)
{
    if (!nnks_ready) nnks_boot(0, NULL);
    if (fkey == 5) {
        nnks_write_file(nnks_path);
    } else if (fkey == 9) {
        int rc = nnks_load_file(nnks_path);
        if (rc == 0) fprintf(stderr, "state: F9 restored '%s' — continuing from"
                                     " step=%llu pc=0x%08X\n",
                            nnks_path, (unsigned long long)steps, last_pc);
        else if (rc > 0) fprintf(stderr, "state: F9 refused '%s' — the run is"
                                         " unchanged\n", nnks_path);
    }
}

static void nnks_boot(int argc, char **argv)
{
    const char *s;
    char a0[NNKS_PATH_MAX];
    if (nnks_ready) return;
    /* default state path: next to the runner binary (operator request) */
    if (argc > 0 && argv && argv[0] && argv[0][0]) {
        char *slash;
        snprintf(a0, sizeof a0, "%s", argv[0]);
        slash = strrchr(a0, '/');
        if (slash) { *slash = 0; snprintf(nnks_dir, sizeof nnks_dir, "%s", a0[0] ? a0 : "/"); }
        else snprintf(nnks_dir, sizeof nnks_dir, ".");
    }
    s = getenv("NNK_SAVESTATE_DIR");
    if (s && s[0]) snprintf(nnks_dir, sizeof nnks_dir, "%s", s);
    snprintf(nnks_path, sizeof nnks_path, "%s/nnk_save1.nnks", nnks_dir);
    snprintf(nnks_save_path, sizeof nnks_save_path, "%s", nnks_path);
    s = getenv("NNK_SAVESTATE_SAVE");
    if (s && s[0]) snprintf(nnks_save_path, sizeof nnks_save_path, "%s", s);
    s = getenv("NNK_SAVESTATE_SAVE_AT");
    if (s && s[0]) nnks_save_at = strtoull(s, NULL, 0);
    s = getenv("NNK_STATE_HASH_AT");
    if (s && s[0]) nnks_hash_at = strtoull(s, NULL, 0);
    nnks_ready = 1;
    fprintf(stderr, "state: savestates %s (dir=%s default=%s save_at=%llu hash_at=%llu)\n",
            "enabled (F5 save / F9 load in NNK_GX=window)",
            nnks_dir, nnks_path,
            (unsigned long long)nnks_save_at, (unsigned long long)nnks_hash_at);
    s = getenv("NNK_SAVESTATE");
    if (s && s[0]) {
        if (nnks_load_file(s) == 0) {
            nnks_boot_load_done = 1;
            /* `steps` now starts at the recorded step, so the compile-time
             * budget (a run LENGTH) would stop on the first instruction.
             * Give the resumed run a full budget; an explicit NNK_STEP_LIMIT
             * is absolute and still wins (main parses it after this hook). */
            if (step_limit <= steps) step_limit = steps + STEP_LIMIT;
        } else fprintf(stderr, "state: boot from '%s' failed — replaying the boot\n", s);
    }
}

#endif /* NNK_STATE_INC */
