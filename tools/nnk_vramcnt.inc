/* tools/nnk_vramcnt.inc — Phase-4M: VRAMCNT_A..I bank-mapping model for the
 * ALREADY-GENERATED dispatch image (work/phase3a/arm9_thumb_ovl_itcm_p4j.c).
 *
 * Spliced into the generated main TU by tools/split_dispatch.py (included next
 * to nnk_boot.inc / nnk_dma.inc), and hooked into the prelude's IO accessors
 * by explicit line substitutions in that script.  nnktool/src/recompile.rs
 * carries the same model for the next image regeneration.
 *
 * WHY (measured, Phase-4L §8.3 + Phase-4M): 0x04000240..0x04000249 are the
 * per-bank VRAMCNT registers (melonDS GPU::Write8/16/32 -> MapVRAM_A..I).  The
 * generated image's IO model was STORE-ONLY and 8-BIT-ONLY:
 *
 *   io_write8_sw: vramcnt[a-0x04000240] = v;     (only width-1 stores land)
 *   io_write16_sw / io_write32_sw: no case at all -> the store is dropped
 *   io_read16 / io_read32: no case -> reads return 0
 *   io_read8: returns vramcnt[] (so only 8-bit stores are ever visible)
 *
 * The guest really does use the wider forms (melonDS p4l_mel_io.txt):
 *
 *   W32 0x04000240 0x00000000 ts=19908841   ; clear A..D in one store
 *   W16 0x04000248 0x00000000 ts=19908857   ; clear H..I in one store
 *   W8  0x04000240 0x00000080 ts=19909691   ; then per-bank programming
 *   ...
 *   W8  0x04000242 0x0000008B ts=20853882   ; AOBJ / ABG / BBG / BOBJ / ...
 *
 * so a 32/16-bit clear (and any readback of it) was invisible to the runtime
 * model.  This fragment makes the model WIDTH-CORRECT and READABLE, and adds
 * the measurement surface Phase-4M needs (write trace, per-window store
 * counts, final DISPCNT/VRAMCNT report).
 *
 * The MAPPING EFFECT itself is owned by gx_display (the merged gxrender API):
 * every IO write is already forwarded to gxd_io_write() with the guest's
 * width, and gx_display.c implements melonDS's MapVRAM_A..I table plus the
 * flat-view rebuild that the 2D compositor reads.  This fragment therefore
 * REPORTS the same mapping the renderer uses (nnk_vrc_dest()) so the log can
 * be diffed against melonDS instead of being trusted.
 *
 * Env:  NNK_VRC_TRACE=1   log every VRAMCNT write (address, width, value, A..I)
 *       (the final report prints unconditionally, one line group)
 */
#ifndef NNK_VRAMCNT_INC
#define NNK_VRAMCNT_INC

static unsigned nnk_vrc_writes;        /* VRAMCNT register stores */
static unsigned nnk_vrc_reads;         /* VRAMCNT register reads */
static unsigned nnk_vrc_stat_reads;    /* VRAMSTAT reads */
static unsigned nnk_vrc_win[3];        /* stores: 0 palette, 1 VRAM windows, 2 OAM */
static unsigned nnk_vrc_win5[5];       /* VRAM windows: ABG/BBG/AOBJ/BOBJ/LCDC */
static unsigned long long nnk_vrc_win_first[3], nnk_vrc_win_last[3];
static uint32_t nnk_vrc_dispcnt[2];    /* last DISPCNT seen (engine A, B) */
static int nnk_vrc_trace = -1;

/* 0x04000240..0x0400024B are the VRAMCNT bytes, WRAMCNT, and padding;
 * 0x0400024C/4D are unused, 0x0400024E is VRAMSTAT (read-only).
 *
 * Phase-5Y: the layout is NOT a flat 0x240+bank table.  melonDS
 * NDS.cpp ARM9Write8 (case list at 0x04000240): the nine banks are
 *   0x04000240..0x04000246 -> VRAMCNT_A..G
 *   0x04000247             -> WRAMCNT   (NOT a VRAM bank)
 *   0x04000248             -> VRAMCNT_H
 *   0x04000249             -> VRAMCNT_I
 * The old flat model put H at 0x247 and I at 0x248, so the guest's WRAMCNT
 * store (0x03) overwrote VRAMCNT_H, the real H store (0x82) landed on I, and
 * the real I store (0x83, "VRAM_I = engine-B BG extended palette") was dropped.
 * That left the engine-B BG layer reading a zero extended palette: the first
 * screen's panel/figure/caption rendered black while the OBJ prompt survived. */
static int nnk_vrc_in(uint32_t a) { return a >= 0x04000240u && a <= 0x0400024Bu; }

/* register byte -> VRAM bank index, or -1 for WRAMCNT / padding */
static int nnk_vrc_slot(uint32_t a)
{
    switch (a) {
    case 0x04000240: return 0;
    case 0x04000241: return 1;
    case 0x04000242: return 2;
    case 0x04000243: return 3;
    case 0x04000244: return 4;
    case 0x04000245: return 5;
    case 0x04000246: return 6;
    case 0x04000248: return 7;
    case 0x04000249: return 8;
    default:         return -1;     /* 0x247 WRAMCNT, 0x24A/0x24B unused */
    }
}

static void nnk_vrc_trace_init(void)
{
    if (nnk_vrc_trace < 0) {
        const char *s = getenv("NNK_VRC_TRACE");
        nnk_vrc_trace = (s && s[0] && !(s[0] == '0' && s[1] == '\0')) ? 1 : 0;
    }
}

/* Store `width` bytes of `v` (little-endian, as the guest wrote them) into the
 * VRAMCNT/WRAMCNT register file.  melonDS decodes wide stores by ADDRESS -
 * ARM9Write16 0x04000240 -> A,B; 0x04000242 -> C,D; 0x04000244 -> E,F;
 * 0x04000246 -> G,WRAMCNT; 0x04000248 -> H,I; ARM9Write32 0x04000240 -> A..D,
 * 0x04000244 -> E,F,G,WRAMCNT, 0x04000248 -> H,I - so each byte is routed
 * through the same slot table (a byte that lands on WRAMCNT stays out of the
 * VRAM bank file). */
static void nnk_vrc_write(uint32_t a, uint32_t v, int width)
{
    int i;
    nnk_vrc_trace_init();
    nnk_vrc_writes++;
    for (i = 0; i < width; i++) {
        int b = nnk_vrc_slot(a + (uint32_t)i);
        if (b >= 0) vramcnt[b] = (v >> (8 * i)) & 0xFFu;
        else if (a + (uint32_t)i == 0x04000247u) wramcnt_map = (uint8_t)((v >> (8 * i)) & 0xFFu);
    }
    if (nnk_vrc_trace) {
        fprintf(stderr, "vrc: W%d 0x%08X=0x%08X A..I=", width, a, v);
        for (i = 0; i < 9; i++) fprintf(stderr, "%02X", (unsigned)vramcnt[i]);
        fprintf(stderr, " step=%llu\n", (unsigned long long)steps);
    }
}

/* Register readback: melonDS ARM9Read8/16/32 serve the same slots, so a byte
 * read of 0x04000247 returns WRAMCNT and a 16-bit read of 0x04000248 returns
 * VRAMCNT_H | VRAMCNT_I<<8. */
static uint32_t nnk_vrc_read(uint32_t a, int width)
{
    int i;
    uint32_t r = 0;
    nnk_vrc_reads++;
    for (i = 0; i < width; i++) {
        uint32_t v = 0;
        int b = nnk_vrc_slot(a + (uint32_t)i);
        if (b >= 0) v = vramcnt[b] & 0xFFu;
        else if (a + (uint32_t)i == 0x04000247u) v = wramcnt_map & 0xFFu;
        r |= v << (8 * i);
    }
    return r;
}

/* VRAMSTAT (0x0400024E): bit0 = VRAM_C mapped to ARM7, bit1 = VRAM_D
 * (melonDS GPU::Read8 / WriteVRAMSTAT semantics: the ARM7-owned banks). */
static uint32_t nnk_vrc_stat(void)
{
    uint32_t s = 0;
    nnk_vrc_stat_reads++;
    if ((vramcnt[2] & 0x87u) == 0x82u) s |= 1u;
    if ((vramcnt[3] & 0x87u) == 0x82u) s |= 2u;
    return s;
}

/* Store accounting for the routed memory windows (measurement only). */
static void nnk_vrc_mem(uint32_t a)
{
    int k = 2;
    if (a >= 0x05000000u && a < 0x05000800u) k = 0;
    else if (a >= 0x06000000u && a < 0x068A0000u) {
        k = 1;
        if (a < 0x06200000u)      nnk_vrc_win5[0]++;
        else if (a < 0x06220000u) nnk_vrc_win5[1]++;
        else if (a < 0x06440000u) nnk_vrc_win5[2]++;
        else if (a < 0x06620000u) nnk_vrc_win5[3]++;
        else                      nnk_vrc_win5[4]++;   /* LCDC window 0x06800000+ */
    }
    if (nnk_vrc_win[k]++ == 0u) nnk_vrc_win_first[k] = (unsigned long long)steps;
    nnk_vrc_win_last[k] = (unsigned long long)steps;
}

/* DISPCNT shadow (engine A is in the runtime model; engine B is forwarded to
 * gx_display only, so record it here for the report). */
static void nnk_vrc_set_dispcnt(int n, uint32_t v) { nnk_vrc_dispcnt[n & 1] = v; }

/* POWCNT1 writes seen by the runtime model (0 = never written). */
static unsigned nnk_vrc_powcnt_n;
static uint32_t nnk_vrc_powcnt_v;
static void nnk_vrc_powcnt(uint32_t v) { nnk_vrc_powcnt_n++; nnk_vrc_powcnt_v = v & 0xFFFFu; }

/* ---- 2D register-block readback shadow (Phase-4M) -----------------------
 * WHY (measured, Phase-4M): the guest's display-init tail at 0x02041710.. is
 * a read-modify-write over BOTH DISPCNT registers:
 *
 *   R32 0x04000000 0x0001110B   ; engine A readback (the runtime does have it)
 *   W32 0x04000000 0x0021111B   ; native reproduces this exactly
 *   R32 0x04001000 0x80011003   ; engine B readback  <- NOT modelled
 *   W32 0x04001000 0x80211013   ; melonDS writes this; native wrote 0x00200010
 *
 * Engine B's whole 2D register block (0x04001000..0x0400106F) lives only in
 * gx_display (write-only from the runtime's side), so guest reads of it
 * returned 0 and every read-modify-write dropped the bits the guest had
 * already programmed -- engine B stayed in DS display mode 0 ("display off",
 * a white screen) instead of mode 1 with BG0/BG1 on.
 *
 * The shadow is the melonDS GPU2D register file as far as the guest can see
 * it: it records every store with the guest's exact address and width, and
 * serves reads for the two 2D blocks.  DISPSTAT/VCOUNT (offsets 0x04/0x06)
 * are NOT shadowed -- the runtime derives them from the live frame clock.
 */
static uint8_t nnk_sh_2d[2][0x70];

static int nnk_sh_addr(uint32_t a, int *eng, uint32_t *off)
{
    if (a >= 0x04000000u && a < 0x04000070u) { *eng = 0; *off = a - 0x04000000u; return 1; }
    if (a >= 0x04001000u && a < 0x04001070u) { *eng = 1; *off = a - 0x04001000u; return 1; }
    return 0;
}

static void nnk_sh_write(uint32_t a, uint32_t v, int width)
{
    int e, i;
    uint32_t off;
    if (!nnk_sh_addr(a, &e, &off)) return;
    if (off == 0x04u || off == 0x06u) return;      /* DISPSTAT / VCOUNT */
    for (i = 0; i < width && off + (uint32_t)i < 0x70u; i++)
        nnk_sh_2d[e][off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}

static int nnk_sh_read(uint32_t a, int width, uint32_t *out)
{
    int e, i;
    uint32_t off, r = 0;
    if (!nnk_sh_addr(a, &e, &off)) return 0;
    if (off == 0x04u || off == 0x06u) return 0;    /* live, not shadowed */
    for (i = 0; i < width && off + (uint32_t)i < 0x70u; i++)
        r |= (uint32_t)nnk_sh_2d[e][off + (uint32_t)i] << (8 * i);
    *out = r;
    return 1;
}

/* One bank -> destination name, per melonDS GPU::MapVRAM_A..I. */
static const char *nnk_vrc_dest(int bank, char *buf, size_t n)
{
    uint32_t c = vramcnt[bank];
    int ofs;
    if (!(c & 0x80u)) { snprintf(buf, n, "off"); return buf; }
    switch (bank) {
    case 0: case 1:
        ofs = (int)((c >> 3) & 3u);
        switch (c & 3u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "ABG[%d]", ofs); break;
        case 2: snprintf(buf, n, "AOBJ[%d]", (ofs & 1) * 8); break;
        default: snprintf(buf, n, "TEX[%d]", ofs); break;
        }
        break;
    case 2: case 3:
        ofs = (int)((c >> 3) & 7u);
        switch (c & 7u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "ABG[%d]", ofs); break;
        case 2: snprintf(buf, n, "ARM7[%d]", ofs & 1); break;
        case 3: snprintf(buf, n, "TEX[%d]", ofs & 3); break;
        case 4: snprintf(buf, n, "%s", bank == 2 ? "BBG" : "BOBJ"); break;
        default: snprintf(buf, n, "reserved%d", (int)(c & 7u)); break;
        }
        break;
    case 4:
        switch (c & 7u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "ABG[0..3]"); break;
        case 2: snprintf(buf, n, "AOBJ[0..3]"); break;
        case 3: snprintf(buf, n, "TEXPAL[0..3]"); break;
        case 4: snprintf(buf, n, "ABGEXT[0..3]"); break;
        default: snprintf(buf, n, "reserved%d", (int)(c & 7u)); break;
        }
        break;
    case 5: case 6: {
        int base = (int)(((c >> 3) & 1u) + (((c >> 3) & 2u) << 1));
        switch (c & 7u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "ABG[%d,%d]", base, base + 2); break;
        case 2: snprintf(buf, n, "AOBJ[%d,%d]", base, base + 2); break;
        case 3: snprintf(buf, n, "TEXPAL[%d]", base); break;
        case 4: snprintf(buf, n, "ABGEXT[%d]", (base & 1) << 1); break;
        case 5: snprintf(buf, n, "AOBJEXT"); break;
        default: snprintf(buf, n, "reserved%d", (int)(c & 7u)); break;
        }
        break;
    }
    case 7:
        switch (c & 3u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "BBG[0,1,4,5]"); break;
        case 2: snprintf(buf, n, "BBGEXT[0..3]"); break;
        default: snprintf(buf, n, "reserved%d", (int)(c & 3u)); break;
        }
        break;
    case 8:
        switch (c & 3u) {
        case 0: snprintf(buf, n, "LCDC"); break;
        case 1: snprintf(buf, n, "BBG[2,3,6,7]"); break;
        case 2: snprintf(buf, n, "BOBJ[0..7]"); break;
        default: snprintf(buf, n, "BOBJEXT"); break;
        }
        break;
    }
    return buf;
}

/* One-line-per-bank report at exit: the state the renderer used last. */
static void nnk_vrc_report(void)
{
    char d[9][24];
    int i;
    for (i = 0; i < 9; i++) nnk_vrc_dest(i, d[i], sizeof d[i]);
    fprintf(stderr, "vrc: writes=%u reads=%u stat_reads=%u stores: palette=%u vram=%u oam=%u\n",
            nnk_vrc_writes, nnk_vrc_reads, nnk_vrc_stat_reads,
            nnk_vrc_win[0], nnk_vrc_win[1], nnk_vrc_win[2]);
    fprintf(stderr, "vrc: store window first/last step: palette=%llu..%llu vram=%llu..%llu oam=%llu..%llu\n",
            nnk_vrc_win_first[0], nnk_vrc_win_last[0],
            nnk_vrc_win_first[1], nnk_vrc_win_last[1],
            nnk_vrc_win_first[2], nnk_vrc_win_last[2]);
    fprintf(stderr, "vrc: VRAM window split: ABG=%u BBG=%u AOBJ=%u BOBJ=%u LCDC=%u\n",
            nnk_vrc_win5[0], nnk_vrc_win5[1], nnk_vrc_win5[2],
            nnk_vrc_win5[3], nnk_vrc_win5[4]);
    fprintf(stderr, "vrc: runtime model: powerctl=0x%08X dispcnt=0x%08X exmemcnt=0x%08X "
                    "powcnt_writes=%u last=0x%04X\n",
            powerctl, dispcnt, exmemcnt, nnk_vrc_powcnt_n, nnk_vrc_powcnt_v);
    fprintf(stderr, "vrc: DISPCNT A=0x%08X B=0x%08X (A: mode=%u bg=%u obj=%u | B: mode=%u bg=%u obj=%u)\n",
            nnk_vrc_dispcnt[0], nnk_vrc_dispcnt[1],
            (nnk_vrc_dispcnt[0] >> 16) & 3u, nnk_vrc_dispcnt[0] & 0x1Fu,
            (nnk_vrc_dispcnt[0] >> 4) & 1u,
            (nnk_vrc_dispcnt[1] >> 16) & 3u, nnk_vrc_dispcnt[1] & 0x1Fu,
            (nnk_vrc_dispcnt[1] >> 4) & 1u);
    fprintf(stderr, "vrc: A=%02X->%s B=%02X->%s C=%02X->%s D=%02X->%s E=%02X->%s "
                    "F=%02X->%s G=%02X->%s H=%02X->%s I=%02X->%s\n",
            (unsigned)vramcnt[0], d[0], (unsigned)vramcnt[1], d[1],
            (unsigned)vramcnt[2], d[2], (unsigned)vramcnt[3], d[3],
            (unsigned)vramcnt[4], d[4], (unsigned)vramcnt[5], d[5],
            (unsigned)vramcnt[6], d[6], (unsigned)vramcnt[7], d[7],
            (unsigned)vramcnt[8], d[8]);
    fprintf(stderr, "p4t: last B7 block addr=0x%08X crc=0x%08llX n=%u starts=%u last_op=0x%02X\n",
            nnk_card_last_addr, (unsigned long long)nnk_blkcrc, nnk_blk_n,
            nnk_card_starts, nnk_card_last_op);
}

/* ==========================================================================
 * Phase-4T — card-stall probe (ROMCNT-busy hang at pc 0x02016974).
 *
 * The native guest spins in the ROMCNT poll loop with busy=1 and DRQ=0
 * forever (150M steps, distinct_pcs frozen at 13,828), so the card transfer
 * behind the overlay load never completes.  This probe fires ONCE the guest
 * has polled a busy, DRQ-less ROMCNT 5,000 times and dumps the whole card
 * state machine — the exact anatomy of the dead transfer — so the fix is
 * measured, not guessed.  Called from io_read32's ROMCNT case (hooked by
 * tools/split_dispatch.py); counters updated at every io_card_start.
 * ========================================================================== */
static int nnk_card_probe_done;
static unsigned nnk_card_probe_polls;
static unsigned nnk_card_hb;
static void nnk_card_probe(void)
{
    if (!(romcnt & 0x80000000u)) { nnk_card_probe_polls = 0; return; }
    if (card_drq) { nnk_card_probe_polls = 0; return; }
    nnk_card_probe_polls++;
    /* Dead transfer: busy, no data coming, nothing scheduled, room left. */
    if (card_next == CARD_NEVER && !card_end_pending && card_pos < card_len) {
        nnk_card_probe_done = 1;
        fprintf(stderr,
            "p4t: DEAD transfer starts=%u last_op=0x%02X last_addr=0x%08X "
            "pos=%u/%u count=%d late=%d endpend=%d cyc=%llu "
            "fifo0=0x%08X fifo1=0x%08X romcnt=0x%08X spicnt=0x%08X "
            "dma0cnt=0x%08X dma2cnt=0x%08X dma3cnt=0x%08X pc=0x%08X steps=%llu\n",
            nnk_card_starts, nnk_card_last_op, nnk_card_last_addr,
            card_pos, card_len, card_count, card_late, card_end_pending,
            (unsigned long long)card_cycles,
            card_fifo[0], card_fifo[1], romcnt, spicnt,
            dma_cnt[0], dma_cnt[2], dma_cnt[3], last_pc,
            (unsigned long long)steps);
        return;
    }
    /* Heartbeat: >1M consecutive busy/no-DRQ polls (healthy streaming polls
     * ~900 between words), so the stall's progress over time is measured. */
    if (nnk_card_probe_polls >= 1000000u && nnk_card_hb < 16u) {
        nnk_card_hb++;
        nnk_card_probe_polls = 0;
        fprintf(stderr,
            "p4t: busy-wait heartbeat %u pos=%u/%u count=%d late=%d "
            "next=%llu cyc=%llu last_op=0x%02X last_addr=0x%08X "
            "pc=0x%08X steps=%llu\n",
            nnk_card_hb, card_pos, card_len, card_count, card_late,
            (unsigned long long)card_next, (unsigned long long)card_cycles,
            nnk_card_last_op, nnk_card_last_addr, last_pc,
            (unsigned long long)steps);
    }
}

#endif /* NNK_VRAMCNT_INC */
