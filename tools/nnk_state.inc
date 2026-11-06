/* tools/nnk_state.inc — Phase-5AQ: savestates (F5 save / F9 load).
 *
 * Spliced into nnk_main.c by tools/split_dispatch.py right after the other
 * runtime fragments, so it sees every runtime static: the CPU register file
 * and banked r13/r14/SPSR, the guest memory window, the IO/PPU/VRAMCNT model,
 * the card/timer/DMA/IPC shims, the SMC generation + band tables, the input
 * script cursor, the movie HLE cursors — and the display model through the
 * gxd_state_* snapshot the renderer gained for this phase.
 *
 * The container FORMAT (header, region records, field directory, checksums,
 * identity check and the two field-matching policies) lives in
 * tools/nnks_container.inc, which is spliced next to this file and is free of
 * machine dependencies — tools/nnks_selftest.c compiles it and the class test
 * exercises the real code.
 *
 * WHAT IS CARRIED
 *   The field table is GENERATED from the effective prelude at build time
 *   (tools/nnks_gen_state_fields.py -> nnk_state_fields.h), so a static added
 *   by a later phase is carried automatically instead of silently diverging
 *   a restored run.  Region list (each with id+version+offset+length+CRC32 and
 *   its own field directory in the file):
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
 *   "NNKS" | version | header_size | region_count | build_fp | flags |
 *   rom_size u64 | rom_hdr_crc | arm9_crc | record_step u64 |
 *   record_frames u64 | payload_size u64 | payload_crc | header_crc
 *   region_count x { id | version | offset u64 | length u64 | crc32 |
 *                   nfields | name[16] }                    (v2, 48 B each)
 *   total_nfields x { name[24] | off u32 | len u32 }          (field directory)
 *   payload
 *   v1 files ({name[16]|offset|length|crc|rsv}, 40 B each, no directory) are
 *   still READ: regions are matched by name, fields by prefix.
 *
 * LOAD POLICY (Phase-5BH)
 *   default            TOLERANT.  Refuse only a real incompatibility (magic,
 *                      container version, header/payload/region CRC, ROM
 *                      identity).  A region that differs in length/layout is
 *                      restored field-by-field by NAME, every defaulted field
 *                      is named in a `state: PARTIAL ...` banner and the run
 *                      resumes at the saved step/frame.
 *   NNK_SAVESTATE_STRICT=1
 *                      exact layout required: same build fingerprint, same
 *                      region count/ids/versions/lengths, else REFUSED with
 *                      the exact region diff.  This is what the round-trip
 *                      determinism proof uses.
 *
 * ENVIRONMENT
 *   NNK_SAVESTATE=<path>          boot straight from a state (no boot replay)
 *   NNK_SAVESTATE_DIR=<dir>       default state directory (else dirname(argv[0]))
 *   NNK_SAVESTATE_SAVE_AT=<step>  write a state at that step (and KEEP RUNNING;
 *                                 the run's own step limit still ends it)
 *   NNK_SAVESTATE_SAVE=<path>     path for SAVE_AT (else <dir>/nnk_save1.nnks)
 *   NNK_STATE_HASH_AT=<step>      print the canonical state hash at that step
 *   NNK_SAVESTATE_STRICT=1        refuse any layout/build difference
 *   NNK_SAVESTATE_LAYOUT=<path>   write this build's region/field layout (what
 *                                 tools/nnks_inspect.py compares a file against)
 *   NNK_GX=window: F5 saves, F9 loads (path printed on both)
 */
#ifndef NNK_STATE_INC
#define NNK_STATE_INC

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nnks_container.inc"

#define NNKS_PATH_MAX 512

/* ---- field table (generated) -------------------------------------------- */

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

/* Region kinds: 0 = generated field table, 1 = display blob, 2 = movie
 * stream cursor table. */
typedef struct {
    uint32_t id;             /* stable numeric identity, matched on load */
    const char *name;
    uint32_t kind;
    const NnkStateField *f;
    uint32_t n;
} NnkStateRegionDef;

#define NNKS_ARRAY(a) (a), (uint32_t)(sizeof(a) / sizeof((a)[0]))

/* IDs are part of the FILE FORMAT: never renumber one, never reuse one. */
static const NnkStateRegionDef nnks_regions[] = {
    { 1,  "io",           0, NNKS_ARRAY(nnks_fields_io) },
    { 2,  "smc",          0, NNKS_ARRAY(nnks_fields_smc) },
    { 3,  "card",         0, NNKS_ARRAY(nnks_fields_card) },
#ifdef NNK_GX
    { 4,  "input",        0, NNKS_ARRAY(nnks_fields_input) },
#else
    { 4,  "input",        0, NULL, 0 },
#endif
    { 5,  "movie",        0, NNKS_ARRAY(nnks_fields_movie) },
    { 6,  "moviestreams", 2, NULL, 0 },
#ifdef NNK_GX
    { 7,  "display",      0, NNKS_ARRAY(nnks_fields_display) },
    { 8,  "gxblob",       1, NULL, 0 },
#else
    { 7,  "display",      0, NULL, 0 },
    { 8,  "gxblob",       1, NULL, 0 },
#endif
    { 9,  "ram",          0, NNKS_ARRAY(nnks_fields_ram) },
    { 10, "itcm",         0, NNKS_ARRAY(nnks_fields_itcm) },
    { 11, "dtcm",         0, NNKS_ARRAY(nnks_fields_dtcm) },
};

#define NNKS_NREGIONS (sizeof(nnks_regions) / sizeof(nnks_regions[0]))

/* ---- run configuration -------------------------------------------------- */

static int         nnks_ready;                     /* boot hook ran */
static int         nnks_resync;                    /* a load happened; the run loop must adopt nnks_pc */
static int         nnks_boot_load_done;             /* a state was applied at boot */
static int         nnks_partial;                    /* the last load defaulted something */
static int         nnks_strict;                     /* NNK_SAVESTATE_STRICT=1 */
static int         nnks_load_n;                     /* loads attempted this process */
static uint32_t    nnks_v1_pc;                       /* v1 tail-rescued pc (0 = none) */

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
static void nnks_write_layout(const char *path);   /* forward */

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
static uint32_t nnks_rom_hdr_crc(void) { return nnks_crc32((const uint8_t *)rom, rom_len < 0x200 ? rom_len : 0x200); }
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

/* The region's LAYOUT VERSION: what a stored region must match to be applied
 * byte-for-byte.  For a field table it is a fingerprint of the fields (so an
 * added/removed/re-ordered field changes it even when the total length happens
 * to match); for the display blob, the renderer's own layout fingerprint; for
 * the stream table, the record size. */
static uint32_t nnks_region_version(uint32_t idx)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    switch (r->kind) {
    case 0:
        return nnks_field_fp(r->f, r->n);
    case 1:
#if defined(NNK_GX)
        return gxd_state_layout_fp();
#else
        return 0;
#endif
    case 2:
        return (uint32_t)sizeof(NnkStateMovie);
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

/* ---- per-region load report --------------------------------------------- */

typedef struct {
    int      present;        /* the file carries this region */
    int      exact;          /* length+version matched: plain byte copy */
    int      applied;        /* something was restored */
    uint32_t stored_version;
    uint64_t stored_len;
    NnkFieldPlan plan;       /* kind 0 only */
} NnkRegionReport;

static uint32_t nnks_nfields_dir(uint32_t idx)
{
    return nnks_regions[idx].kind == 0 ? nnks_regions[idx].n : 0;
}

static void nnks_report_line(uint32_t idx, const NnkRegionReport *rep)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    const char *status;
    if (!rep->present) status = "DEFAULTED (not in the file)";
    else if (rep->exact) status = "EXACT";
    else if (rep->applied) status = "PARTIAL";
    else status = "DEFAULTED";
    fprintf(stderr, "state:   %-12s %-22s stored %llu B (v0x%08X) -> build %zu B"
                    " (v0x%08X)\n", r->name, status,
            (unsigned long long)rep->stored_len, rep->stored_version,
            nnks_region_len(idx), nnks_region_version(idx));
    if (r->kind == 0 && !rep->exact && rep->present) {
        uint32_t k;
        fprintf(stderr, "state:   %-12s %u field(s) restored, %u defaulted,"
                        " %u partial, %u gone",
                "", rep->plan.ok, rep->plan.missing, rep->plan.partial,
                rep->plan.extra);
        if (rep->plan.nnames_total) {
            fprintf(stderr, ", defaulted:");
            for (k = 0; k < rep->plan.nnames; k++)
                fprintf(stderr, " %s", rep->plan.names[k]);
            if (rep->plan.nnames_total > rep->plan.nnames)
                fprintf(stderr, " (+%u more)", rep->plan.nnames_total - rep->plan.nnames);
        }
        fprintf(stderr, "\n");
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

static int nnks_streams_load(const uint8_t *src, size_t len)
{
    const NnkStateMovie *m = (const NnkStateMovie *)src;
    uint32_t i, n;
    if (len % sizeof(NnkStateMovie)) {
        fprintf(stderr, "state: movie stream table is %zu B, not a multiple of"
                        " %zu — stream cursors defaulted\n", len,
                sizeof(NnkStateMovie));
        return -1;
    }
    n = (uint32_t)(len / sizeof(NnkStateMovie));
    if (n > (uint32_t)P5T_MAXSTREAM) n = (uint32_t)P5T_MAXSTREAM;
    /* An EMPTY stream table carries no decoder state: leave the lazily
     * initialised p5t_state exactly as the `movie` region restored it
     * (0 = not yet initialised).  Calling p5t_init() here turned the saved
     * 0 into 3, so a restored run's state hash differed from the original
     * run's at the same step although every byte of RAM/VRAM/ITCM matched. */
    if (n == 0) return 0;
    if (!p5t_state) p5t_init();
    if (p5t_state < 2) {
        fprintf(stderr, "state: %u movie stream(s) in the state but the"
                        " movie HLE is off in this process — cursors defaulted\n", n);
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
                            " (codec 0x%08X) — cursors defaulted\n", i, m[i].codec);
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

/* Apply one region.  `dir`/`ndir` are the stored field directory (NULL for a
 * v1 file, which has none).  Returns 0 when the region was restored exactly,
 * 1 when it was restored partially/defaulted (the caller reports it), -1 when
 * the stored bytes cannot be used at all. */
static int nnks_region_load(uint32_t idx, const uint8_t *src, size_t len,
                            uint32_t stored_version, const NnkFieldRec *dir,
                            uint32_t ndir, NnkRegionReport *rep)
{
    const NnkStateRegionDef *r = &nnks_regions[idx];
    size_t curlen = nnks_region_len(idx);
    uint32_t curver = nnks_region_version(idx);
    uint32_t i;

    rep->present = 1;
    rep->stored_version = stored_version;
    rep->stored_len = (uint64_t)len;
    rep->exact = (len == curlen && stored_version == curver);

    switch (r->kind) {
    case 0:
        if (rep->exact) {
            size_t off = 0;
            for (i = 0; i < r->n; i++) {
                memcpy(r->f[i].p, src + off, r->f[i].len);
                off += r->f[i].len;
            }
            rep->applied = 1;
            return 0;
        }
        if (dir && ndir) nnks_restore_fields(r->f, r->n, dir, ndir, src, len, &rep->plan);
        else             nnks_restore_fields_prefix(r->f, r->n, src, len, &rep->plan);
        /* v1 files have no field directory, so the fields past their stored
         * length are unlocatable — including the pc the run must resume at.
         * `nnks_pc` is the LAST io field in every build since Phase-5AQ (the
         * generator appends the EXTRA rows after every scanned static), so the
         * trailing 4 bytes of a v1 io region are the saved pc.  Measured on two
         * operator states: 0x02007138 / 0x0207CEA0, both inside the guest's
         * executable windows.  It is taken only when the prefix restore did not
         * already supply a pc, and only when the value looks like a guest
         * address; otherwise the run says it enters at ENTRY_CONST. */
        if (!dir && strcmp(r->name, "io") == 0 && len >= 4) {
            uint32_t cand;
            memcpy(&cand, src + len - 4, 4);
            if (cand && (cand & ~1u) >= 0x01FF0000u && (cand & ~1u) <= 0x023FFFFFu)
                nnks_v1_pc = cand;
        }
        rep->applied = (rep->plan.ok + rep->plan.partial) > 0;
        if (rep->applied && nnks_plan_all_ok(&rep->plan) && rep->plan.extra == 0) {
            /* every field matched by name/prefix: this is an exact restore even
             * though the stored layout version could not prove it up front */
            rep->exact = 1;
            return 0;
        }
        return rep->applied ? 1 : -1;
    case 1:
#if defined(NNK_GX)
        if (len && gxd) {
            int rc;
            if (!rep->exact) {
                /* The display blob is self-describing and its loader validates
                 * its own header/length; a layout change is reported by it. */
                rc = gxd_state_load(gxd, src, len);
                if (rc < 0) {
                    fprintf(stderr, "state: display state not usable (gxd rc=%d)"
                                    " — display defaulted\n", rc);
                    return 1;
                }
                fprintf(stderr, "state: display state accepted from a different"
                                " layout (gxd rc=0)\n");
                rep->applied = 1;
                return 1;
            }
            rc = gxd_state_load(gxd, src, len);
            if (rc < 0) {
                fprintf(stderr, "state: display state rejected (gxd rc=%d)\n", rc);
                return -1;
            }
        }
#endif
        rep->applied = 1;
        return 0;
    case 2:
        if (nnks_streams_load(src, len) != 0) { rep->applied = 0; return 1; }
        rep->applied = 1;
        return rep->exact ? 0 : 1;
    }
    return -1;
}

/* ---- save --------------------------------------------------------------- */

/* This build's region/field layout, as text: what a state file is compared
 * against by tools/nnks_inspect.py.  Written next to a saved state and to
 * NNK_SAVESTATE_LAYOUT. */
static void nnks_write_layout(const char *path)
{
    FILE *fp = fopen(path, "w");
    uint32_t i, j;
    if (!fp) return;
    fprintf(fp, "NNKLAYOUT %u\nbuild 0x%08X\nregions %u\n",
            NNKS_VERSION, nnks_build_fp(), (unsigned)NNKS_NREGIONS);
    for (i = 0; i < NNKS_NREGIONS; i++) {
        const NnkStateRegionDef *r = &nnks_regions[i];
        fprintf(fp, "region %u 0x%08X %zu %u %s\n", r->id,
                nnks_region_version(i), nnks_region_len(i),
                nnks_nfields_dir(i), r->name);
        if (r->kind == 0) {
            size_t off = 0;
            for (j = 0; j < r->n; j++) {
                fprintf(fp, "field %s %zu %u\n", r->f[j].name, off, r->f[j].len);
                off += r->f[j].len;
            }
        }
    }
    fprintf(fp, "end\n");
    fclose(fp);
}

/* Total field-directory entries this build writes. */
static uint32_t nnks_ndir_total(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NNKS_NREGIONS; i++) n += nnks_nfields_dir(i);
    return n;
}

static int nnks_write_file(const char *path)
{
    size_t hdrlen = sizeof(NnkStateHdr) + NNKS_NREGIONS * sizeof(NnkStateRegionV2) +
                    (size_t)nnks_ndir_total() * sizeof(NnkFieldRec);
    size_t total = 0, off;
    uint8_t *buf, *dirbuf = NULL;
    NnkStateHdr h;
    NnkStateRegionV2 regs[NNKS_MAXREGIONS];
    NnkFieldRec *dirs;
    FILE *fp;
    char tmp[NNKS_PATH_MAX + 8];
    char layout[NNKS_PATH_MAX + 16];
    int i, n, bad_name = 0;

    if (NNKS_NREGIONS > NNKS_MAXREGIONS) return -1;
    for (i = 0; i < (int)NNKS_NREGIONS; i++) total += nnks_region_len((uint32_t)i);
    buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return -1;
    dirs = (NnkFieldRec *)calloc(nnks_ndir_total() ? nnks_ndir_total() : 1, sizeof(NnkFieldRec));
    if (!dirs) { free(buf); return -1; }
    off = 0;
    for (i = 0; i < (int)NNKS_NREGIONS; i++) {
        size_t len = nnks_region_len((uint32_t)i);
        nnks_region_store((uint32_t)i, buf + off);
        memset(&regs[i], 0, sizeof regs[i]);
        regs[i].id = nnks_regions[i].id;
        regs[i].version = nnks_region_version((uint32_t)i);
        snprintf(regs[i].name, sizeof regs[i].name, "%s", nnks_regions[i].name);
        regs[i].offset = (uint64_t)off;
        regs[i].length = (uint64_t)len;
        regs[i].crc = nnks_crc32(buf + off, len);
        if (nnks_regions[i].kind == 0) {
            int bad = 0;
            nnks_field_total(nnks_regions[i].f, nnks_regions[i].n, &bad);
            if (bad) bad_name = 1;
        }
        off += len;
    }
    /* the directory is written in region order, right after the table */
    {
        uint32_t w = 0, left = nnks_ndir_total();
        for (i = 0; i < (int)NNKS_NREGIONS; i++) {
            if (nnks_regions[i].kind != 0) continue;
            regs[i].nfields = nnks_mkdir(nnks_regions[i].f, nnks_regions[i].n,
                                         dirs + w, left);
            w += regs[i].nfields;
            left -= regs[i].nfields;
        }
    }
    if (bad_name)
        fprintf(stderr, "state: warning: a field name is %d chars or longer;"
                        " name matching (tolerant restore) will not work for it\n",
                NNKS_FIELDNAME);

    memset(&h, 0, sizeof h);
    memcpy(h.magic, "NNKS", 4);
    h.version = NNKS_VERSION;
    h.header_size = (uint32_t)hdrlen;
    h.region_count = (uint32_t)NNKS_NREGIONS;
    h.build_fp = nnks_build_fp();
    h.flags = NNKS_FLAG_FIELDDIR;
    h.rom_size = nnks_rom_size();
    h.rom_hdr_crc = nnks_rom_hdr_crc();
    h.arm9_crc = nnks_arm9_crc();
    h.record_step = steps;
    h.record_frames = nnks_frames();
    h.payload_size = (uint64_t)total;
    h.payload_crc = nnks_crc32(buf, total);
    h.header_crc = nnks_crc32((const uint8_t *)&h, NNKS_OFF_HDRCRC);

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) { free(buf); free(dirs); return -1; }
    n = 0;
    n += (int)fwrite(&h, 1, sizeof h, fp);
    n += (int)fwrite(regs, 1, NNKS_NREGIONS * sizeof(NnkStateRegionV2), fp);
    if (nnks_ndir_total())
        n += (int)fwrite(dirs, 1, (size_t)nnks_ndir_total() * sizeof(NnkFieldRec), fp);
    if (total) n += (int)fwrite(buf, 1, total, fp);
    free(dirs);
    if (fclose(fp) != 0) { free(buf); fprintf(stderr, "state: SAVE FAILED %s (close)\n", path); return -1; }
    if ((size_t)n != hdrlen + total) {
        free(buf);
        fprintf(stderr, "state: SAVE FAILED %s (%d of %zu bytes written)\n",
                path, n, hdrlen + total);
        return -1;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        free(buf);
        fprintf(stderr, "state: SAVE FAILED %s (rename from %s)\n", path, tmp);
        return -1;
    }
    free(buf);
    fprintf(stderr, "state: saved %s (%zu bytes: header %zu + payload %zu,"
                    " region table %u + field directory %u entries;"
                    " step=%llu frame=%u build=0x%08X rom=%llu crc=0x%08X)\n",
            path, hdrlen + total, hdrlen, total, (unsigned)NNKS_NREGIONS,
            nnks_ndir_total(), (unsigned long long)steps, nnks_frames(),
            h.build_fp, (unsigned long long)h.rom_size, h.rom_hdr_crc);
    /* The layout sidecar is what tools/nnks_inspect.py compares a file
     * against; writing it next to the state means an F5 save is always
     * classifiable with one command. */
    snprintf(layout, sizeof layout, "%s.layout", path);
    nnks_write_layout(layout);
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

static const char *nnks_identity_reason(uint32_t f)
{
    if (f & NNKS_ID_MAGIC)   return "bad magic (not an NNKS savestate)";
    if (f & NNKS_ID_VERSION) return "container format version not supported";
    if (f & NNKS_ID_HDRCRC)  return "header CRC mismatch (corrupt or from another format)";
    if (f & NNKS_ID_ROMSIZE) return "ROM size differs";
    if (f & NNKS_ID_ROMHDR)  return "ROM header CRC differs (wrong card image)";
    if (f & NNKS_ID_ARM9)    return "ROM ARM9 CRC differs (wrong card image)";
    return "unknown";
}

static uint32_t nnks_index_of_id(uint32_t id, const char *name)
{
    uint32_t i;
    for (i = 0; i < NNKS_NREGIONS; i++) {
        if (nnks_regions[i].id == id) return i;
    }
    for (i = 0; i < NNKS_NREGIONS; i++) {     /* v1 has no id: match the name */
        if (strncmp(name, nnks_regions[i].name, 16) == 0) return i;
    }
    return NNKS_NREGIONS;
}

/* Loud, banner-prominence report of a load that is not a clean exact restore.
 * The defect this phase fixes is that a refused/partial state scrolled past as
 * one line and every later measurement silently was a cold boot. */
static void nnks_banner(const char *tag, const char *path, const char *detail)
{
    fprintf(stderr, "state: ##############################################################\n");
    fprintf(stderr, "state: ###  %s\n", tag);
    fprintf(stderr, "state: ###  %s\n", path);
    if (detail && detail[0]) fprintf(stderr, "state: ###  %s\n", detail);
    fprintf(stderr, "state: ##############################################################\n");
}

/* Returns 0 when a state was applied (exactly or partially), 1 on a refusal
 * (the run continues unchanged), -1 on an I/O or allocation error. */
static int nnks_load_file(const char *path)
{
    FILE *fp;
    NnkStateHdr h;
    NnkStateRegionV1 regs1[NNKS_MAXREGIONS];
    NnkStateRegionV2 regs2[NNKS_MAXREGIONS];
    NnkStateRegionV2 st[NNKS_MAXREGIONS];
    NnkFieldRec *dirs = NULL;
    uint8_t *payload = NULL;
    uint32_t map[NNKS_MAXREGIONS];
    NnkRegionReport rep[NNKS_MAXREGIONS];
    long fsz;
    size_t dirbytes = 0, cur;
    uint32_t ident, total_dir = 0, i, id;
    int v1 = 0, nreg = 0, npartial = 0, nexact = 0, napplied = 0, ndropped = 0;
    char detail[512];

    memset(rep, 0, sizeof rep);
    nnks_v1_pc = 0;
    nnks_load_n++;
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
        snprintf(detail, sizeof detail, "file is shorter than the %zu-byte header",
                 sizeof h);
        nnks_banner("REFUSED — this file is not a usable savestate", path, detail);
        return 1;
    }
    /* ---- identity: the ONLY things that can make a file unusable ---- */
    ident = nnks_identity(&h, nnks_rom_size(), nnks_rom_hdr_crc(), nnks_arm9_crc());
    if (ident) {
        fclose(fp);
        if (ident & NNKS_ID_VERSION)
            snprintf(detail, sizeof detail, "container version %u, this build"
                     " reads %u..%u", h.version, NNKS_VERSION_MIN, NNKS_VERSION);
        else
            snprintf(detail, sizeof detail, "%s", nnks_identity_reason(ident));
        nnks_banner("REFUSED — incompatible savestate", path, detail);
        return 1;
    }
    v1 = (h.version == 1);
    nreg = (int)h.region_count;
    if (nreg <= 0 || nreg > NNKS_MAXREGIONS) {
        fclose(fp);
        snprintf(detail, sizeof detail, "region count %d is outside 1..%d", nreg,
                 NNKS_MAXREGIONS);
        nnks_banner("REFUSED — incompatible savestate", path, detail);
        return 1;
    }
    if (v1) {
        if (h.header_size != sizeof h + (size_t)nreg * sizeof(NnkStateRegionV1)) {
            fclose(fp);
            snprintf(detail, sizeof detail, "v1 header is %u B, expected %zu B",
                     h.header_size, sizeof h + (size_t)nreg * sizeof(NnkStateRegionV1));
            nnks_banner("REFUSED — incompatible savestate", path, detail);
            return 1;
        }
        if (fread(regs1, sizeof(NnkStateRegionV1), (size_t)nreg, fp) != (size_t)nreg) {
            fclose(fp);
            nnks_banner("REFUSED — truncated region table", path, NULL);
            return 1;
        }
        for (i = 0; i < (uint32_t)nreg; i++) {
            memset(&st[i], 0, sizeof st[i]);
            /* v1 carries no id and no layout version: the region is matched by
             * NAME and its fields by prefix, so the stored version is unknown
             * (0) and can never count as "exact". */
            st[i].version = 0;
            st[i].offset = regs1[i].offset;
            st[i].length = regs1[i].length;
            st[i].crc = regs1[i].crc;
            st[i].nfields = 0;
            memcpy(st[i].name, regs1[i].name, sizeof st[i].name);
        }
    } else {
        if (fread(regs2, sizeof(NnkStateRegionV2), (size_t)nreg, fp) != (size_t)nreg) {
            fclose(fp);
            nnks_banner("REFUSED — truncated region table", path, NULL);
            return 1;
        }
        for (i = 0; i < (uint32_t)nreg; i++) {
            st[i] = regs2[i];
            if (h.flags & NNKS_FLAG_FIELDDIR) total_dir += regs2[i].nfields;
        }
        cur = sizeof h + (size_t)nreg * sizeof(NnkStateRegionV2) +
              (size_t)total_dir * sizeof(NnkFieldRec);
        if (total_dir && h.header_size != (uint32_t)cur) {
            fclose(fp);
            snprintf(detail, sizeof detail, "header is %u B, its table+directory"
                     " describe %zu B", h.header_size, cur);
            nnks_banner("REFUSED — incompatible savestate", path, detail);
            return 1;
        }
        if (h.header_size != sizeof h + (size_t)nreg * sizeof(NnkStateRegionV2) &&
            h.header_size != (uint32_t)cur) {
            fclose(fp);
            snprintf(detail, sizeof detail, "header is %u B, region table alone is"
                     " %zu B", h.header_size,
                     sizeof h + (size_t)nreg * sizeof(NnkStateRegionV2));
            nnks_banner("REFUSED — incompatible savestate", path, detail);
            return 1;
        }
        dirbytes = (size_t)total_dir * sizeof(NnkFieldRec);
        if (dirbytes) {
            dirs = (NnkFieldRec *)malloc(dirbytes);
            if (!dirs || fread(dirs, 1, dirbytes, fp) != dirbytes) {
                if (dirs) free(dirs);
                fclose(fp);
                nnks_banner("REFUSED — truncated field directory", path, NULL);
                return 1;
            }
        }
    }
    if ((uint64_t)fsz != h.header_size + h.payload_size) {
        if (dirs) free(dirs);
        fclose(fp);
        snprintf(detail, sizeof detail, "file is %ld B, header+payload say %llu B",
                 fsz, (unsigned long long)(h.header_size + h.payload_size));
        nnks_banner("REFUSED — truncated savestate", path, detail);
        return 1;
    }
    payload = (uint8_t *)malloc(h.payload_size ? (size_t)h.payload_size : 1);
    if (!payload || fread(payload, 1, (size_t)h.payload_size, fp) != h.payload_size) {
        if (payload) free(payload);
        if (dirs) free(dirs);
        fclose(fp);
        fprintf(stderr, "state: cannot read payload from %s\n", path);
        return -1;
    }
    fclose(fp);
    if (h.payload_crc != nnks_crc32(payload, (size_t)h.payload_size)) {
        free(payload);
        if (dirs) free(dirs);
        nnks_banner("REFUSED — payload CRC mismatch (corrupt file)", path, NULL);
        return 1;
    }

    /* ---- every stored region must be intact and in bounds ---- */
    for (i = 0; i < (uint32_t)nreg; i++) {
        if (st[i].offset + st[i].length > h.payload_size) {
            free(payload);
            if (dirs) free(dirs);
            snprintf(detail, sizeof detail, "region '%.16s' spans %llu..%llu B of a"
                     " %llu B payload", st[i].name,
                     (unsigned long long)st[i].offset,
                     (unsigned long long)(st[i].offset + st[i].length),
                     (unsigned long long)h.payload_size);
            nnks_banner("REFUSED — region outside the payload", path, detail);
            return 1;
        }
        if (st[i].crc != nnks_crc32(payload + st[i].offset, (size_t)st[i].length)) {
            free(payload);
            if (dirs) free(dirs);
            snprintf(detail, sizeof detail, "region '%.16s' CRC mismatch", st[i].name);
            nnks_banner("REFUSED — corrupt savestate", path, detail);
            return 1;
        }
    }

    /* ---- STRICT: the exact-layout contract (the round-trip proof uses it) ---- */
    if (nnks_strict) {
        int bad = 0;
        size_t dlen = 0;
        detail[0] = 0;
        if (h.build_fp != nnks_build_fp()) {
            snprintf(detail, sizeof detail, "build fingerprint 0x%08X != 0x%08X",
                     h.build_fp, nnks_build_fp());
            dlen = strlen(detail);
            bad = 1;
        }
        if ((uint32_t)nreg != (uint32_t)NNKS_NREGIONS) {
            snprintf(detail + dlen, sizeof detail - dlen, "%sregion count %d != %u",
                     dlen ? "; " : "", nreg, (unsigned)NNKS_NREGIONS);
            dlen = strlen(detail);
            bad = 1;
        } else {
            for (i = 0; i < NNKS_NREGIONS; i++) {
                id = v1 ? nnks_index_of_id(0, st[i].name) : nnks_index_of_id(st[i].id, st[i].name);
                if (id != i) {
                    snprintf(detail + dlen, sizeof detail - dlen,
                             "%sregion %u is '%.16s' in the file and '%s' in this build",
                             dlen ? "; " : "", i, st[i].name, nnks_regions[i].name);
                    dlen = strlen(detail);
                    bad = 1;
                    break;
                }
                if (st[i].length != (uint64_t)nnks_region_len(i) ||
                    (!v1 && st[i].version != nnks_region_version(i)) ||
                    (nnks_regions[i].kind == 2 && st[i].length % sizeof(NnkStateMovie))) {
                    snprintf(detail + dlen, sizeof detail - dlen,
                             "%sregion '%s' stored id=%u v0x%08X %llu B, build id=%u"
                             " v0x%08X %zu B (%+lld B)",
                             dlen ? "; " : "", nnks_regions[i].name, st[i].id,
                             st[i].version, (unsigned long long)st[i].length,
                             nnks_regions[i].id, nnks_region_version(i),
                             nnks_region_len(i),
                             (long long)st[i].length -
                             (long long)nnks_region_len(i));
                    dlen = strlen(detail);
                    bad = 1;
                    break;
                }
            }
        }
        if (bad) {
            free(payload);
            if (dirs) free(dirs);
            nnks_banner("REFUSED (NNK_SAVESTATE_STRICT=1) — layout differs", path, detail);
            return 1;
        }
    } else if (h.build_fp != nnks_build_fp()) {
        /* Tolerant by default: a state from ANY earlier build is restored by
         * id/name.  This is the whole point of Phase-5BH. */
        fprintf(stderr, "state: note: %s was written by build 0x%08X, this build is"
                        " 0x%08X — regions are matched BY ID and fields BY NAME"
                        " (tolerant restore)%s\n", path, h.build_fp, nnks_build_fp(),
                getenv("NNK_SAVESTATE_IGNORE_BUILD") ?
                " [NNK_SAVESTATE_IGNORE_BUILD is now the default]" : "");
    }

    if (h.record_step > steps && nnks_load_n > 1) {
        fprintf(stderr, "state: warning: state was recorded at step %llu, this"
                        " run is at %llu (loading anyway)\n",
                (unsigned long long)h.record_step, (unsigned long long)steps);
    }

    /* ---- map stored regions to this build by ID ---- */
    for (i = 0; i < (uint32_t)nreg; i++) map[i] = NNKS_NREGIONS;
    for (i = 0; i < (uint32_t)nreg; i++) {
        uint32_t idx = nnks_index_of_id(v1 ? 0 : st[i].id, st[i].name);
        if (idx >= NNKS_NREGIONS) {
            ndropped++;
            fprintf(stderr, "state: note: stored region '%.16s' (%llu B) is not in"
                            " this build — dropped\n", st[i].name,
                    (unsigned long long)st[i].length);
            continue;
        }
        map[i] = idx;
    }

    /* ---- apply ---- */
    for (i = 0; i < (uint32_t)nreg; i++) {
        uint32_t idx = map[i];
        int rc;
        const NnkFieldRec *dir = NULL;
        uint32_t ndir = 0;
        if (idx >= NNKS_NREGIONS) continue;
        if (dirs && st[i].nfields) {
            size_t off = 0, k;
            for (k = 0; k < i; k++) off += st[k].nfields;
            if (off + st[i].nfields <= total_dir) {
                dir = dirs + off;
                ndir = st[i].nfields;
            }
        }
        rc = nnks_region_load(idx, payload + st[i].offset, (size_t)st[i].length,
                              st[i].version, dir, ndir, &rep[idx]);
        if (rc < 0) {
            free(payload);
            if (dirs) free(dirs);
            snprintf(detail, sizeof detail, "region '%s' could not be applied",
                     nnks_regions[idx].name);
            nnks_banner("REFUSED — the stored bytes cannot be applied", path, detail);
            return 1;
        }
        if (rc == 0) nexact++;
        else npartial++;
        napplied++;
    }
    free(payload);
    if (dirs) free(dirs);

    /* A region the file does not carry at all is defaulted, and says so. */
    for (i = 0; i < NNKS_NREGIONS; i++) {
        uint32_t k;
        int present = 0;
        for (k = 0; k < (uint32_t)nreg; k++) if (map[k] == i) present = 1;
        if (!present) {
            rep[i].present = 0;
            rep[i].stored_len = 0;
            rep[i].stored_version = 0;
            npartial++;
        }
    }
    if (!napplied) {
        nnks_banner("REFUSED — no region of this file applies to this build", path, NULL);
        return 1;
    }

    /* The pc the run resumes at is a field of `io`.  A v1 file has no field
     * directory, so the fields past its stored length cannot be located; say
     * so instead of letting `resume pc=` look like a cold boot. */
    {
        uint32_t k2, i2;
        int pc_lost = 0;
        for (i2 = 0; i2 < NNKS_NREGIONS; i2++) {
            if (strcmp(nnks_regions[i2].name, "io") != 0 || rep[i2].exact) continue;
            for (k2 = 0; k2 < rep[i2].plan.nnames; k2++)
                if (strcmp(rep[i2].plan.names[k2], "nnks_pc") == 0) pc_lost = 1;
            if (rep[i2].plan.missing + rep[i2].plan.partial > rep[i2].plan.nnames)
                pc_lost = 1;      /* more defaulted fields than names quoted */
        }
        if (pc_lost) {
            if (nnks_v1_pc) {
                nnks_pc = nnks_v1_pc;
                fprintf(stderr, "state: note: v1 file (no field directory): the saved"
                                " pc 0x%08X was rescued from the trailing 4 bytes of io"
                                " (nnks_pc is the last io field since Phase-5AQ)\n",
                        nnks_pc);
            } else {
                fprintf(stderr, "state: note: this file does not carry the saved pc in a"
                                " locatable form; the run enters at pc=0x%08X with the"
                                " restored memory\n", nnks_pc);
            }
        }
    }

    /* The step counter and the frame counter are part of io/display; a partial
     * restore must still resume at the SAVED step and frame, never at 0 (the
     * defect made every partial state look like a cold boot). */
    if (h.record_step && steps + 1 != h.record_step) {
        uint64_t want = h.record_step - 1;
        fprintf(stderr, "state: note: the saved step counter (%llu) was not restored;"
                        " resuming the count at %llu\n",
                (unsigned long long)steps, (unsigned long long)want);
        steps = want;
    }
#ifdef NNK_GX
    if (h.record_frames && nnks_frames() != (unsigned)h.record_frames) {
        fprintf(stderr, "state: note: the saved frame counter (%u) was not restored;"
                        " resuming at frame %llu\n",
                nnks_frames(), (unsigned long long)h.record_frames);
        gxd_frames = (unsigned)h.record_frames;
    }
#endif

    nnks_partial = (npartial > 0);
    if (nnks_partial) {
        fprintf(stderr, "state: **************************************************************\n");
        fprintf(stderr, "state: ***  PARTIAL SAVESTATE: %d of %d region(s) exact, %d"
                        " restored best-effort\n", nexact, napplied, npartial);
        for (i = 0; i < NNKS_NREGIONS; i++) nnks_report_line(i, &rep[i]);
        if (ndropped)
            fprintf(stderr, "state: %d stored region(s) do not exist in this build\n",
                    ndropped);
        fprintf(stderr, "state: ***  the guest resumes at the saved pc/step/frame, but the\n");
        fprintf(stderr, "state: ***  machine is NOT the saved one: the fields listed above\n");
        fprintf(stderr, "state: ***  keep this build's defaults.\n");
        fprintf(stderr, "state: ***  NNK_SAVESTATE_STRICT=1 refuses such a file instead.\n");
        fprintf(stderr, "state: **************************************************************\n");
    }
    fprintf(stderr, "state: loaded %s (%ld bytes, v%u, identity OK: rom %llu B"
                    " hdr_crc=0x%08X arm9_crc=0x%08X build=0x%08X%s;"
                    " restored step=%llu frame=%u, saved step=%llu frame=%llu)\n",
            path, fsz, h.version, (unsigned long long)h.rom_size,
            h.rom_hdr_crc, h.arm9_crc, h.build_fp,
            nnks_partial ? ", PARTIAL" : "",
            (unsigned long long)steps, nnks_frames(),
            (unsigned long long)h.record_step, (unsigned long long)h.record_frames);
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

/* Phase-5BX: the next step at which this module needs a per-step look (a
 * pending NNK_STATE_HASH_AT / NNK_SAVESTATE_SAVE_AT, which fire on the first
 * step >= the target), or ~0 when nothing is armed.  The idle-loop skip in
 * run() must never jump over it. */
static uint64_t nnks_next_step(void)
{
    uint64_t n = ~0ull;
    if (!nnks_ready) return n;
    if (nnks_save_at && !nnks_save_done && nnks_save_at < n) n = nnks_save_at;
    if (nnks_hash_at && !nnks_hash_done && nnks_hash_at < n) n = nnks_hash_at;
    return n;
}

/* F5 / F9 (see tools/nnk_input.inc's SDL filter). */
static void nnk_state_ui_key(int fkey)
{
    if (!nnks_ready) nnks_boot(0, NULL);
    if (fkey == 5) {
        nnks_write_file(nnks_path);
    } else if (fkey == 9) {
        int rc = nnks_load_file(nnks_path);
        if (rc == 0) fprintf(stderr, "state: F9 restored '%s'%s — continuing from"
                                     " step=%llu pc=0x%08X\n",
                            nnks_path, nnks_partial ? " (PARTIAL)" : "",
                            (unsigned long long)steps, last_pc);
        else if (rc > 0) fprintf(stderr, "state: F9 REFUSED '%s' — the run is"
                                         " unchanged (still the previous machine)\n",
                                nnks_path);
    }
}

static void nnks_boot(int argc, char **argv)
{
    const char *s;
    char a0[NNKS_PATH_MAX];
    if (nnks_ready) return;
    {
        const char *st = getenv("NNK_SAVESTATE_STRICT");
        nnks_strict = (st && st[0] == '1');
    }
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
    fprintf(stderr, "state: savestates enabled (F5 save / F9 load in NNK_GX=window;"
                    " %s; container v%u)\n",
            nnks_strict ? "STRICT layout check" : "tolerant restore of older layouts",
            NNKS_VERSION);
    fprintf(stderr, "state: dir=%s default=%s save_at=%llu hash_at=%llu build=0x%08X\n",
            nnks_dir, nnks_path,
            (unsigned long long)nnks_save_at, (unsigned long long)nnks_hash_at,
            nnks_build_fp());
    s = getenv("NNK_SAVESTATE_LAYOUT");
    if (s && s[0]) nnks_write_layout(s);
    s = getenv("NNK_SAVESTATE");
    if (s && s[0]) {
        if (nnks_load_file(s) == 0) {
            nnks_boot_load_done = 1;
            /* `steps` now starts at the recorded step, so the compile-time
             * budget (a run LENGTH) would stop on the first instruction.
             * Give the resumed run a full budget; an explicit NNK_STEP_LIMIT
             * is absolute and still wins (main parses it after this hook). */
            if (step_limit <= steps) step_limit = steps + STEP_LIMIT;
        } else {
            nnks_banner("BOOT FROM SAVESTATE FAILED — the run REPLAYS THE COLD BOOT",
                        s, "every measurement from here is a fresh boot, not the"
                        " saved machine (see the state: REFUSED line above)");
        }
    }
}

#endif /* NNK_STATE_INC */
