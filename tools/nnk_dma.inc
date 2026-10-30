/* tools/nnk_dma.inc — Phase-4L: DMA completion/timing shim for the split build.
 *
 * Spliced into the generated main TU by tools/split_dispatch.py (included next
 * to nnk_boot.inc, called once per dispatch from the dispatch loop).
 *
 * WHY (measured, Phase-4L): the guest arms DMA3 at 0x01FF84D0 (ITCM code) with
 * DMA3CNT = 0x85000018 and then waits for bit31 to clear:
 *
 *   0x0200929C  ldr r0,[r4]        ; r4 = 0x040000DC (DMA3CNT)
 *   0x020092A0  tst r0,#0x80000000
 *   0x020092A4  bne 0x0200929C
 *
 * melonDS (p4l_mel_io.txt): the write at ts=20387028 is followed by reads that
 * already return 0x05000018 (bit31 clear) at ts=20387141 — 113 cycles later.
 * Native returned 0x85000018 for the rest of the run and spun in that 3-PC
 * loop from <19M to >60M instructions (p4l_pchist.py: 2,500,001 traced PCs,
 * 3 distinct PCs).
 *
 * Root cause: Cnt bits 27..29 are the ARM9 start-timing field (melonDS
 * DMA::WriteCnt: StartMode = (Cnt >> 27) & 7).  The runtime's io_do_dma()
 * reads `(cnt >> 24) & 3`, which for 0x85000018 yields 1 ("not immediate", so
 * it returns without running the transfer) even though the field is 0
 * (immediate).  Bits 24/26 in that value are the source-address control
 * (fixed) and the 32-bit size flag.
 *
 * recompile.rs carries the authoritative fix (same mask, in io_do_dma) for the
 * next image regeneration; this shim makes the CURRENT generated image
 * (work/phase3a/arm9_thumb_ovl_itcm_p4j.c, generated before the fix) behave
 * correctly without regenerating 25 MB of C:
 *   - an enabled channel in immediate mode (0) that io_do_dma missed is run
 *     here, with the transfer semantics of melonDS DMA::Start/WriteCnt
 *     (word/half, source/destination increment/decrement/fixed, repeat bit);
 *   - vblank (1) and hblank (2) channels are triggered on the corresponding
 *     vcount edges, which the runtime never did at all.
 * Nothing is faked: the copy really happens and the enable bit is really
 * cleared, exactly once per transfer.
 */
static int nnk_dma_last_vc = -1;
static unsigned nnk_dma_shim_n;          /* transfers completed by this shim */
static unsigned nnk_dma_timed_n;         /* vblank/hblank triggers */

static void nnk_dma_copy(int i) {
    uint32_t cnt = dma_cnt[i];
    uint32_t count = cnt & 0x001FFFFFu;
    int word = (cnt & (1u << 26)) != 0;
    uint32_t step = word ? 4u : 2u;
    uint32_t sadir = (cnt >> 23) & 3u;    /* 0:+1  1:-1  2:fixed  3:+1 */
    uint32_t dadir = (cnt >> 21) & 3u;
    uint32_t s = dma_sad[i], d = dma_dad[i];
    if (!count) count = 0x00200000u;
    for (uint32_t k = 0; k < count; k++) {
        if (word) st32(d, ld32(s)); else st16(d, (uint16_t)ld16(s));
        if (sadir == 1u) s -= step; else if (sadir != 2u) s += step;
        if (dadir == 1u) d -= step; else if (dadir != 2u) d += step;
    }
    if (nnk_dma_shim_n < 4u) {
        fprintf(stderr, "dma: shim completed DMA%d cnt=0x%08X count=%u %s "
                "0x%08X->0x%08X pc=0x%08X\n", i, cnt, count,
                word ? "word" : "half", dma_sad[i], dma_dad[i], last_pc);
        nnk_dma_shim_n++;
    }
    /* bit25 = repeat: keep the enable bit for timed modes, clear otherwise */
    dma_cnt[i] = (((cnt >> 27) & 7u) != 0u && (cnt & (1u << 25)))
                     ? cnt : (cnt & ~0x80000000u);
}

/* Phase-4N: DMA start mode 3 = "special" = game card (melonDS DMA::WriteCnt
 * StartMode 3 for ARM9 = NDSCart DRQ).  The storage-image read loop arms such
 * a channel and then sleeps in the WFI idle loop at 0x0200712C waiting for the
 * cart transfer-done IRQ.  Without a card-DRQ drain the transfer stalls at the
 * 2-word FIFO, never reaches ROMEndTransfer, and the IRQ never fires
 * (measured: p4n_wake.txt "irqwake ... enabled=0x00080000" once per transfer).
 * The drain walks the card state machine forward to each scheduled event, so
 * the transfer really completes and AUXSPICNT bit14 really raises IF bit19. */
static unsigned nnk_dma_card_n;
static void nnk_dma_card(int i) {
    uint32_t cnt = dma_cnt[i];
    uint32_t count = cnt & 0x001FFFFFu;
    int word = (cnt & (1u << 26)) != 0;
    uint32_t step = word ? 4u : 2u;
    uint32_t dadir = (cnt >> 21) & 3u;
    uint32_t d = dma_dad[i], done = 0;
    if (!count) count = 0x00200000u;
    while (done < count) {
        uint32_t guard = 0x10000u;
        while (!card_drq && card_next != CARD_NEVER && guard--) {
            card_cycles = card_next;      /* advance the scheduled card event */
            card_tick(0u);
        }
        if (!card_drq) break;             /* card idle: leave the channel armed */
        {
            uint32_t v = card_fifo[card_poscpu];
            card_poscpu ^= 1;
            if (card_count > 0) card_count--;
            card_drq = 0;
            if (card_pos < card_len) {
                if (card_late) { card_late = 0; card_advance_receive(); }
            } else {
                if (card_count == 0) card_end_transfer();
                else card_drq = 1;
            }
            if (word) st32(d, v); else st16(d, (uint16_t)v);
        }
        if (dadir == 1u) d -= step; else if (dadir != 2u) d += step;
        done++;
    }
    if (done == count) {
        dma_cnt[i] = (cnt & (1u << 25)) ? cnt : (cnt & ~0x80000000u);
        if (nnk_dma_card_n < 8u) {
            fprintf(stderr, "dma: card DMA%d drained %u %s words from 0x%08X "
                    "to 0x%08X pc=0x%08X\n", i, done, word ? "word" : "half",
                    dma_sad[i], dma_dad[i], last_pc);
            nnk_dma_card_n++;
        }
    }
}

void nnk_dma_tick(void) {
    /* Phase-4N probe: one-shot state dump at the ARM9 WFI idle loop. */
    { static int lp_done;
      if (!lp_done && last_pc == 0x02007134u) {
          lp_done = 1;
          fprintf(stderr, "loopprobe step=%llu card_cycles=%llu card_next=%lld "
                  "endpend=%d romcnt=%08X spicnt=%08X card_pos=%u/%u count=%d late=%d drq=%d "
                  "ie=%08X if=%08X ime=%u cpsr=%02X dma=%08X,%08X,%08X,%08X\n",
                  (unsigned long long)steps, (unsigned long long)card_cycles,
                  (long long)(card_next == CARD_NEVER ? -1 : (long long)card_next),
                  card_end_pending, romcnt, spicnt, card_pos, card_len, card_count,
                  card_late, card_drq, ie, iflags, ime, (unsigned)cpsr_ctl,
                  dma_cnt[0], dma_cnt[1], dma_cnt[2], dma_cnt[3]);
      }
    }
    uint32_t vc = vcount();
    int vbedge = 0, hbedge = 0;
    if (nnk_dma_last_vc >= 0 && vc != (uint32_t)nnk_dma_last_vc) {
        hbedge = 1;
        if (nnk_dma_last_vc < 192 && vc >= 192) vbedge = 1;
    }
    nnk_dma_last_vc = (int)vc;
    for (int i = 0; i < 4; i++) {
        uint32_t cnt = dma_cnt[i];
        if (!(cnt & 0x80000000u)) continue;
        uint32_t mode = (cnt >> 27) & 7u;
        if (mode == 0u) { nnk_dma_copy(i); continue; }
        if (mode == 3u) { nnk_dma_card(i); continue; }
        if ((mode == 1u && vbedge) || (mode == 2u && hbedge)) {
            if (nnk_dma_timed_n < 4u) {
                fprintf(stderr, "dma: timed DMA%d mode=%u fired pc=0x%08X "
                        "cnt=0x%08X vc=%u\n", i, mode, last_pc, cnt, vc);
                nnk_dma_timed_n++;
            }
            nnk_dma_copy(i);
        }
    }
}
