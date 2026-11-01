/* ==========================================================================
 * Phase-4J — SMC: runtime image-state tracking for self-modifying code.
 *
 * WHY: the dispatch image has one `case` per PC, each body compiled from ONE
 * generation of the guest image.  Ni no Kuni's overlay loader DECRYPTS its
 * resident module in place, in ten passes, with the guest's own rolling-key
 * XOR routine (0x0215B468).  After each pass a range of the arena holds NEW
 * instruction words, so a statically compiled case is stale as soon as the
 * guest stores plaintext over it (Phase-4I: pc=0x00000000 at 0x0215B834
 * because that case had been compiled from the ciphertext generation).
 *
 * WHAT: the general mechanism, not a per-pass patch.
 *
 *   (1) GENERATION-0 SNAPSHOT.  `smc_arm()` copies the compile-time guest
 *       image — the exact bytes every case body was compiled from — at load
 *       time.  That copy is the compiled generation of every word.
 *
 *   (2) CODE-WINDOW WRITE TRACKING.  Every guest store routes through
 *       st8/st16/st32 (stores, DMA, SWP), which call `smc_note()`: it marks the
 *       32 KiB band of the main-RAM window dirty and bumps that band's
 *       GENERATION COUNTER (`smc_gen[]`).  Counters are unbounded (u32), so
 *       the ten decrypt passes are ten generations of one window — not a
 *       compile-time union of two images.
 *
 *   (3) DISPATCH-TIME INVALIDATION + RE-DECODE.  The dispatch loop calls
 *       `nnk_smc_try(&pc)` before the compiled band switch.  If the band has a
 *       nonzero generation AND the live word no longer matches the compiled
 *       generation, the compiled case is INVALID and the instruction is
 *       re-decoded from the CURRENT live RAM — "recompile from the current
 *       image" for a C recompiler means regenerating the body at run time with
 *       the decoder below.  Words that still match keep their compiled bodies,
 *       so the compiled fast path is untouched.
 *
 *   (4) HONEST FAILURE.  An encoding this decoder does not model HALTS with
 *       `smc: unsupported live instruction` plus the exact pc and word.  It
 *       never silently "succeeds".
 *
 * Env:  NNK_SMC=0          disable (compiled dispatch only = Phase-4I)
 *       NNK_SMC_TRACE=<n>  log the first n re-decoded instructions
 * ========================================================================== */
#ifndef NNK_SMC_INC
#define NNK_SMC_INC

#define SMC_BAND_SHIFT 15
#define SMC_BAND_N (GSIZE >> SMC_BAND_SHIFT)

static uint8_t *smc_img;                  /* generation 0: compiled image bytes */
static int smc_armed;                     /* tracking on (snapshot present) */
static int smc_f8log = -1;                /* NNK_F8LOG: live Thumb format-8 log cap */
static uint8_t smc_dirty[SMC_BAND_N + 2]; /* band has seen a guest store */
static uint32_t smc_gen[SMC_BAND_N + 2];  /* per-band store generation */
static uint32_t smc_icnt[SMC_BAND_N + 2]; /* re-decoded instructions per band */
static unsigned long long smc_ntry;       /* probes that landed in a dirty band */
static unsigned long long smc_nhit;       /* compiled generation found stale */
static unsigned long long smc_ninsn;      /* instructions re-decoded */
static unsigned long long smc_nhalt;      /* unsupported live instruction */
static unsigned long long smc_nfb;        /* state fallback taken */
static unsigned long long smc_nany;       /* Phase-4L: decode-on-demand hits */
static int smc_trace_left;                /* NNK_SMC_TRACE budget */
static unsigned long long smc_trace_from; /* NNK_SMC_TRACE_FROM: suppress
                                           * trace lines before this step.
                                           * The budget used to count down from
                                           * the FIRST live decode, so a probe
                                           * aimed at step 40M was always
                                           * spent long before the interesting
                                           * window (Phase-5J). */
static int smc_t_sticky;                  /* Phase-4Z: last resolved T bit */
/* Phase-5C: the processor state CARRIED by an interworking branch.  The T bit
 * is part of the ARM9 state, not of the PC: a Thumb `blx <imm>` branches to an
 * ARM-state target (ARM ARM A7.1.4: BLX always switches state), while `bl`
 * stays in Thumb.  The live re-decoder had only the compiled map and a sticky
 * last-state guess, so the target was re-decoded in the WRONG state: measured
 * at 0x020D2418 `blx 0x020E7968` (Thumb word 0xF015 0xEAA6) - the target holds
 * the ARM `push {r3,r4,r5,lr}` (0xE92D4038), the run re-decoded it as Thumb
 * (`stmdb sp!,{ip,lr}`), walked the pointer table at 0x02160394 for 25,284
 * PCs through the 0x0214xxxx-0x0217xxxx band and halted at 0x02178E9E on the
 * data word 0x1C36F000 (a "coprocessor" encoding = data executed as code).
 * The state is recorded at the branch and honoured for exactly the next
 * dispatch (the step stamp keeps a loop revisit from inheriting it). */
static uint32_t smc_t_pc = 0xFFFFFFFFu;
static int smc_t_val = -1;
static unsigned long long smc_t_step;
static void smc_branch_mode(uint32_t raw_target) {
    smc_t_pc = raw_target & ~1u;
    smc_t_val = (int)(raw_target & 1u);
    smc_t_step = steps;
}
static int smc_carried_thumb(uint32_t pc, int *state) {
    if (smc_t_val < 0 || smc_t_pc != pc || steps != smc_t_step + 1ull) return 0;
    *state = smc_t_val;
    return 1;
}

/* Phase-5X: the PIC-base register.  A Thumb `add rd, pc` (format 5 case 0)
 * writes Align(pc+4,4) into rd.  That value is EVEN and can be exactly the
 * address of a later instruction group, so `smc_state_from_regs` below read it
 * as an ARM-state interworking target and re-decoded a pure Thumb stream in
 * ARM state.
 *   Measured (Phase-5X): 0x020DDE9A `add r3, pc` (hw 0x447B) set
 *   r3 = Align(0x020DDE9E,4) = 0x020DDE9C; the very next dispatch was
 *   0x020DDE9C, smc_state_from_regs found r3 == pc -> "ARM", and the ARM
 *   reading of the Thumb jump-table prologue walked 4-byte steps down the
 *   halfword table (0x88DB, 0x041B, 0x141B, 0x449F, 0x0006, ...) until the ARM
 *   decode of 0x020DDF08 (Thumb-2 `f753 e838`) branched to pc=0 — the
 *   `untranslated pc=0x00000000` halt with r13=r14=0 at step 331,706,950.
 * The tag is honoured for exactly one dispatch (same contract as
 * smc_t_pc/smc_t_step): the value only ever exists as a target on the very
 * next instruction, because the following PC-relative load consumes it. */
static int smc_pcbase_reg = -1;
static unsigned long long smc_pcbase_step;
static void smc_note_pcbase(int rd) {
    smc_pcbase_reg = rd;
    smc_pcbase_step = steps;
}
static int smc_is_pcbase(int k) {
    return smc_pcbase_reg == k && steps == smc_pcbase_step + 1ull;
}

/* Phase-5J: the compiled image records the state of an INTERWORKING transfer.
 * The recompiler's emitted `pop {..,pc}` / `ldm ..,{..,pc}` sets
 *     pc = _t & ~1u;
 * and drops bit0 — the only place the target's state exists (melonDS
 * A_POP/A_LDM JumpTo(pc) with bit0).  The live re-decoder then had no state
 * for the target at all and fell back to `smc_t_sticky` (the last on-demand
 * decode's state, arbitrarily stale).  Emitted cases now call this hook, which
 * is exactly the same one-step-scoped carry the live decoder uses for its own
 * interworking branches (Phase-5C).  Non-static: the band TUs call it.
 * Measured: 0x020F5C88 `pop {r3,r4,r5,r6,r7,pc}` returned to 0x020F5CC4, the
 * live decoder ran the pop's Thumb bytes as an ARM `ldrsh`+`pc=pc+4`, sp was
 * then clobbered by an ARM `andcs sp,r1,r1` at 0x020F5CE8 and the frame's
 * epilogue popped 0 into pc. */
void nnk_smc_note_branch(uint32_t raw) { smc_branch_mode(raw); }

/* Emitted next to the dispatch image by tools/split_dispatch.py.  A mono
 * (recompile-all -> cc) build has no generated map, so fall back to ARM-only
 * when the header is absent — the mono path is the regression baseline. */
#if defined(__has_include)
#  if __has_include("nnk_smc_map.h")
#    include "nnk_smc_map.h"
#  endif
#endif
#ifndef NNK_SMC_MAP_H
#define NNK_SMC_MAP_H 1
static inline int nnk_pc_is_thumb(uint32_t pc) { (void)pc; return 0; }
#endif

/* Phase-4Z: recover the processor state for a PC the map cannot classify.
 * nnk_pc_is_thumb() is built from the COMPILED cases whose emitter-written
 * fall-through is addr+2, so a PC the recompiler never rooted — measured:
 * the overlay vtable thunk 0x020D22BC and the dispatcher epilogue
 * 0x020D1A1E — reads as ARM, and the ARM decode of their Thumb bytes
 * silently SUCCEEDS (it never reaches the Thumb fallback, state_fallbacks=0),
 * producing npc=pc+4 garbage that fabricated a branch into the not-yet
 * downloaded module at 0x02988AA0 (halt step 39,001,655).
 * A register-indirect transfer strips bit0 into pc (`pc = _t & ~1u`, and
 * `*npc = tv & ~1u` in this decoder) but the SOURCE register still carries
 * it on the very next dispatch, so: whichever register holds the tagged
 * target for this pc decides the state.  Only consulted when the map says
 * "not Thumb", so every classified PC keeps its old behaviour and an
 * unclassified ARM PC with no matching register still falls back to ARM
 * (the Phase-4L 0x0215C034 case), with the existing decode-failure
 * fallback covering the rest. */
static int smc_state_from_regs(uint32_t pc)
{
    int k;
    for (k = 0; k < 15; k++) {
        uint32_t v = r[k];
        /* Phase-5X: a register that the PREVIOUS instruction filled from the
         * PC (`add rd, pc` PIC base) is not a branch target — its value is
         * Align(pc+4,4), which can equal this very pc and would otherwise
         * force an ARM decode of Thumb code. */
        if (smc_is_pcbase(k)) continue;
        if ((v & ~1u) == pc) return (int)(v & 1u);
    }
    return -1;                            /* no register carries this pc */
}

/* ---- generation-0 image segments ---------------------------------------
 * The COMPILED generation is the image `nnktool recompile-all` decoded, which
 * is NOT the file the runtime loads: the ARM9 binary is stored compressed and
 * the guest decompresses it into main RAM at run time (measured: 0x02027BA8
 * holds 0x018080B9 in arm9_stored_padded.bin and 0xE12FFF1E in the recompiler's
 * arm9.bin).  NNK_SMC_IMAGE therefore names the recompiler's inputs as
 * "addr:path[,addr:path]"; every word is compared against ITS segment. */
#define SMC_SEG_N 4
static struct { uint32_t addr; uint32_t len; uint8_t *data; } smc_seg[SMC_SEG_N];
static int smc_seg_n;

/* ---- generation-0 snapshot ---------------------------------------------- */
static void smc_arm(void) {
    const char *p = getenv("NNK_SMC");
    const char *img;
    if (p && p[0] == '0') {
        fprintf(stderr, "smc: disabled by NNK_SMC=0 (compiled dispatch only)\n");
        return;
    }
    img = getenv("NNK_SMC_IMAGE");
    if (!img || !img[0]) {
        fprintf(stderr, "smc: NNK_SMC_IMAGE unset; tracking off "
                "(want \"addr:path[,addr:path]\" of the recompiler inputs)\n");
        return;
    }
    while (*img && smc_seg_n < SMC_SEG_N) {
        char path[512];
        size_t n = 0;
        uint32_t addr, len;
        FILE *f;
        while (*img == ',' || *img == ' ') img++;
        if (!*img) break;
        addr = (uint32_t)strtoul(img, (char **)&img, 0);
        if (*img != ':') {
            fprintf(stderr, "smc: bad NNK_SMC_IMAGE segment \"%s\"\n", img);
            return;
        }
        img++;
        while (*img && *img != ',' && n + 1u < sizeof path) path[n++] = *img++;
        path[n] = 0;
        f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "smc: cannot open image %s\n", path); return; }
        fseek(f, 0, SEEK_END); len = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (len == 0u) { fclose(f); fprintf(stderr, "smc: empty image %s\n", path); return; }
        smc_seg[smc_seg_n].data = (uint8_t *)malloc(len);
        if (!smc_seg[smc_seg_n].data) { fclose(f); return; }
        if (fread(smc_seg[smc_seg_n].data, 1, len, f) != len) { fclose(f); return; }
        fclose(f);
        smc_seg[smc_seg_n].addr = addr;
        smc_seg[smc_seg_n].len = len;
        fprintf(stderr, "smc: generation-0 segment 0x%08X..0x%08X (%u bytes) %s\n",
                addr, addr + len, len, path);
        smc_seg_n++;
    }
    if (!smc_seg_n) return;
    smc_armed = 1;
    p = getenv("NNK_SMC_TRACE");
    if (p && p[0]) smc_trace_left = atoi(p);
    p = getenv("NNK_SMC_TRACE_FROM");
    if (p && p[0]) smc_trace_from = strtoull(p, NULL, 0);
    fprintf(stderr, "smc: armed, %d generation-0 segments, %d bands%s\n",
            smc_seg_n, (int)SMC_BAND_N, smc_trace_left ? ", tracing" : "");
}

/* Compile-time word at `pc` (absolute address); returns 0 when no segment
 * covers it, i.e. the dispatch case there was not compiled from an image word. */
static int smc_img_word(uint32_t pc, uint32_t *out, uint32_t *segoff) {
    int i;
    for (i = 0; i < smc_seg_n; i++) {
        uint32_t d = pc - smc_seg[i].addr;
        if (pc >= smc_seg[i].addr && d + 4u <= smc_seg[i].len) {
            const uint8_t *b = smc_seg[i].data + d;
            *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                   ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
            if (segoff) *segoff = d;
            return 1;
        }
    }
    return 0;
}

static void smc_stats(void) {
    int i, shown = 0;
    if (!smc_armed) return;
    fprintf(stderr, "smc: probes=%llu stale=%llu redecoded=%llu halts=%llu "
            "state_fallbacks=%llu on_demand=%llu\n",
            smc_ntry, smc_nhit, smc_ninsn, smc_nhalt, smc_nfb, smc_nany);
    for (i = 0; i < (int)SMC_BAND_N && shown < 12; i++) {
        if (!smc_gen[i]) continue;
        fprintf(stderr, "smc: band 0x%08X gen=%u redecoded=%u\n",
                (unsigned)(GBASE + ((uint32_t)i << SMC_BAND_SHIFT)),
                smc_gen[i], smc_icnt[i]);
        shown++;
    }
}

/* ---- code-window write tracking ----------------------------------------- */
/* Phase-5N: generic guest-store watch.  NNK_STOREWATCH="lo:hi" (+_PATH, _N)
 * logs every store whose byte range overlaps [lo,hi) with the RAW guest
 * address, the value it REPLACES (read through the guest data path, so the
 * DTCM/ITCM windows are honoured) and the executing PC.  It answers "who
 * wrote this guest word, and when" by measurement.  NNK_STOREWATCH_MASK=<m>
 * tests (addr & m) instead: with m=0x7FFF plus "0x50:0x58" it catches every
 * store that lands in the 32 KiB ITCM array's first 0x58 bytes whatever the
 * guest-side alias — and only stores that actually route into the ITCM
 * window (addr < itcm_size) are logged in that mode.
 * Lazy env parse on the first store; one integer test per store afterwards. */
static FILE *storewatch_fp = NULL;
static int storewatch_state = 0;          /* 0 unparsed, 1 off, 2 on */
static uint32_t storewatch_lo = 0, storewatch_hi = 0;
static uint32_t storewatch_mask = 0xFFFFFFFFu;
static long storewatch_n = 0, storewatch_limit = 2000;
static void storewatch_init(void) {
    const char *s = getenv("NNK_STOREWATCH");
    storewatch_state = 1;
    if (!s || !s[0]) return;
    { const char *q = s;
      while (*q == ' ' || *q == ',') q++;
      storewatch_lo = (uint32_t)strtoul(q, (char **)&q, 0);
      while (*q == ' ' || *q == ':' || *q == '-') q++;
      storewatch_hi = (uint32_t)strtoul(q, (char **)&q, 0);
      if (!storewatch_hi) storewatch_hi = storewatch_lo + 4u; }
    { const char *p = getenv("NNK_STOREWATCH_PATH");
      const char *m = getenv("NNK_STOREWATCH_N");
      const char *k = getenv("NNK_STOREWATCH_MASK");
      if (m && m[0]) storewatch_limit = (long)strtol(m, NULL, 0);
      if (k && k[0]) storewatch_mask = (uint32_t)strtoul(k, NULL, 0);
      if (p && p[0]) storewatch_fp = fopen(p, "w"); }
    if (!storewatch_fp) return;
    storewatch_state = 2;
    fprintf(stderr, "storewatch: 0x%08X..0x%08X mask=0x%08X limit=%ld\n",
            storewatch_lo, storewatch_hi, storewatch_mask, storewatch_limit);
}
static void storewatch_hit(uint32_t a, uint32_t n) {
    if (!storewatch_fp) return;
    /* lr names the caller of the routine that stores; r0-r3 the arguments
     * that selected this store; sp lets the stack frame be walked later. */
    fprintf(storewatch_fp,
            "W%u 0x%08X old=0x%08X pc=0x%08X lr=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X sp=0x%08X step=%llu\n",
            n * 8u, a, ld32(a), last_pc, r[14], r[0], r[1], r[2], r[3], r[13],
            (unsigned long long)steps);
    fflush(storewatch_fp);
    if (++storewatch_n >= storewatch_limit) {
        fclose(storewatch_fp);
        storewatch_fp = NULL; storewatch_state = 1;
    }
}
static inline void smc_note(uint32_t a, uint32_t n) {
    uint32_t o, b, e;
    /* Phase-5N store watch: inline range test first (two compares, same cost
     * shape as the Phase-4V probes below); the logging body is only reached
     * on a hit.  NNK_STOREWATCH unset leaves storewatch_state == 1. */
    if (!storewatch_state) storewatch_init();
    if (storewatch_state == 2) {
        uint32_t _sw = a & storewatch_mask;
        if (storewatch_mask != 0xFFFFFFFFu && a >= itcm_size) _sw = 0xFFFFFFFFu;
        if (_sw >= storewatch_lo && _sw < storewatch_hi) storewatch_hit(a, n);
    }
    /* Phase-4V probe: who writes the DTCM IRQ-vector slot and the halted
     * code word?  Fires on the exact bytes (mirrors included). */
    if (a >= 0x027E3FC0u && a <= 0x027E3FFFu)
        fprintf(stderr, "p4v: vslot store a=0x%08X n=%u step=%llu pc=0x%08X old=0x%08X\n",
                a, n, (unsigned long long)steps, last_pc, ld32(a));
    if ((a & 0xFF000000u) == 0x02000000u && (a & 0x3FFFFFu) <= 0x1E12Cu &&
        (a & 0x3FFFFFu) + (n ? n : 1u) > 0x1E12Cu)
        fprintf(stderr, "p4v: code store a=0x%08X n=%u step=%llu pc=0x%08X old=0x%08X\n",
                a, n, (unsigned long long)steps, last_pc, ld32(a));
    if (!smc_armed) return;
    if ((a & 0xFF000000u) != GBASE) return;   /* main-RAM code window only */
    o = a & 0x3FFFFFu;                        /* 4 MiB mirror mask (melonDS) */
    if (o >= GSIZE) return;
    e = n ? (o + n - 1u) : o;
    if (e >= GSIZE) e = GSIZE - 1u;
    for (b = o >> SMC_BAND_SHIFT; b <= (e >> SMC_BAND_SHIFT); b++) {
        if (b >= SMC_BAND_N) break;
        smc_dirty[b] = 1;
        smc_gen[b]++;
    }
}

/* ---- decode helpers ----------------------------------------------------- */
static inline uint32_t smc_reg(uint32_t idx, uint32_t pc, int thumb) {
    if (idx == 15u) return thumb ? ((pc + 4u) & ~3u) : (pc + 8u);
    return r[idx];
}

static int smc_cond(unsigned cond) {
    switch (cond) {
    case 0x0: return z != 0;                     /* EQ */
    case 0x1: return z == 0;                     /* NE */
    case 0x2: return c != 0;                     /* CS */
    case 0x3: return c == 0;                     /* CC */
    case 0x4: return n != 0;                     /* MI */
    case 0x5: return n == 0;                     /* PL */
    case 0x6: return v != 0;                     /* VS */
    case 0x7: return v == 0;                     /* VC */
    case 0x8: return c != 0 && z == 0;           /* HI */
    case 0x9: return c == 0 || z != 0;           /* LS */
    case 0xA: return n == v;                     /* GE */
    case 0xB: return n != v;                     /* LT */
    case 0xC: return z == 0 && n == v;           /* GT */
    case 0xD: return z != 0 || n != v;           /* LE */
    case 0xE: return 1;                          /* AL */
    default:  return 0;                          /* NV */
    }
}

/* Data-processing result + flags, mirroring recompile.rs Kind::Dp exactly
 * (64-bit intermediates for carry/overflow). */
static inline uint32_t smc_dp(unsigned op, uint32_t rn, uint32_t rm, int setflags) {
    uint32_t rr;
    uint64_t uw;
    int64_t sw;
    switch (op) {
    case 0:  rr = rn & rm;  break;
    case 1:  rr = rn ^ rm;  break;
    case 2:  rr = rn - rm;  break;
    case 3:  rr = rm - rn;  break;
    case 4:  rr = rn + rm;  break;
    case 5:  rr = rn + rm + c; break;
    case 6:  rr = rn - rm - (c ^ 1u); break;
    case 7:  rr = rm - rn - (c ^ 1u); break;
    case 8:  rr = rn & rm;  break;
    case 9:  rr = rn ^ rm;  break;
    case 10: rr = rn - rm;  break;
    case 11: rr = rn + rm;  break;
    case 12: rr = rn | rm;  break;
    case 13: rr = rm;       break;
    case 14: rr = rn & ~rm; break;
    default: rr = ~rm;      break;
    }
    if (!setflags) return rr;
    z = (rr == 0);
    n = (rr >> 31) & 1u;
    switch (op) {
    case 2: case 6: case 10:
        uw = (uint64_t)rn - (uint64_t)rm - (op == 6 ? (uint64_t)(c ^ 1u) : 0u);
        sw = (int64_t)(int32_t)rn - (int64_t)(int32_t)rm - (op == 6 ? (int64_t)(c ^ 1u) : 0);
        c = (uw >> 32) ? 0u : 1u;
        v = (sw > (int64_t)0x7FFFFFFF || sw < -(int64_t)0x80000000) ? 1u : 0u;
        break;
    case 3: case 7:
        uw = (uint64_t)rm - (uint64_t)rn - (op == 7 ? (uint64_t)(c ^ 1u) : 0u);
        sw = (int64_t)(int32_t)rm - (int64_t)(int32_t)rn - (op == 7 ? (int64_t)(c ^ 1u) : 0);
        c = (uw >> 32) ? 0u : 1u;
        v = (sw > (int64_t)0x7FFFFFFF || sw < -(int64_t)0x80000000) ? 1u : 0u;
        break;
    case 4: case 11:
        uw = (uint64_t)rn + (uint64_t)rm;
        sw = (int64_t)(int32_t)rn + (int64_t)(int32_t)rm;
        c = (uint32_t)(uw >> 32);
        v = (sw > (int64_t)0x7FFFFFFF || sw < -(int64_t)0x80000000) ? 1u : 0u;
        break;
    case 5:
        uw = (uint64_t)rn + (uint64_t)rm + (uint64_t)c;
        sw = (int64_t)(int32_t)rn + (int64_t)(int32_t)rm + (int64_t)c;
        c = (uint32_t)(uw >> 32);
        v = (sw > (int64_t)0x7FFFFFFF || sw < -(int64_t)0x80000000) ? 1u : 0u;
        break;
    default:
        break;    /* logical ops leave C/V alone (the shifted form sets C) */
    }
    return rr;
}

/* ---- ARM ---------------------------------------------------------------- */
/* 1 = executed (npc/cost written), 0 = encoding not modelled. */
static int smc_exec_arm(uint32_t pc, uint32_t w, uint32_t *npc, uint32_t *costp) {
    unsigned cond = (unsigned)(w >> 28);
    uint32_t next = pc + 4u;
    uint32_t rn, rd, rm, tv, ea, base, off, addr;
    unsigned i, k, cnt;
    int setflags;

    *npc = next; *costp = 1;
    if (cond == 0xFu) {
        if ((w & 0x0E000000u) == 0x0A000000u) {        /* BLX immediate */
            uint32_t o = ((w & 0x00FFFFFFu) << 2) | ((w >> 23) & 2u);
            int32_t so = (int32_t)(o << 6) >> 6;       /* sign-extend 26 bits */
            r[14] = pc + 4u;
            *npc = (pc + 8u + (uint32_t)so) & ~1u;
            /* Phase-5I: ARM-state BLX(1) SWITCHES TO THUMB (ARM ARM A7.1.4:
             * "BLX <label> ... the T bit is set").  The old call tagged the
             * target as ARM, so 0x0207CAC0 `blx 0x020F5C98` (word 0xFA01E474)
             * ran the Thumb prologue `push {r3,r4,r5,r6,r7,lr}` as ARM
             * 0x1C05B5F8, walked 4-byte steps to the Thumb pair
             * `F754 EAC6` and branched to 0x012B39FC (the ARM reading of
             * those bytes) — the run's first "untranslated pc". */
            smc_branch_mode(*npc | 1u);
            *costp = 3;
            return 1;
        }
        return 0;                                      /* PLD / undefined space */
    }
    if (!smc_cond(cond)) return 1;                     /* condition failed */

    /* BX / BLX (register): 0x012FFF1m / 0x012FFF3m.  Mirrors the emitter
     * (nnktool recompile.rs Kind::Bx): the dispatch key is the EVEN target
     * address and the instruction state is implied by the case that exists
     * there; BLX links the next instruction with bit0 set so the callee
     * returns to Thumb.  Phase-4K: without this the encoding fell through to
     * the data-processing branch below and was silently decoded as
     * `teq pc, rm, lsr pc` (op=9, rd=15), which is why the run fell through
     * 0x0215B700 (E12FFF3C blx r12) instead of calling the decrypt helper at
     * 0x0215B528 — measured first divergence vs melonDS (step 13,142,583). */
    if ((w & 0x0FFFFF30u) == 0x012FFF10u ||
        (w & 0x0FFFFF30u) == 0x012FFF30u) {
        unsigned link = (w & 0x30u) == 0x30u;
        unsigned bxm = (unsigned)(w & 0xFu);
        uint32_t tgt = (bxm == 15u) ? (pc + 8u) : r[bxm];
        /* Phase-5D: ARM BLX (register) links pc+4 with bit0 CLEAR.  melonDS
         * A_BLX_REG (src/ARMInterpreter_Branch.cpp:57): `u32 lr = R[15]-4;
         * JumpTo(Rm); R[14] = lr;` - R[15] is pc+8 in ARM state, so lr = pc+4
         * and no interworking bit is added.  The old `| 1u` tagged the return
         * address Thumb; with the Phase-5D LDM interworking carry that made a
         * `pop {pc}` return to an ARM caller in Thumb state (measured at
         * 0x0215AC20, whose bytes are ARM `E1A02005`, step 13,146,7xx). */
        if (link) r[14] = pc + 4u;             /* BX (no L) leaves LR as-is */
        *npc = tgt & ~1u;
        smc_branch_mode(tgt);              /* Phase-5D: bx/blx rM interworks */
        *costp = 3;
        return 1;
    }

    /* multiply / long multiply: bits 27..24 = 0000, bits 7..4 = 1001.
     * Phase-4K: the old mask (0x0FC000F0) also covered bits 23..22, so it only
     * matched MUL/MLA and every LONG multiply (UMULL/SMULL/UMLAL/SMLAL, bit23)
     * fell through to the data-processing branch and was executed as an
     * add/orr — measured: 0x0215B100 `umull r0,r1,r0,r4` (E0810094) produced
     * r0=0 in native vs 0x00324415 in melonDS, which flipped the `bne` at
     * 0x0215B110 (the 63,438-instruction frontier). */
    if ((w & 0x0F0000F0u) == 0x00000090u) {
        if (w & 0x00800000u) {
            uint32_t rdhi = (w >> 16) & 0xFu, rdlo = (w >> 12) & 0xFu;
            uint32_t rs = (w >> 8) & 0xFu, rm2 = w & 0xFu;
            unsigned acc = (unsigned)((w >> 21) & 1u);
            if (w & 0x00400000u) {                     /* signed */
                int64_t p = (int64_t)(int32_t)r[rs] * (int64_t)(int32_t)r[rm2];
                if (acc) p += (int64_t)(((uint64_t)r[rdhi] << 32) | r[rdlo]);
                r[rdlo] = (uint32_t)(uint64_t)p;
                r[rdhi] = (uint32_t)((uint64_t)p >> 32);
            } else {
                uint64_t p = (uint64_t)r[rs] * (uint64_t)r[rm2];
                if (acc) p += ((uint64_t)r[rdhi] << 32) | r[rdlo];
                r[rdlo] = (uint32_t)p;
                r[rdhi] = (uint32_t)(p >> 32);
            }
            if (w & 0x00100000u) { z = (r[rdlo] == 0); n = (r[rdlo] >> 31) & 1u; }
            return 1;
        }
        rd = (w >> 16) & 0xFu; rn = (w >> 12) & 0xFu; rm = w & 0xFu;
        tv = (uint32_t)((uint64_t)r[(w >> 8) & 0xFu] * (uint64_t)r[rm]);
        if (w & 0x00200000u) tv += r[rn];
        r[rd] = tv;
        if (w & 0x00100000u) { z = (tv == 0); n = (tv >> 31) & 1u; }
        return 1;
    }
    if ((w & 0x0FBF0FFFu) == 0x010F0000u) {            /* MRS */
        rd = (w >> 12) & 0xFu;
        r[rd] = (w & 0x00400000u)
            ? spsr_bank[mode_cur]
            : ((n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x10u | cpsr_ctl);
        return 1;
    }
    if ((w & 0x0FB00000u) == 0x03200000u && (w & 0x000F0000u) != 0u) {  /* MSR imm */
        uint32_t mask = (w >> 16) & 0xFu;
        uint32_t rot = ((w >> 8) & 0xFu) * 2u;
        uint32_t imm8 = w & 0xFFu;
        tv = rot ? ((imm8 >> rot) | (imm8 << (32u - rot))) : imm8;
        if (w & 0x00400000u) {
            uint32_t e = spsr_bank[mode_cur];
            if (mask & 1u) e = (e & ~0xFFu) | (tv & 0xFFu);
            if (mask & 8u) e = (e & 0x0FFFFFFFu) | (tv & 0xF0000000u);
            spsr_bank[mode_cur] = e;
        } else {
            if (mask & 1u) cpsr_set_ctl(tv & 0xFFu);
            if (mask & 8u) { n = (tv >> 31) & 1u; z = (tv >> 30) & 1u;
                             c = (tv >> 29) & 1u; v = (tv >> 28) & 1u; }
        }
        return 1;
    }
    if ((w & 0x0FB0FFF0u) == 0x0120F000u) {            /* MSR reg */
        uint32_t mask = (w >> 16) & 0xFu;
        tv = r[w & 0xFu];
        if (w & 0x00400000u) {
            uint32_t e = spsr_bank[mode_cur];
            if (mask & 1u) e = (e & ~0xFFu) | (tv & 0xFFu);
            if (mask & 8u) e = (e & 0x0FFFFFFFu) | (tv & 0xF0000000u);
            spsr_bank[mode_cur] = e;
        } else {
            if (mask & 1u) cpsr_set_ctl(tv & 0xFFu);
            if (mask & 8u) { n = (tv >> 31) & 1u; z = (tv >> 30) & 1u;
                             c = (tv >> 29) & 1u; v = (tv >> 28) & 1u; }
        }
        return 1;
    }
    if ((w & 0x0FFF0FF0u) == 0x016F0F10u) {            /* CLZ */
        r[(w >> 12) & 0xFu] = clz32(r[w & 0xFu]);
        return 1;
    }
    if ((w & 0x0FB00FF0u) == 0x01000090u) {            /* SWP / SWPB */
        ea = r[(w >> 16) & 0xFu];
        tv = (w & 0x00400000u) ? ld8(ea) : ld32(ea);
        if (w & 0x00400000u) st8(ea, r[w & 0xFu]); else st32(ea, r[w & 0xFu]);
        r[(w >> 12) & 0xFu] = tv;
        *costp = 3;
        return 1;
    }
    if ((w & 0x0E000090u) == 0x00000090u) {            /* LDRH/STRH/LDRSB/LDRSH */
        unsigned sh = (unsigned)((w >> 5) & 3u);
        unsigned L = (unsigned)((w >> 20) & 1u);
        unsigned P = (unsigned)((w >> 24) & 1u);
        unsigned U = (unsigned)((w >> 23) & 1u);
        unsigned Wb = (unsigned)((w >> 21) & 1u);
        unsigned imm = (unsigned)((w >> 22) & 1u);
        rn = (w >> 16) & 0xFu; rd = (w >> 12) & 0xFu;
        base = smc_reg(rn, pc, 0);
        off = imm ? (((w >> 4) & 0xF0u) | (w & 0x0Fu)) : r[w & 0xFu];
        ea = P ? (U ? base + off : base - off) : base;
        if (sh == 1u) {
            if (L) { r[rd] = ld16(ea); *costp = 1; }
            else   { st16(ea, (uint16_t)r[rd]); *costp = 3; }
        } else if (sh == 2u) {
            r[rd] = (uint32_t)(int32_t)(int8_t)ld8(ea); *costp = 1;
        } else {
            r[rd] = (uint32_t)(int32_t)(int16_t)ld16(ea); *costp = 1;
        }
        if (!P || Wb) r[rn] = U ? base + off : base - off;
        return 1;
    }
    if ((w & 0x0C000000u) == 0x04000000u) {            /* single data transfer */
        unsigned I = (unsigned)((w >> 25) & 1u);
        unsigned P = (unsigned)((w >> 24) & 1u);
        unsigned U = (unsigned)((w >> 23) & 1u);
        unsigned B = (unsigned)((w >> 22) & 1u);
        unsigned Wb = (unsigned)((w >> 21) & 1u);
        unsigned L = (unsigned)((w >> 20) & 1u);
        rn = (w >> 16) & 0xFu; rd = (w >> 12) & 0xFu;
        base = smc_reg(rn, pc, 0);
        if (I) off = sh_op(smc_reg(w & 0xFu, pc, 0), (unsigned)((w >> 5) & 3u),
                           (unsigned)((w >> 7) & 0x1Fu), c);
        else   off = w & 0xFFFu;
        ea = P ? (U ? base + off : base - off) : base;
        if (L) {
            tv = B ? ld8(ea) : ld32(ea);
            /* Phase-5D: melonDS ARMv5 A_LDR0 `if (Num==1) val &= ~0x1` then
             * JumpTo(val) - an LDR into PC does NOT interwork on the ARM9.
             * Measured (src/ARMInterpreter_LoadStore.cpp:95..121). */
            if (rd == 15u) *npc = tv & ~1u;
            else r[rd] = tv;
            *costp = 1;
        } else {
            if (B) st8(ea, r[rd]); else st32(ea, r[rd]);
            *costp = 3;
        }
        if (!P || Wb) r[rn] = U ? base + off : base - off;
        return 1;
    }
    if ((w & 0x0E000000u) == 0x08000000u) {            /* LDM / STM */
        unsigned P = (unsigned)((w >> 24) & 1u);
        unsigned U = (unsigned)((w >> 23) & 1u);
        unsigned Sb = (unsigned)((w >> 22) & 1u);
        unsigned Wb = (unsigned)((w >> 21) & 1u);
        unsigned L = (unsigned)((w >> 20) & 1u);
        unsigned list = (unsigned)(w & 0xFFFFu);
        int user_bank;
        rn = (w >> 16) & 0xFu;
        cnt = 0u;
        for (k = 0; k < 16u; k++) if (list & (1u << k)) cnt++;
        if (cnt == 0u) return 0;                       /* unpredictable */
        base = smc_reg(rn, pc, 0);
        user_bank = Sb && (list & 0x8000u) == 0;
        if (P && U)       addr = base + 4u;
        else if (P && !U) addr = base - cnt * 4u;
        else if (!P && U) addr = base;
        else              addr = base - cnt * 4u + 4u;
        k = 0u;
        for (i = 0; i < 16u; i++) {
            if (!(list & (1u << i))) continue;
            if (L) {
                uint32_t lv = ld32(addr + k * 4u);
                if (i == 15u) {
                    /* Phase-5J: melonDS A_LDM (ARMInterpreter_LoadStore.cpp
                     * :436) masks bit0 for a SINGLE-register list
                     * (`if (cpu->Num == 1) pc &= ~0x1;`) and otherwise lets
                     * JumpTo take the loaded word's bit0.  The unconditional
                     * smc_branch_mode(lv) fabricated an interworking on the
                     * one-register form. */
                    uint32_t raw = (cnt == 1u) ? (lv & ~1u) : lv;
                    *npc = raw & ~1u; smc_branch_mode(raw);
                }
                else if (user_bank && i == 13u) bank13_set_usr(lv);
                else if (user_bank && i == 14u) bank14_set_usr(lv);
                else r[i] = lv;
            } else {
                if (user_bank && i == 13u) st32(addr + k * 4u, bank13_get_usr());
                else if (user_bank && i == 14u) st32(addr + k * 4u, bank14_get_usr());
                else st32(addr + k * 4u, r[i]);
            }
            k++;
        }
        if (Wb) r[rn] = U ? base + cnt * 4u : base - cnt * 4u;
        *costp = L ? 1 : 3;
        return 1;
    }
    if ((w & 0x0E000000u) == 0x0A000000u) {            /* B / BL */
        uint32_t bo = ((w & 0x00FFFFFFu) << 2);
        int32_t so = (int32_t)(bo << 6) >> 6;
        if (w & 0x01000000u) r[14] = pc + 4u;
        *npc = (pc + 8u + (uint32_t)so) & ~3u;
        *costp = 3;
        return 1;
    }
    if ((w & 0x0F000000u) == 0x0F000000u) {            /* SWI */
        if (!hle_swi(w & 0x00FFFFFFu, pc, 0)) {
            halt_reason("smc: swi not implemented");
            step_limit = steps;
        }
        return 1;
    }
    if ((w & 0x0F000000u) == 0x0E000000u) {            /* coprocessor */
        unsigned cp = (unsigned)((w >> 8) & 0xFu);
        if ((w & 0x0F000010u) != 0x0E000010u) return 0;  /* CDP/LDC/STC */
        rd = (w >> 12) & 0xFu;
        if (cp == 15u) {
            uint32_t id = (((w >> 16) & 0xFu) << 8) | ((w & 0xFu) << 4) | ((w >> 5) & 7u);
            if (w & 0x00100000u) r[rd] = cp15_read(id);
            else cp15_write(id, r[rd]);
        } else if (w & 0x00100000u) {
            r[rd] = cp15_reg;
        }
        return 1;
    }
    if ((w & 0x0C000000u) == 0x00000000u) {            /* data processing */
        unsigned I = (unsigned)((w >> 25) & 1u);
        unsigned op = (unsigned)((w >> 21) & 0xFu);
        setflags = (int)((w >> 20) & 1u);
        rn = (w >> 16) & 0xFu; rd = (w >> 12) & 0xFu;
        if (I) {
            uint32_t imm8 = w & 0xFFu;
            unsigned rot = (unsigned)((w >> 8) & 0xFu) * 2u;
            if (rot) { tv = (imm8 >> rot) | (imm8 << (32u - rot)); carry_out = (imm8 >> (rot - 1u)) & 1u; }
            else     { tv = imm8; carry_out = c; }
        } else {
            unsigned kind = (unsigned)((w >> 5) & 3u);
            uint32_t rmv = smc_reg(w & 0xFu, pc, 0);
            if (w & 0x10u) tv = sh_op_reg(rmv, kind, r[(w >> 8) & 0xFu] & 255u, c);
            else           tv = sh_op(rmv, kind, (unsigned)((w >> 7) & 0x1Fu), c);
        }
        tv = smc_dp(op, smc_reg(rn, pc, 0), tv, setflags);
        if (!I && setflags && (op != 2u && op != 3u && op != 4u && op != 5u &&
                              op != 6u && op != 7u && op != 10u && op != 11u))
            c = carry_out;                             /* shift carry-out */
        if (op < 8u || op > 11u) {
            if (rd == 15u) {
                *npc = tv & ~1u;
                /* Phase-5D: melonDS A_DataProcessing writes PC with
                 * `JumpTo(res & ~1)` (src/ARMInterpreter_ALU.cpp:321) - no
                 * interworking on MOV/ADD/SUB pc, only the S-form restores
                 * CPSR.  Bit0 stays stripped, state unchanged. */
                if (setflags && op < 8u && mode_priv()) {   /* exception return */
                    uint32_t spsr = spsr_bank[mode_cur];
                    cpsr_set_ctl(spsr & 0xFFu);
                    n = (spsr >> 31) & 1u; z = (spsr >> 30) & 1u;
                    c = (spsr >> 29) & 1u; v = (spsr >> 28) & 1u;
                }
            } else {
                r[rd] = tv;
            }
        }
        return 1;
    }
    return 0;
}

/* ---- Thumb (Thumb-1 subset; Thumb-2 passes hand off with a halt) -------- */
static int smc_exec_thumb(uint32_t pc, uint32_t *npc, uint32_t *costp) {
    uint32_t hw = ld16(pc), hw2 = ld16(pc + 2u);
    uint32_t rd, rs, tv, ea, base;
    unsigned f;

    *npc = pc + 2u; *costp = 1;
    if ((hw & 0xF800u) == 0xE800u || (hw & 0xF800u) == 0xF000u ||
        (hw & 0xF800u) == 0xF800u) {
        if ((hw & 0xF800u) == 0xF000u && (hw2 & 0x8000u)) {   /* BL / BLX */
            uint32_t sg = (hw >> 10) & 1u, j1 = (hw2 >> 13) & 1u, j2 = (hw2 >> 11) & 1u;
            uint32_t i1 = (j1 ^ sg) ^ 1u, i2 = (j2 ^ sg) ^ 1u;
            int32_t o = (int32_t)(((sg << 24) | (i1 << 23) | (i2 << 22) |
                                   ((hw & 0x3FFu) << 12) | ((hw2 & 0x7FFu) << 1)));
            o = (o << 7) >> 7;
            /* Phase-5E: the BASE differs between the two forms (ARM ARM
             * ARMv5T, BL(1)/BLX(1) immediate):
             *   BL(1)  (suffix 11111): target = PC + offset, PC = addr + 4
             *   BLX(1) (suffix 11101): target = Align(PC,4) + offset
             * because BLX(1) calls ARM code and the encoding forces the low
             * offset bit to 0 (H).  Measured against binutils round-trip:
             * the ROM's F779 EFE6 at 0x020D2426 is `blx 0x0204C3F4`
             * (Align(0x020D242A,4) - 0x86034); with the unaligned base the
             * runtime decoded 0x0204C3F6 (2 bytes into the ARM function at
             * 0x0204C3F4) and executed its prologue in Thumb state. */
            {
                uint32_t base = (hw2 & 0x1000u) ? (pc + 4u) : ((pc + 4u) & ~3u);
                *npc = (base + (uint32_t)o) & ~1u;
            }
            r[14] = (pc + 4u) | 1u;
            /* Phase-5C: the suffix's bit 12 separates the forms - BL
             * (11111, bit12=1) stays in Thumb, BLX (11101, bit12=0) makes
             * the target ARM state. */
            smc_branch_mode(*npc | ((hw2 & 0x1000u) ? 1u : 0u));
            *costp = 3;
            return 1;
        }
        return 0;                                    /* Thumb-2 encodings */
    }
    f = (hw >> 11) & 0x1Fu;
    if ((hw & 0xE000u) == 0x0000u) {
        if ((hw & 0x1800u) != 0x1800u) {             /* format 1: shift imm */
            rd = hw & 7u; rs = (hw >> 3) & 7u;
            tv = sh_op(r[rs], (unsigned)((hw >> 11) & 3u), (unsigned)((hw >> 6) & 0x1Fu), c);
            r[rd] = tv; z = (tv == 0); n = (tv >> 31) & 1u; c = carry_out;
            return 1;
        }
        /* format 2: ADD/SUB reg/imm3 */
        rd = hw & 7u; rs = (hw >> 3) & 7u;
        tv = (hw & 0x0400u) ? (uint32_t)((hw >> 6) & 7u) : r[(hw >> 6) & 7u];
        r[rd] = (hw & 0x0200u) ? smc_dp(2, r[rs], tv, 1) : smc_dp(4, r[rs], tv, 1);
        return 1;
    }
    if ((hw & 0xE000u) == 0x2000u) {                 /* format 3: imm8 ops */
        rd = (hw >> 8) & 7u; tv = hw & 0xFFu;
        switch ((hw >> 11) & 3u) {
        case 0: r[rd] = smc_dp(13, 0, tv, 1); break;
        case 1: (void)smc_dp(10, r[rd], tv, 1); break;
        case 2: r[rd] = smc_dp(4, r[rd], tv, 1); break;
        default: r[rd] = smc_dp(2, r[rd], tv, 1); break;
        }
        return 1;
    }
    if ((hw & 0xFC00u) == 0x4000u) {                 /* format 4: ALU */
        rd = hw & 7u; rs = (hw >> 3) & 7u;
        switch ((hw >> 6) & 0xFu) {
        case 0:  r[rd] = smc_dp(0, r[rd], r[rs], 1); break;
        case 1:  r[rd] = smc_dp(1, r[rd], r[rs], 1); break;
        case 2:  r[rd] = sh_op(r[rd], 0, r[rs] & 255u, c);
                 z = (r[rd] == 0); n = (r[rd] >> 31) & 1u; c = carry_out; break;
        case 3:  r[rd] = sh_op(r[rd], 1, r[rs] & 255u, c);
                 z = (r[rd] == 0); n = (r[rd] >> 31) & 1u; c = carry_out; break;
        case 4:  r[rd] = sh_op(r[rd], 2, r[rs] & 255u, c);
                 z = (r[rd] == 0); n = (r[rd] >> 31) & 1u; c = carry_out; break;
        case 5:  r[rd] = smc_dp(5, r[rd], r[rs], 1); break;
        case 6:  r[rd] = smc_dp(6, r[rd], r[rs], 1); break;
        case 7:  r[rd] = sh_op(r[rd], 3, r[rs] & 255u, c);
                 z = (r[rd] == 0); n = (r[rd] >> 31) & 1u; c = carry_out; break;
        case 8:  (void)smc_dp(8, r[rd], r[rs], 1); break;
        case 9:  r[rd] = smc_dp(3, r[rd], r[rs], 1); break;
        case 10: (void)smc_dp(10, r[rd], r[rs], 1); break;
        case 11: (void)smc_dp(11, r[rd], r[rs], 1); break;
        case 12: r[rd] = smc_dp(12, r[rd], r[rs], 1); break;
        case 13: r[rd] = (uint32_t)((uint64_t)r[rd] * (uint64_t)r[rs]);
                 z = (r[rd] == 0); n = (r[rd] >> 31) & 1u; break;
        case 14: r[rd] = smc_dp(14, r[rd], r[rs], 1); break;
        default: r[rd] = smc_dp(15, r[rd], r[rs], 1); break;
        }
        return 1;
    }
    if ((hw & 0xFC00u) == 0x4400u) {                 /* format 5: hi-reg / BX */
        rd = (hw & 7u) | ((hw >> 4) & 8u);
        rs = (hw >> 3) & 0xFu;
        switch ((hw >> 8) & 3u) {
        case 0:
            /* Phase-5I: BOTH operands go through smc_reg, so `add rd, pc`
             * reads the PC (Thumb: (pc+4) & ~3) instead of the stale
             * register slot r[15].  Measured at 0x020D24E0
             * (`add r0, pc`, 0x4478): the old code used r[rs] = r[15] = 0,
             * so the jump-table base became r[0]'s old value; the table
             * load then returned 0 and the following `add pc, r0`
             * (0x020D24E8) fell through into the table, walking straight
             * into the 0x020D3250 halt stub. */
            tv = smc_reg(rd, pc, 1) + smc_reg(rs, pc, 1);
            /* Phase-5X: `add rd, pc` / `add rd, pc`-style PIC base — tag rd so
             * smc_state_from_regs cannot read the aligned PC as a target. */
            if (rs == 15u) smc_note_pcbase((int)rd);
            if (rd == 15u) { *npc = tv & ~1u; smc_branch_mode(*npc | 1u); return 1; }
            r[rd] = tv;
            return 1;
        case 1: (void)smc_dp(10, smc_reg(rd, pc, 1), smc_reg(rs, pc, 1), 1); return 1;
        case 2: tv = smc_reg(rs, pc, 1);
                if (rd == 15u) { *npc = tv & ~1u; smc_branch_mode(*npc | 1u); }
                else { if (rs == 15u) smc_note_pcbase((int)rd); r[rd] = tv; }
                return 1;
        default: tv = smc_reg(rs, pc, 1);
                 if (hw & 0x0080u) r[14] = (pc + 2u) | 1u;
                 *npc = tv & ~1u;
                 /* Phase-5X: BX rM interworks on tv's bit0; BLX rM (bit7) is
                  * always ARM state (ARM ARM A7.1.4) — the live decoder used
                  * to strip bit0 with no carry at all, so the target's state
                  * fell back to the stale sticky value. */
                 smc_branch_mode((hw & 0x0080u) ? (*npc) : tv);
                 *costp = 3; return 1;
        }
    }
    if ((hw & 0xF800u) == 0x4800u) {                 /* format 6: LDR literal */
        rd = (hw >> 8) & 7u;
        r[rd] = ld32(((pc + 4u) & ~3u) + ((hw & 0xFFu) << 2));
        return 1;
    }
    if ((hw & 0xF000u) == 0x5000u) {                 /* formats 7/8 */
        /* Phase-5L: the discriminator between format 7 (`0101 L B 0 Ro Rb Rd`,
         * LDR/STR/LDRB/STRB with REGISTER offset) and format 8 (`0101 H S 1
         * Ro Rb Rd`, halfword/signed with register offset) is bit 9 (0x0200),
         * and B (byte) is bit10 — NOT bit12, which is part of the 0x5 prefix
         * and therefore always 1 in this whole block.  The old arm derived
         * B/L from bit12/bit11, scaled the offset as an imm5 when bit11 was
         * clear, and then did `if (hw & 0x1000u) return 0;` — i.e. it refused
         * EVERY 0x5xxx word (bit12 is always set), so the live decoder could
         * not execute a plain `ldrb r1,[r0,r1]` at all.  Measured at
         * 0x020F6560 inside the Mobiclip player (word 0x1C025C41,
         * smcfail hw0=0x5C41 hw1=0x1C02): the run halted 2,255,768,127 steps
         * in with `dispatch to untranslated pc`.  Format 7 is implemented
         * here exactly as nnktool/src/decode.rs (bit9 test, B=bit10, offset
         * R[Ro], no shift); format 8 still returns 0 so an unimplemented
         * halfword form halts honestly instead of executing as a word. */
        if (hw & 0x0200u) {                          /* format 8: halfword/signed */
            /* Phase-5M: format 8 is `0101 H S 1 Ro Rb Rd` with bit 11 = H
             * and bit 10 = S: (H,S) = (0,0) STRH, (0,1) LDRSB, (1,0) LDRH,
             * (1,1) LDRSH — exactly what nnktool/src/decode.rs:1077-1085
             * decodes.  Phase-5L left this arm returning 0 (honest halt); it
             * is now implemented so the Mobiclip player's register-offset
             * halfword accesses execute instead of halting the run. */
            unsigned H = (unsigned)((hw >> 11) & 1u);
            unsigned S = (unsigned)((hw >> 10) & 1u);
            unsigned ro = (unsigned)((hw >> 6) & 7u);
            rd = hw & 7u;
            ea = r[(hw >> 3) & 7u] + r[ro];
            if (smc_f8log < 0) {
                const char *e = getenv("NNK_F8LOG");
                smc_f8log = (e && e[0]) ? atoi(e) : 0;
            }
            {   /* Phase-5M measurement hook: every live-decoded format 8,
                 * with the exact effective address and both values. */
                static int f8n;
                if (smc_f8log && f8n < smc_f8log) {
                    f8n++;
                    fprintf(stderr, "f8: n=%d step=%llu pc=0x%08X hw=0x%04X HS=%u%u "
                            "ro=%u rb=%u rd=%u ea=0x%08X r=0x%08X\n",
                            f8n, (unsigned long long)steps, pc, (unsigned)hw,
                            H, S, ro, (unsigned)((hw >> 3) & 7u), (unsigned)rd,
                            ea, r[rd]);
                }
            }
            if (H) {
                r[rd] = S ? (uint32_t)(int32_t)(int16_t)ld16(ea) : ld16(ea);
                *costp = 1;
            } else if (S) {
                r[rd] = (uint32_t)(int32_t)(int8_t)ld8(ea);
                *costp = 1;
            } else {
                st16(ea, (uint16_t)r[rd]);
                *costp = 3;
            }
            return 1;
        }
        {
            unsigned L = (unsigned)((hw >> 11) & 1u);
            unsigned B = (unsigned)((hw >> 10) & 1u);
            unsigned ro = (unsigned)((hw >> 6) & 7u);
            rd = hw & 7u;
            ea = r[(hw >> 3) & 7u] + r[ro];
            if (L) { r[rd] = B ? ld8(ea) : ld32(ea); *costp = 1; }
            else   { if (B) st8(ea, r[rd]); else st32(ea, r[rd]); *costp = 3; }
        }
        return 1;
    }
    if ((hw & 0xE000u) == 0x6000u) {                 /* format 9: LDR/STR/LDRB/STRB imm5 */
        unsigned L = (unsigned)((hw >> 11) & 1u), B = (unsigned)((hw >> 12) & 1u);
        rd = hw & 7u;
        /* byte forms (0x7000..0x7FFF, B=1) take imm5 as a BYTE offset; only
         * the word forms are word-scaled.  The mask used to be 0xF000, so the
         * whole LDRB/STRB half fell through as "unsupported" and the ARM
         * fallback silently executed those Thumb bytes as ARM (npc=pc+4),
         * measured at 0x020D22CA `strb r3,[r0,#15]` (Phase-4Z). */
        ea = r[(hw >> 3) & 7u] + (((hw >> 6) & 0x1Fu) << (B ? 0 : 2));
        if (L) { r[rd] = B ? ld8(ea) : ld32(ea); *costp = 1; }
        else   { if (B) st8(ea, r[rd]); else st32(ea, r[rd]); *costp = 3; }
        return 1;
    }
    if ((hw & 0xF000u) == 0x8000u) {                 /* format 8: LDRH/STRH imm5 */
        /* Phase-5I: the immediate halfword form `1000 L imm5 Rn Rd` lives in
         * 0x8000..0x8FFF and was swallowed by the format-10 SP-relative test
         * below (mask 0xF000 == 0x8000), so `ldrh r0,[r0,#6]` executed as
         * `ldr r0,[sp,#imm]`.  Measured at 0x020D24E2 inside the SDK card
         * state machine: the guest's jump-table halfword came back as
         * whatever sat at sp+0x300, the table index died, and `add pc, r0`
         * walked the table into the 0x020D3250 halt stub. */
        unsigned L = (unsigned)((hw >> 11) & 1u);
        rd = hw & 7u;
        ea = r[(hw >> 3) & 7u] + (((hw >> 6) & 0x1Fu) << 1);
        if (L) { r[rd] = ld16(ea); *costp = 1; }
        else   { st16(ea, (uint16_t)r[rd]); *costp = 3; }
        return 1;
    }
    if ((hw & 0xF000u) == 0x9000u) {                 /* format 10: SP-relative */
        unsigned L = (unsigned)((hw >> 11) & 1u);
        rd = (hw >> 8) & 7u;
        ea = r[13] + ((hw & 0xFFu) << 2);
        if (L) { r[rd] = ld32(ea); *costp = 1; } else { st32(ea, r[rd]); *costp = 3; }
        return 1;
    }
    if ((hw & 0xF000u) == 0xA000u) {                 /* format 11: ADD Rd,PC/SP */
        rd = (hw >> 8) & 7u;
        r[rd] = ((hw & 0x0800u) ? r[13] : ((pc + 4u) & ~3u)) + ((hw & 0xFFu) << 2);
        return 1;
    }
    if ((hw & 0xFF00u) == 0xB000u) {                 /* format 12: ADD/SUB SP,#imm */
        /* Phase-5J: bit7 of the low byte selects the direction (ARM ARM
         * A6.6.4: `1011 0000 0 imm7` = ADD SP, `1011 0000 1 imm7` = SUB SP);
         * both imm7 fields are <<2.  The old arm matched the whole 0xB000..0xB0FF
         * block as ADD, so every `sub sp,#imm` RAISED the stack pointer.
         * Measured at 0x020DDE72 (`sub sp,#16`, word 0xB084) in the frame that
         * ends at 0x020DDE6E: sp sat 0x20 too high, the `[sp,#8]`/`[sp,#12]`
         * locals read the wrong words and `pop {r4,r5,r6,r7,pc}` popped 0 into
         * pc (halt step 41,720,639). */
        if (hw & 0x80u) r[13] -= (hw & 0x7Fu) << 2;
        else            r[13] += (hw & 0x7Fu) << 2;
        return 1;
    }
    /* Phase-5A: format 13 is `1011 L 10 R <list>` and bit 11 is the L bit that
     * separates PUSH (0xB4xx / 0xB5xx) from POP (0xBCxx / 0xBDxx).  The mask
     * used to be 0xF600, which leaves bit 11 out, so every POP matched this
     * PUSH arm first and the POP arm below was unreachable dead code: a Thumb
     * `pop {r4,r5,r6,pc}` (0xBD70) pushed a frame and fell through instead of
     * returning.  Measured at the overlay dispatcher's epilogue 0x020D1A1E
     * (Phase-5A): the `push {r4,r5,r6,lr}` at 0x020D19B8 had stored the
     * correct link 0x020D1647, but the epilogue never popped it. */
    if ((hw & 0xFE00u) == 0xB400u) {                 /* PUSH */
        unsigned list = (hw & 0xFFu) | ((hw & 0x0100u) ? 0x4000u : 0u);
        uint32_t sp = r[13];
        unsigned k, cnt = 0u, idx = 0u;
        for (k = 0; k < 16u; k++) if (list & (1u << k)) cnt++;
        sp -= cnt * 4u;
        for (k = 0; k < 16u; k++) {
            if (!(list & (1u << k))) continue;
            st32(sp + idx * 4u, r[k]);
            idx++;
        }
        r[13] = sp; *costp = 3;
        return 1;
    }
    if ((hw & 0xFE00u) == 0xBC00u) {                 /* POP */
        unsigned list = (hw & 0xFFu) | ((hw & 0x0100u) ? 0x8000u : 0u);
        uint32_t sp = r[13];
        unsigned k, idx = 0u, cnt = 0u;
        for (k = 0; k < 16u; k++) if (list & (1u << k)) cnt++;
        for (k = 0; k < 16u; k++) {
            if (!(list & (1u << k))) continue;
            tv = ld32(sp + idx * 4u);
            idx++;
            if (k == 15u) {
                /* Phase-5J: melonDS T_POP (ARMInterpreter_LoadStore.cpp:745)
                 * is `if (cpu->Num==1) pc |= 0x1; cpu->JumpTo(pc);` — the
                 * forced Thumb tag belongs to a SINGLE-register pop only.  For
                 * a multi-register `pop {r3,r4,r5,r6,r7,pc}` JumpTo takes the
                 * loaded word's bit0, so returning to an ARM caller resumes in
                 * ARM.  Phase-5D made the |1u unconditional, which re-entered
                 * ARM code at 0x0207CAC4 (the ARM BLX caller of 0x020F5C98)
                 * in Thumb state: word 0xE587000C = `str r0,[r7,#12]` was run
                 * as two Thumb ops and the run halted at 0x0207D274. */
                uint32_t raw = (cnt == 1u) ? (tv | 1u) : tv;
                *npc = raw & ~1u;
                smc_branch_mode(raw);
            }
            else r[k] = tv;
        }
        r[13] = sp + idx * 4u; *costp = 1;
        return 1;
    }
    if ((hw & 0xF000u) == 0xC000u) {                 /* format 14: LDMIA/STMIA */
        unsigned L = (unsigned)((hw >> 11) & 1u);
        unsigned list = hw & 0xFFu, k, idx = 0u;
        uint32_t base2 = r[(hw >> 8) & 7u];
        for (k = 0; k < 8u; k++) {
            if (!(list & (1u << k))) continue;
            if (L) r[k] = ld32(base2 + idx * 4u);
            else   st32(base2 + idx * 4u, r[k]);
            idx++;
        }
        if (!(L && (list & (1u << ((hw >> 8) & 7u))))) r[(hw >> 8) & 7u] = base2 + idx * 4u;
        *costp = L ? 1 : 3;
        return 1;
    }
    if ((hw & 0xF000u) == 0xD000u) {
        if ((hw & 0xFF00u) == 0xDF00u) {             /* SWI */
            if (!hle_swi(hw & 0xFFu, pc, 1)) {
                halt_reason("smc: swi not implemented");
                step_limit = steps;
            }
            return 1;
        }
        if (smc_cond((unsigned)((hw >> 8) & 0xFu))) {  /* format 15: cond branch */
            int32_t bo = (int32_t)((hw & 0xFFu) << 1);
            *npc = pc + 4u + (uint32_t)((bo << 23) >> 23);
            *costp = 3;
        }
        return 1;
    }
    if ((hw & 0xF800u) == 0xE000u) {                 /* format 17: B */
        int32_t bo = (int32_t)((hw & 0x7FFu) << 1);
        *npc = pc + 4u + (uint32_t)((bo << 21) >> 21);
        *costp = 3;
        return 1;
    }
    return 0;
}

/* ---- dispatch hook ------------------------------------------------------ */
/* Called once per dispatch from the PC-dispatch loop, before the compiled band
 * switch.  Returns 1 when the instruction was executed here (pc updated), so
 * the loop counts the step and skips the band switch. */
int nnk_smc_try(uint32_t *pcp) {
    uint32_t pc = *pcp, o, w, iw, npc, cost;
    int thumb, rc;

    if (!smc_armed) return 0;
    if ((pc & 0xFF000000u) != GBASE) return 0;      /* main-RAM code window only */
    o = pc & 0x3FFFFFu;
    if (o >= GSIZE) return 0;
    if (!smc_dirty[o >> SMC_BAND_SHIFT]) return 0;
    smc_ntry++;
    w = (uint32_t)gmem[o] | ((uint32_t)gmem[o + 1u] << 8) |
        ((uint32_t)gmem[o + 2u] << 16) | ((uint32_t)gmem[o + 3u] << 24);
    if (!smc_img_word(pc, &iw, NULL)) return 0;     /* no compiled word here */
    if (w == iw) return 0;                          /* compiled generation current */
    smc_nhit++;
    smc_icnt[o >> SMC_BAND_SHIFT]++;
    smc_ninsn++;

    trace_pc(pc);
    thumb = nnk_pc_is_thumb(pc);
    { int st;
      if (smc_carried_thumb(pc, &st)) thumb = st;             /* Phase-5C */
      else if (!thumb) {
        /* Phase-5J: the stale-band hook reused the map + carried state only,
         * so a PC the map cannot classify (any case whose emitted flow is not
         * the addr+2 fall-through — every `pop {..,pc}`/branch — and every
         * unrooted PC) fell straight to the ARM decode, whose
         * Thumb-bytes-as-ARM decode succeeds silently.  Same resolution as
         * nnk_smc_exec_any (Phase-4Z): a register still carrying the tagged
         * target, else the last resolved state.
         * Measured: 0x020F5CC4 (`adds r0,r4,#0;` + `pop {..,pc}`) executed
         * here as one ARM `ldrsh` (word 0xBDF81C20) instead of two Thumb ops,
         * which turned sp into 0x02E7FFF0 at the next ARM `andcs sp,r1,r1`. */
        int m = smc_state_from_regs(pc);
        if (m < 0) m = smc_t_sticky;
        thumb = (m > 0);
      } }
    rc = thumb ? smc_exec_thumb(pc, &npc, &cost) : smc_exec_arm(pc, w, &npc, &cost);
    if (!rc && thumb) {
        rc = smc_exec_arm(pc, w, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 0; }
    } else if (!rc && !thumb && (pc & 1u) == 0u) {
        rc = smc_exec_thumb(pc, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 1; }
    }
    if (!rc) {
        smc_nhalt++;
        fprintf(stderr, "smc: unsupported live instruction pc=0x%08X img=0x%08X "
                "word=0x%08X state=%s band_gen=%u\n", pc, iw, w,
                thumb ? "thumb" : "arm", smc_gen[o >> SMC_BAND_SHIFT]);
        halt_reason("smc: unsupported live instruction");
        step_limit = steps;
        return 1;
    }
    card_tick(cost);
    smc_t_sticky = thumb;                 /* Phase-5J: this pc's resolved state */
    if (smc_trace_left > 0 && steps >= smc_trace_from) {
        smc_trace_left--;
        fprintf(stderr, "smc: redecoded step=%llu pc=0x%08X img=0x%08X word=0x%08X "
                "state=%s gen=%u -> 0x%08X\n",
                (unsigned long long)steps, pc, iw, w, thumb ? "thumb" : "arm",
                smc_gen[o >> SMC_BAND_SHIFT], npc);
    }
    *pcp = npc;
    return 1;
}

/* ---- Phase-4L: decode-on-demand for PCs with no compiled case ------------
 * The generation-0 image carries exactly the recompiled PC set (the recovery
 * roots).  A PC that melonDS executes but the roots never reached — measured
 * case: 0x0215C034, the `ldmfd sp!,{pc}` tail of the arena routine at
 * 0x0215C014, reached only after 13.2M ARM9 instructions — arrives at the
 * dispatch default with rc==0.  nnk_smc_try() cannot help there: it requires
 * a generation-0 word at the PC (smc_img_word) before it will re-decode, and
 * an unrooted PC has none.
 *
 * This hook decodes the LIVE word unconditionally (ARM first, Thumb fallback,
 * the same decoder the generation tracker uses) and executes it in place.
 * Returns 1 when executed (pc updated), 0 when the word decodes as neither
 * mode — the caller then halts with the honest "untranslated pc" report, so a
 * genuine data-as-code jump still stops the run instead of running wild.
 * Main-RAM window only (0x02000000..0x03800000), like nnk_smc_try. */
/* ==========================================================================
 * Phase-5T — HLE Mobiclip movie decode (host decode feeds the guest planes).
 *
 * MEASURED CONTRACT (PHASE5T.md):
 *   * the intro movie is data/movie/op_u.mods (MODS container, MobiClip video,
 *     YCoCg 4:2:0, 256x192, 1317 frames);
 *   * the guest's per-frame plane is a plain raster Y plane: melonDS's slot
 *     0x022C0880 == the reference decoder's Y plane of frame 130 byte-exactly
 *     (corr 1.0000, MAE 0.0, 0/49152 differences);
 *   * the player object is at 0x02278264 (fields +0x38 callback 0x01FF8540,
 *     +0x3C current source plane, +0x40 next source plane, +0x44 VRAM dst,
 *     +0x48 stride 0x200);
 *   * the codec state is at the callback's r0 (0x0227834C): +0x00 stream
 *     cursor, +0x04 width, +0x08 height, +0x0C..+0x20 the six Y-plane ring
 *     slots, +0x24..+0x38 the six chroma slots (Y+0xC010).
 *
 * HOOK SHAPE (the DQ8 ladder-skip recipe): the guest decode callback
 * 0x01FF8540 still runs unchanged — the queue, stream cursor, slot ring,
 * pacing and display state machine all advance exactly as before — and its
 * *pixel* output is replaced right after it returns to its caller
 * (0x0207B21C, the instruction after the `blx r1` at 0x0207B218): the host
 * frame for this decode step is copied into the player's source plane and
 * every ring slot, so the guest's own unmodified converter/compositor (proven
 * faithful in Phase-5P) presents host pixels.
 *
 * Frame index = decode steps since the movie session started (the guest
 * decodes exactly one source frame per step); a stream cursor that moves
 * backwards starts a new session at frame 0.
 *
 * Phase-5AA: the intro drives TWO independent movies, one per screen — the
 * callback's r0 alternates between two codec structs (0x0227834C and
 * 0x022E8B74 in the measured runs), each owned by its own player object
 * (player + 0x34 == codec; player + 0x38 callback; module code in overlay 13).
 * melonDS at ts 2.6179e9 holds player 0x02278264 (codec 0x0227834C, ring
 * 0x022C0880) whose plane is op_u frame 130 and player 0x022E8A8C (codec
 * 0x022E8B74, ring 0x023310A8) whose plane is op_d frame 130.  A single
 * global frame index reset on every codec change, so every present showed
 * frame 0.  Each codec now has its own stream state (index, cursor, source
 * file); source 1 = NNK_MOVIE_YUV, source 2 = NNK_MOVIE_YUV2 (defaults to
 * source 1), further sessions reuse source 1.
 *
 * OFF unless NNK_MOVIE_YUV names a raw yuv420p file (73728 B/frame). */
#define P5T_W 256
#define P5T_H 192
#define P5T_YSZ (P5T_W * P5T_H)
#define P5T_CSZ ((P5T_W / 2) * (P5T_H / 2))
#define P5T_FRAME (P5T_YSZ + 2 * P5T_CSZ)
#define P5T_PLAYER 0x02278264u
#define P5T_CB_ENTRY 0x01FF8540u
#define P5T_CB_RET 0x0207B21Cu
#define P5T_MAXSTREAM 4

/* Phase-5AA: one entry per codec (per movie), not one global cursor. */
typedef struct {
    uint32_t codec;      /* stream struct base = the callback's r0 */
    uint32_t cursor;     /* [codec] at the previous present of THIS stream */
    long index;          /* next frame index (-1 = session start) */
    long frames, presents, shortr;
    FILE *fp;
} P5tStream;

static int p5t_state;                 /* 0 unparsed, 1 off, 2 on */
static P5tStream p5t_str[P5T_MAXSTREAM];
static int p5t_nstr;
static uint32_t p5t_codec;            /* codec of the CURRENT decode step */
static uint8_t p5t_buf[P5T_FRAME];

static void p5t_init(void) {
    const char *p = getenv("NNK_MOVIE_YUV");
    FILE *probe;
    p5t_state = 1;
    if (!p || !p[0]) return;
    /* Phase-5AC: NNK_MOVIE_YUV must NAME a readable raw yuv420p file (73728 B
     * per 256x192 frame); it is not a boolean.  A bare truthy value ("1",
     * "on") used to switch the HLE ON with no source at all: p5t_stream()
     * then failed to fopen("1") for the FIRST codec, p5t_nstr stayed 0, and
     * the hook wrote nothing for the whole run — measured in Phase-5AC as
     * cfg1 (NNK_MOVIE_YUV=1) vs cfg0 (unset) producing 14/14 BYTE-IDENTICAL
     * display frames, i.e. exactly the HLE-off picture, while the startup
     * banner still claimed "hle src1=1".  Only a source that actually opens
     * enables the present path, and the fallback is announced. */
    probe = fopen(p, "rb");
    if (!probe) {
        fprintf(stderr, "movie: NNK_MOVIE_YUV=%s is not a readable yuv420p file"
                        " -> HLE OFF (the guest's own decoder runs).  Canonical"
                        " configuration: NNK_MOVIE_YUV=<op_u.yuv>"
                        " NNK_MOVIE_YUV2=<op_d.yuv>\n", p);
        return;
    }
    fclose(probe);
    p5t_state = 2;
    fprintf(stderr, "movie: hle src1=%s src2=%s plane=%dx%d\n",
            p, (getenv("NNK_MOVIE_YUV2") && getenv("NNK_MOVIE_YUV2")[0])
                   ? getenv("NNK_MOVIE_YUV2") : "(src1)",
            P5T_W, P5T_H);
}

/* Register a codec and open its frame source.  Stream 1 uses NNK_MOVIE_YUV,
 * stream 2 NNK_MOVIE_YUV2 (falling back to stream 1's file), later sessions
 * stream 1's file again — a session restart at a new codec address starts a
 * new stream at frame 0, exactly like a new movie session on the real player. */
static P5tStream *p5t_stream(uint32_t codec) {
    P5tStream *s;
    const char *p, *n;
    int i;
    for (i = 0; i < p5t_nstr; i++)
        if (p5t_str[i].codec == codec) return &p5t_str[i];
    if (p5t_nstr >= P5T_MAXSTREAM) {
        fprintf(stderr, "movie: no stream slot for codec 0x%08X (dropped)\n", codec);
        return NULL;
    }
    p = (p5t_nstr == 1) ? getenv("NNK_MOVIE_YUV2") : NULL;
    if (!p || !p[0]) p = getenv("NNK_MOVIE_YUV");
    s = &p5t_str[p5t_nstr];
    s->codec = codec;
    s->cursor = 0;
    s->index = -1;
    s->frames = 0;
    s->presents = 0;
    s->shortr = 0;
    s->fp = fopen(p, "rb");
    if (!s->fp) {
        fprintf(stderr, "movie: stream %d cannot open %s\n", p5t_nstr, p);
        return NULL;
    }
    n = getenv("NNK_MOVIE_FRAMES");
    s->frames = (n && n[0]) ? strtol(n, NULL, 0) : 0;
    if (s->frames <= 0) {
        fseek(s->fp, 0, SEEK_END);
        s->frames = ftell(s->fp) / P5T_FRAME;
        rewind(s->fp);
    }
    if (s->frames <= 0) s->frames = 1;
    fprintf(stderr, "movie: stream %d codec=0x%08X source=%s frames=%ld step=%llu\n",
            p5t_nstr, codec, p, s->frames, (unsigned long long)steps);
    p5t_nstr++;
    return s;
}

/* Callback entry: remember which codec this decode step belongs to. */
static void p5t_note_codec(uint32_t codec) {
    if (!p5t_state) p5t_init();
    if (p5t_state != 2) return;
    if (!p5t_stream(codec)) return;
    p5t_codec = codec;
}

/* Copy one 8-bit plane into a guest buffer; silently ignores addresses that
 * are not main RAM (the host decode never writes IO/VRAM/TCM directly).
 *
 * Phase-5W: a movie plane may only land inside the guest's MOVIE ARENA.  The
 * destinations this hook is handed come from the codec struct (+0x0C..+0x20,
 * the six Y ring slots) and from the player struct (+0x3C/+0x40, the source
 * planes).  Measured valid buffers in this session: melonDS's ring slot
 * 0x022C0880, the native cursors 0x022E48E8 / 0x02355110, the codec structs
 * 0x0227834C / 0x022E8B74 and the player 0x02278264 -> the arena is
 * 0x02100000..0x02400000.  At the first decode steps of a session the player's
 * source-plane fields are NOT yet populated ([player+0x3C] reads 0x20) and the
 * stale field dropped a full 256x192 plane on the SDK's OSi_ThreadInfo block:
 * measured in Phase-5W as the block's only write in 2.25e9..2.257e9 steps that
 * did not come through st8/st16/st32 - the guard exists because too many
 * things go through the store funnel to see it. */
#define P5T_ARENA_LO 0x02100000u
#define P5T_ARENA_HI 0x02400000u
static int p5t_plane(uint32_t dst, const uint8_t *src, uint32_t n) {
    uint32_t o;
    if ((dst & 0xFF000000u) != GBASE) return 0;
    o = dst & 0x3FFFFFu;
    if (o + n > GSIZE) return 0;
    if (dst < P5T_ARENA_LO || dst + n > P5T_ARENA_HI) {
        /* Log once per distinct destination: a wrong field must be visible,
         * not silently swallowed. */
        static uint32_t seen[8];
        static int nseen;
        int i, have = 0;
        for (i = 0; i < nseen; i++) if (seen[i] == dst) have = 1;
        if (!have && nseen < 8) {
            seen[nseen++] = dst;
            fprintf(stderr, "movie: plane REFUSED dst=0x%08X n=%u step=%llu"
                            " pc=0x%08X (outside movie arena 0x%08X..0x%08X)\n",
                    dst, n, (unsigned long long)steps, last_pc,
                    P5T_ARENA_LO, P5T_ARENA_HI);
        }
        return 0;
    }
    memcpy(gmem + o, src, n);
    return 1;
}

/* Callback return: replace this decode step's pixels with the host frame of
 * THIS stream.  Only the current codec's six Y ring slots are written (the
 * decoder's real output buffers, which the converter selects through
 * [player+0x5C][[player+0xC4]]), so the two concurrent movies never write
 * into each other's planes. */
static void p5t_present(void) {
    P5tStream *s = NULL;
    uint32_t cur, player;
    int i;
    if (p5t_state != 2) return;
    for (i = 0; i < p5t_nstr; i++)
        if (p5t_str[i].codec == p5t_codec) s = &p5t_str[i];
    if (!s || !s->fp) return;
    cur = ld32(p5t_codec);
    /* One source frame per decode step (measured: melonDS's ring holds
     * consecutive op_u/op_d frames 125..130 at f130).  The stream cursor
     * [codec] is NOT monotonic — it oscillates between two read positions
     * (0x022E48E8/0x022E6A88/0x022E4A58/0x022E6B28 in the measured run) —
     * so it must not be used to detect a session restart; a new session is a
     * NEW codec address, which registers as a new stream at frame 0. */
    if (s->index < 0) s->index = 0;
    else s->index++;
    s->cursor = cur;
    if (s->index >= s->frames) {           /* past EOF: hold the last frame */
        s->index = s->frames - 1;
    }
    if (fseek(s->fp, (long)s->index * P5T_FRAME, SEEK_SET) != 0) return;
    if (fread(p5t_buf, 1, P5T_FRAME, s->fp) != (size_t)P5T_FRAME) {
        s->shortr++;
        return;
    }
    /* The decoder's OUTPUT buffers are the codec's six Y ring slots
     * (codec+0x0C..+0x20).  The player's own source-plane fields are NOT
     * written here: overlay 13's present function (0x0207B2AC) fills
     * player+0x3C/+0x40 from the arrays at player+0x5C/+0x60 — whose six
     * entries ARE those ring slots (verified in both engines) — immediately
     * before it calls the converter, so a write there is both unnecessary and
     * (measured, Phase-5W) the exact field that once handed p5t_plane a stale
     * destination.  The six slots cover every ring index the converter can
     * select ([player+0xC4]). */
    for (i = 0; i < 6; i++)
        p5t_plane(ld32(p5t_codec + 0x0Cu + 4u * i), p5t_buf, P5T_YSZ);
    player = p5t_codec - 0xE8u;
    s->presents++;
    if (s->presents <= 8 || (s->presents % 60) == 0)
        fprintf(stderr, "movie: present stream=%d codec=0x%08X n=%ld frame=%ld"
                        " cursor=0x%08X player=0x%08X slot0=0x%08X step=%llu\n",
                (int)(s - p5t_str), p5t_codec, s->presents, s->index, cur,
                player, ld32(p5t_codec + 0x0Cu), (unsigned long long)steps);
    fflush(stderr);
}

/* ==========================================================================
 * Phase-5U probe — the B1/B2 measurement (NNK_P5U=1, off by default).
 *
 *   NNK_P5U=1            enable
 *   NNK_P5U_FROM=<step>  first step counted (default 0)
 *   NNK_P5U_EVERY=<n>    sample interval in steps (default 10,000,000)
 *   NNK_P5U_TOP=<n>      histogram entries reported at exit (default 24)
 *
 * Prints, per sample: step, cycles, vcount(), gxd_frames (NNK_GX builds), the
 * PC, the next 8 dispatched PCs (the loop body), and the witness words
 * player+0x30..+0x48 / +0xA4 plus the codec's first words.  At exit it prints
 * the top PCs of the window (the spin loop) and the callback entry/return
 * counts.  This is a measurement, not a model: it changes no guest state.
 */
static int p5u_state;                         /* 0 unparsed, 1 off, 2 on */
static uint64_t p5u_from, p5u_every = 10000000ull, p5u_next;
static int p5u_capture;                       /* print this many more PCs */
static long p5u_samples, p5u_top_n = 24;
static long p5u_cb_in, p5u_cb_ret;
#define P5U_HBITS 15
#define P5U_HSZ (1u << P5U_HBITS)
static uint32_t p5u_hpc[P5U_HSZ];
static uint32_t p5u_hit[P5U_HSZ];

static void p5u_report(void) {
    long i, k, shown = 0;
    const char *mp = getenv("NNK_P5U_MEM");
    if (mp && mp[0]) {
        /* Full post-run snapshot so the hot loop can be disassembled. */
        char p[512];
        FILE *f;
        snprintf(p, sizeof p, "%s.ram.bin", mp);
        f = fopen(p, "wb");
        if (f) { fwrite(gmem, 1, GSIZE, f); fclose(f); }
        snprintf(p, sizeof p, "%s.itcm.bin", mp);
        f = fopen(p, "wb");
        if (f) { fwrite(itcm, 1, ITCM_PHYS, f); fclose(f); }
        fprintf(stderr, "p5u: memory snapshot -> %s.{ram,itcm}.bin\n", mp);
    }
    fprintf(stderr, "p5u: report window=[%llu,%llu] cb_entry=%ld cb_ret=%ld"
                    " samples=%ld\n",
            (unsigned long long)p5u_from, (unsigned long long)steps,
            p5u_cb_in, p5u_cb_ret, p5u_samples);
    /* simple selection: repeatedly take the largest remaining count */
    for (k = 0; k < p5u_top_n; k++) {
        uint32_t best = 0;
        long bi = -1;
        for (i = 0; i < P5U_HSZ; i++)
            if (p5u_hit[i] > best) { best = p5u_hit[i]; bi = i; }
        if (bi < 0 || best == 0) break;
        fprintf(stderr, "p5u: hot #%ld pc=0x%08X hits=%u\n",
                ++shown, p5u_hpc[bi], p5u_hit[bi]);
        p5u_hit[bi] = 0;
    }
    fflush(stderr);
}

static void p5u_init(void) {
    const char *s = getenv("NNK_P5U");
    p5u_state = 1;                             /* off unless enabled */
    if (!s || !s[0] || s[0] == '0') return;
    s = getenv("NNK_P5U_FROM");
    if (s && s[0]) p5u_from = (uint64_t)strtoull(s, NULL, 0);
    s = getenv("NNK_P5U_EVERY");
    if (s && s[0]) p5u_every = (uint64_t)strtoull(s, NULL, 0);
    s = getenv("NNK_P5U_TOP");
    if (s && s[0]) p5u_top_n = strtol(s, NULL, 0);
    if (p5u_every == 0) p5u_every = 1;
    p5u_next = p5u_from;
    p5u_state = 2;
    atexit(p5u_report);
    fprintf(stderr, "p5u: on from=%llu every=%llu\n",
            (unsigned long long)p5u_from, (unsigned long long)p5u_every);
}

/* Called for EVERY dispatched instruction from trace_pc (the compiled
 * dispatch cases and nnk_smc_exec_any both go through it), so the histogram
 * covers both the translated and the live-decoded paths. */
static void p5u_track(uint32_t pc);
static void p5u_hook(uint32_t pc) {
    if (p5u_state == 0) p5u_init();
    if (p5u_state != 2) return;
    p5u_track(pc);
}

static void p5u_track(uint32_t pc) {
    if (steps < p5u_from) return;
    {
        uint32_t h = (pc * 2654435761u) >> (32 - P5U_HBITS);
        uint32_t i = h;
        for (;;) {
            if (p5u_hit[i] == 0 || p5u_hpc[i] == pc) {
                p5u_hpc[i] = pc; p5u_hit[i]++; break;
            }
            i = (i + 1u) & (P5U_HSZ - 1u);
            if (i == h) break;
        }
    }
    if (pc == P5T_CB_ENTRY) p5u_cb_in++;
    if (pc == P5T_CB_RET) p5u_cb_ret++;
    if (p5u_capture > 0) {
        fprintf(stderr, "p5u:   -> pc=0x%08X step=%llu\n", pc,
                (unsigned long long)steps);
        p5u_capture--;
    }
    if (steps >= p5u_next) {
        uint32_t p = P5T_PLAYER, c = ld32(p + 0x34u);
        p5u_samples++;
        p5u_next = steps + p5u_every;
        fprintf(stderr,
            "p5u: t step=%llu cyc=%llu vc=%u gxf=%u pc=0x%08X"
            " player[30]=%08X [34]=%08X [38]=%08X [3C]=%08X [40]=%08X"
            " [44]=%08X [48]=%08X [A4]=%08X"
            " codec[00]=%08X [04]=%08X [0C]=%08X [10]=%08X [20]=%08X"
            " r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X sp=%08X"
            " disp=%04X ie=%08X if=%08X ime=%u cpsr=%02X\n",
            (unsigned long long)steps, (unsigned long long)cycles, vcount(),
#ifdef NNK_GX
            gxd_frames,
#else
            0u,
#endif
            pc,
            ld32(p + 0x30u), c, ld32(p + 0x38u), ld32(p + 0x3Cu),
            ld32(p + 0x40u), ld32(p + 0x44u), ld32(p + 0x48u),
            ld32(p + 0xA4u),
            c ? ld32(c) : 0u, c ? ld32(c + 4u) : 0u,
            c ? ld32(c + 0x0Cu) : 0u, c ? ld32(c + 0x10u) : 0u,
            c ? ld32(c + 0x20u) : 0u,
            r[0], r[1], r[2], r[3], r[14], r[13],
            dispstat, ie, iflags, ime, cpsr_ctl);
        p5u_capture = 8;
        fflush(stderr);
    }
}

int nnk_smc_exec_any(uint32_t *pcp) {
    uint32_t pc = *pcp, o, w, npc = 0, cost = 0;
    int thumb, rc;


    if (pc >= ITCM_LO && pc < ITCM_HI) {
        /* Phase-5M: the ITCM window is a SECOND live-code region.  The band
         * dispatch routes ITCM PCs to the compiled nnk_part_itcm, but that
         * image only carries the ITCM PCs the recovery roots reached; an
         * unrooted ITCM PC (measured: 0x01FF8540, the Mobiclip player's
         * `blx r1` target at 0x0207B218, step 2,256,160,152) returned 0 from
         * the part and then died here, because this hook only knew the main
         * RAM window.  ITCM code is fetched from the live 32 KiB array the
         * guest itself filled (ld16/st16 already bind it), so decode it here
         * exactly like a main-RAM PC. */
        const uint8_t *q = itcm + (pc & (ITCM_PHYS - 1u));
        w = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
            ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
        /* Phase-5T: the movie decode callback's entry names the codec base
         * (r0 = 0x0227834C) — that is where the host frame source comes from. */
        if (pc == P5T_CB_ENTRY) p5t_note_codec(r[0]);
    } else {
        if ((pc & 0xFF000000u) != GBASE) return 0;
        o = pc & 0x3FFFFFu;
        if (o + 4u > GSIZE) return 0;
        w = (uint32_t)gmem[o] | ((uint32_t)gmem[o + 1u] << 8) |
            ((uint32_t)gmem[o + 2u] << 16) | ((uint32_t)gmem[o + 3u] << 24);
    }
    smc_ntry++;
    smc_nany++;
    trace_pc(pc);
    thumb = nnk_pc_is_thumb(pc);
    { int st; if (smc_carried_thumb(pc, &st)) { thumb = st; goto smc_mode_done; } }
    if (!thumb) {
        int m = smc_state_from_regs(pc);
        if (m < 0) m = smc_t_sticky;
        thumb = (m > 0);
    }
  smc_mode_done:;
    rc = thumb ? smc_exec_thumb(pc, &npc, &cost) : smc_exec_arm(pc, w, &npc, &cost);
    if (!rc && thumb) {
        rc = smc_exec_arm(pc, w, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 0; }
    } else if (!rc && !thumb && (pc & 1u) == 0u) {
        rc = smc_exec_thumb(pc, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 1; }
    }
    if (!rc) {
        /* Phase-5I diagnostic: name the exact word/state a live-decode failure
         * happens on (NNK_SMC_FAILLOG=1).  The generic "dispatch to untranslated
         * pc" line names the PC but not which decoder ran or what bytes it saw. */
        const char *fl = getenv("NNK_SMC_FAILLOG");
        if (fl && fl[0])
            fprintf(stderr, "smcfail pc=0x%08X word=0x%08X thumb_try=%d hw0=0x%04X hw1=0x%04X\n",
                    pc, w, thumb, (unsigned)(w & 0xFFFFu), (unsigned)((w >> 16) & 0xFFFFu));
        return 0;
    }
    smc_t_sticky = thumb;                 /* Phase-4Z: the T bit this pc resolved to */
    smc_ninsn++;
    card_tick(cost);
    if (smc_trace_left > 0 && steps >= smc_trace_from) {
        smc_trace_left--;
        fprintf(stderr, "smc: on-demand step=%llu pc=0x%08X word=0x%08X state=%s sticky=%d -> 0x%08X\n",
                (unsigned long long)steps, pc, w, thumb ? "thumb" : "arm", smc_t_sticky, npc);
    }
    *pcp = npc;
    /* Phase-5T: the callback returns to its caller (0x0207B21C, right after
     * the `blx r1` at 0x0207B218) — replace its pixel output with the host
     * frame for this decode step, before the guest converter reads the plane. */
    if (npc == P5T_CB_RET && pc >= ITCM_LO && pc < ITCM_HI) p5t_present();
    return 1;
}

/* ==========================================================================
 * Phase-5B — the ARM7's shared keypad/touch mirror at main-RAM 0x3FFFA8.
 *
 * MEASURED (melonDS memwatch capture `p5b_mel_top2.txt`, ARM7 write dispatch):
 * the ARM7 stores W16 0x2C00 to 0x027FFFA8 (main-RAM offset 0x3FFFA8; the
 * ARM9 reads it at 0x02FFFFA8) as a PERIODIC publish, first at master-cycle
 * ts 8,421,838 and then every ~267,000 cycles (267,088 / 266,158 / 267,983 /
 * ... — 267 writes before ts 27.0M, every one of them 0x00002C00; every
 * write comes from ARM7 pc 0x037FE688, i.e. one ARM7 handler).
 *
 * WHY IT MATTERS: the game's keypad EDGE detector at 0x02049B0C does
 *     ldrh ip,=0x02FFFFA8 ; ldrh r2,[ip] ; ... ; ldrh r3,=KEYINPUT
 *     orr  r2,r3,r2 ; eor r2,r2,ip,lsr#12 (=0x2FFF) ; and r2,r2,0x2FFF
 *     strh r2,[r0,#8]                       ; the "key state" word
 * i.e. it ORs its own KEYINPUT read (0x03FF idle: bits 0..9 = released) with
 * the ARM7's published word and inverts ONLY the bits the ARM7 marks as
 * hardware-present (0x2C00 = bits 10,11,13, bit 12 = pen).  With the word
 * left at 0 (no ARM7 model) the OR yields 0x03FF, the inverted word is
 * 0x2C00 instead of 0, and the edge detector believes keys 10/11/13 are held
 * from boot: object state 1 instead of 0, so 0x02049BDC branches into
 * 0x02049C18 where melonDS falls through to 0x02049BF4.  That single missing
 * ARM7 store is the whole Phase-5A divergence.
 *
 * TIMING: native `cycles` is a coarse ~1-per-instruction counter, melonDS ts
 * is the master cycle; the measured scale at the same program point is
 * 1.542 (mel ts 27,127,554 at native step 17,589,766).  The measured mel
 * numbers therefore land at native cycles 5,461,000 (first publish) and
 * 173,150 (cadence).  Only the VALUE is observable by the guest (it is a
 * constant), so the schedule just has to precede the first ARM9 read — the
 * measured first read is at cycle 17,589,7xx, ~12M cycles later.
 * ========================================================================== */
#define NNK_KP_MIRROR 0x02FFFFA8u
static uint64_t nnk_kp_next;
void arm7_publish_keypad(void) {
    if (nnk_kp_next == 0) nnk_kp_next = 5461000ull;
    if (cycles < nnk_kp_next) return;
    nnk_kp_next += 173150ull;
    st16(NNK_KP_MIRROR, 0x2C00u);        /* bits 10,11,13 = 1, pen (12) = 0 */
}

#endif /* NNK_SMC_INC */
