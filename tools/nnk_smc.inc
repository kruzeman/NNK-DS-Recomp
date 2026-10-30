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
static inline void smc_note(uint32_t a, uint32_t n) {
    uint32_t o, b, e;
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
        if (link) r[14] = (pc + 4u) | 1u;
        *npc = tgt & ~1u;
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
                if (i == 15u) *npc = lv & ~1u;
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
            r[14] = (pc + 4u) | 1u;
            *npc = (pc + 4u + (uint32_t)o) & ~1u;
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
            if (rd == 15u) { *npc = (smc_reg(rd, pc, 1) + r[rs]) & ~1u; return 1; }
            r[rd] = smc_reg(rd, pc, 1) + r[rs];
            return 1;
        case 1: (void)smc_dp(10, smc_reg(rd, pc, 1), smc_reg(rs, pc, 1), 1); return 1;
        case 2: tv = smc_reg(rs, pc, 1);
                if (rd == 15u) *npc = tv & ~1u; else r[rd] = tv;
                return 1;
        default: tv = smc_reg(rs, pc, 1);
                 if (hw & 0x0080u) r[14] = (pc + 2u) | 1u;
                 *npc = tv & ~1u; *costp = 3; return 1;
        }
    }
    if ((hw & 0xF800u) == 0x4800u) {                 /* format 6: LDR literal */
        rd = (hw >> 8) & 7u;
        r[rd] = ld32(((pc + 4u) & ~3u) + ((hw & 0xFFu) << 2));
        return 1;
    }
    if ((hw & 0xF000u) == 0x5000u) {                 /* formats 7/8/9 */
        unsigned L = (unsigned)((hw >> 11) & 1u);
        unsigned B = (unsigned)((hw >> 12) & 1u);
        rd = hw & 7u;
        base = r[(hw >> 3) & 7u];
        if (hw & 0x0800u) {                          /* 0101 R: register offset */
            unsigned ro = (unsigned)((hw >> 6) & 0x1Fu);
            ea = base + ((hw & 0x0400u) ? r[ro] : ro);
        } else {                                     /* 011: immediate offset */
            ea = base + (((hw >> 6) & 0x1Fu) << 2);
            B = (unsigned)((hw >> 12) & 1u);
        }
        if (hw & 0x1000u) return 0;                  /* format 8: signed/half */
        if (L) { r[rd] = B ? ld8(ea) : ld32(ea); *costp = 1; }
        else   { if (B) st8(ea, r[rd]); else st32(ea, r[rd]); *costp = 3; }
        return 1;
    }
    if ((hw & 0xF000u) == 0x6000u) {                 /* format 9: LDR/STR imm5 */
        unsigned L = (unsigned)((hw >> 11) & 1u), B = (unsigned)((hw >> 12) & 1u);
        rd = hw & 7u;
        ea = r[(hw >> 3) & 7u] + (((hw >> 6) & 0x1Fu) << 2);
        if (L) { r[rd] = B ? ld8(ea) : ld32(ea); *costp = 1; }
        else   { if (B) st8(ea, r[rd]); else st32(ea, r[rd]); *costp = 3; }
        return 1;
    }
    if ((hw & 0xF000u) == 0x8000u) {                 /* format 10: SP-relative */
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
    if ((hw & 0xFF00u) == 0xB000u) {                 /* format 12: ADD SP,#imm */
        r[13] += (hw & 0x7Fu) << 2;
        return 1;
    }
    if ((hw & 0xF600u) == 0xB400u) {                 /* PUSH */
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
    if ((hw & 0xF600u) == 0xBC00u) {                 /* POP */
        unsigned list = (hw & 0xFFu) | ((hw & 0x0100u) ? 0x8000u : 0u);
        uint32_t sp = r[13];
        unsigned k, idx = 0u;
        for (k = 0; k < 16u; k++) {
            if (!(list & (1u << k))) continue;
            tv = ld32(sp + idx * 4u);
            idx++;
            if (k == 15u) *npc = tv & ~1u;
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
    if (smc_trace_left > 0) {
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
int nnk_smc_exec_any(uint32_t *pcp) {
    uint32_t pc = *pcp, o, w, npc = 0, cost = 0;
    int thumb, rc;

    if ((pc & 0xFF000000u) != GBASE) return 0;
    o = pc & 0x3FFFFFu;
    if (o + 4u > GSIZE) return 0;
    w = (uint32_t)gmem[o] | ((uint32_t)gmem[o + 1u] << 8) |
        ((uint32_t)gmem[o + 2u] << 16) | ((uint32_t)gmem[o + 3u] << 24);
    smc_ntry++;
    smc_nany++;
    trace_pc(pc);
    thumb = nnk_pc_is_thumb(pc);
    rc = thumb ? smc_exec_thumb(pc, &npc, &cost) : smc_exec_arm(pc, w, &npc, &cost);
    if (!rc && thumb) {
        rc = smc_exec_arm(pc, w, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 0; }
    } else if (!rc && !thumb && (pc & 1u) == 0u) {
        rc = smc_exec_thumb(pc, &npc, &cost);
        if (rc) { smc_nfb++; thumb = 1; }
    }
    if (!rc) return 0;
    smc_ninsn++;
    card_tick(cost);
    if (smc_trace_left > 0) {
        smc_trace_left--;
        fprintf(stderr, "smc: on-demand step=%llu pc=0x%08X word=0x%08X state=%s -> 0x%08X\n",
                (unsigned long long)steps, pc, w, thumb ? "thumb" : "arm", npc);
    }
    *pcp = npc;
    return 1;
}

#endif /* NNK_SMC_INC */
