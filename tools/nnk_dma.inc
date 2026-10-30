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

/* Phase-4P: a DMA that reads or writes IO space must go through the IO bus,
 * not the flat memory accessor.  melonDS's DMA::Run9 calls ARM9Read32 /
 * ARM9Write32, so a copy sourced from the cart data FIFO (0x04100010) POPS
 * the card FIFO and advances the transfer.  With ld32() the FIFO never moved:
 * measured at step 18534214, the guest's 1-word card DMA (DMA2CNT
 * 0x85000001, SAD 0x04100010) left card_pos=0/512, count=0, drq=0, so the
 * 0x200-byte transfer could never finish and the guest's next command stalled
 * in the ROMCNT-busy wait at 0x020168A0. */
static int nnk_is_io(uint32_t a) { return a >= 0x04000000u && a < 0x05000000u; }
static void nnk_ipc_tick(void);   /* Phase-4Q: defined below (ARM7 IPC reply) */
static uint32_t nnk_bus_rd32(uint32_t a) { return nnk_is_io(a) ? io_read32(a) : ld32(a); }
static uint32_t nnk_bus_rd16(uint32_t a) { return nnk_is_io(a) ? io_read16(a) : ld16(a); }
static void nnk_bus_wr32(uint32_t a, uint32_t v) { if (nnk_is_io(a)) io_write32(a, v); else st32(a, v); }
static void nnk_bus_wr16(uint32_t a, uint32_t v) { if (nnk_is_io(a)) io_write16(a, v); else st16(a, v); }

/* Phase-4P: ARM9 bus-stall (DMA ownership) model.
 *
 * melonDS's DMA::Run9 stops the CPU (CPUStop_DMA9_n) while a channel drains, so
 * the guest cannot execute inside a cart transfer: it never observes a
 * half-arrived 0x200 read.  Native counted guest instructions straight through
 * the transfer, which is how the guest reached its 1-word FIFO DMA at step
 * 18,534,214 only 69 instructions after the start (cmddelay is 8,180 cycles)
 * and read an EMPTY FIFO — stranding the transfer at card_pos=8/512.
 *
 * Model: while a mode-5 (cart DRQ) channel is armed and a transfer is in
 * flight, the transfer runs to completion in emulated card time before the
 * guest gets its next instruction; the elapsed cycles are added to the ARM9
 * clock (melonDS advances ARM9Timestamp across the halt exactly like this).
 * Measured card time per 0x200 transfer: native 10,761 cycles vs melonDS
 * 21,457 raw = 10,728 ARM9 cycles (ARM9ClockShift=1), 0.3%. */
static void nnk_dma_card(int i);   /* defined below: mode-5 FIFO drain */
static int nnk_dma_mode5_ch(void) {
    for (int i = 0; i < 4; i++)
        if ((dma_cnt[i] & 0x80000000u) && (((dma_cnt[i] >> 27) & 7u) == 5u)) return i;
    return -1;
}
static void nnk_card_stall(void) {
    int ch;
    uint64_t t0;
    unsigned guard = 0;
    if (!(romcnt & 0x80000000u)) return;
    ch = nnk_dma_mode5_ch();
    if (ch < 0) return;
    t0 = card_cycles;
    while ((romcnt & 0x80000000u) && guard++ < 8192u) {
        if (card_next != CARD_NEVER && card_next > card_cycles)
            card_cycles = card_next;              /* engine time passes */
        if (card_next != CARD_NEVER && card_next <= card_cycles)
            card_tick(0u);                        /* receive a word, or end */
        if (card_drq && (romcnt & 0x80000000u))
            nnk_dma_card(ch);                     /* DMA pops what is due */
        else if (!card_drq && card_next == CARD_NEVER)
            break;
    }
    cycles += (card_cycles - t0);                 /* the ARM9 clock ran on */
}

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
        if (word) nnk_bus_wr32(d, nnk_bus_rd32(s));
        else nnk_bus_wr16(d, nnk_bus_rd16(s));
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

/* Phase-4O: ARM9 DMA start mode 5 = "game card DRQ".
 *
 * Phase-4N modelled this as mode 3 ("special") and therefore never fired: the
 * ARM9 start-timing field is (Cnt >> 27) & 7 (melonDS DMA::WriteCnt) and the
 * cart trigger is mode 5 — NDSCart::Interface::CheckDMA() calls
 * NDS::CheckDMAs(0, 0x05) when ROMCNT bit23 (DRQ) rises.
 *
 * MEASURED PRODUCER (melonDS ground truth, MELONDS_DMALOG_PATH +
 * MELONDS_CARDLOG_PATH, p4o_dma.log / p4o_card.log): the ITCM card-read helper
 * writes DMA2CNT = 0xAF000001 at PC 0x01FF84D0, ts 56,526,612 —
 *   bit31 enable, bits27-29 = 5 (cart DRQ), bit25 repeat, 16-bit units,
 *   SAD = 0x04100010 (card data FIFO), DAD = 0x02079F80 (destination buffer)
 * — 182 cycles before the guest reaches the WFI idle loop 0x0200712C, and the
 * same helper then starts a 0x200-byte B7 read (ROMCNT 0xA1416657,
 * AUXSPICNT 0xC000 = IRQ enable).  While the CPU is stopped the DMA moves the
 * whole transfer out of the card FIFO (melonDS ReadROMData log: halted=1,
 * pc=0x0200911C, reads=128, last read -> ROMEndTransfer -> IF bit19 at
 * ts 56,548,252 = the loop's first wake).  So the producer is this DMA, not a
 * guest poll loop: without it the FIFO stalls at 2 words, the transfer never
 * ends and the loop can never be woken.
 *
 * The drain below walks the card state machine forward to each scheduled
 * event, so the transfer really completes and AUXSPICNT bit14 really raises
 * IF bit19; the destination buffer receives the card byte stream in order
 * (16-bit units = two halfwords per FIFO word, 32-bit units = one word). */
static unsigned nnk_dma_card_n;
static void nnk_dma_card(int i) {
    uint32_t cnt = dma_cnt[i];
    uint32_t count = cnt & 0x001FFFFFu;
    int word = (cnt & (1u << 26)) != 0;
    uint32_t step = word ? 4u : 2u;
    uint32_t per = word ? 1u : 2u;        /* count units per FIFO word */
    uint32_t dadir = (cnt >> 21) & 3u;
    uint32_t d = dma_dad[i], done = 0, popped = 0;
    if (!count) count = 0x00200000u;
    for (;;) {
        /* Only fire card events that are ALREADY due on the card-bus clock:
         * the transfer must take its modelled 21,7xx cycles of wall time, or
         * the DMA would deliver the whole 0x200-byte read in one step and the
         * guest would never enter the WFI idle loop the vendor SDK uses for
         * blocking card reads.  melonDS throttles the same way: its DMA runs
         * only up to ARM9Target (DMA::Run9), so the CPU halts in between. */
        while (!card_drq && card_next != CARD_NEVER && card_next <= card_cycles)
            card_tick(0u);
        if (!card_drq) break;             /* nothing due: leave the channel armed */
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
            if (word) st32(d, v);
            else { st16(d, (uint16_t)(v & 0xFFFFu)); st16(d + 2u, (uint16_t)(v >> 16)); }
        }
        popped++;
        done += per;
        if (dadir == 1u) d -= step; else if (dadir != 2u) d += step;
        if (done >= count) {
            /* Repeat bit (25) keeps the channel armed and reloads the count;
             * without it the channel completes and the enable bit clears
             * (melonDS DMA::Run9). */
            if (!(cnt & (1u << 25))) { dma_cnt[i] = cnt & ~0x80000000u; break; }
            done = 0;
        }
    }
    dma_dad[i] = d;
    if (nnk_dma_card_n < 8u && popped) {
        fprintf(stderr, "dma: card DMA%d drained %u FIFO words (%s) from "
                "0x%08X to 0x%08X cnt=0x%08X pc=0x%08X\n", i, popped,
                word ? "word" : "half", dma_sad[i], dma_dad[i], dma_cnt[i], last_pc);
        nnk_dma_card_n++;
    }
}

void nnk_dma_tick(void) {
    /* Phase-4Q: deliver a pending measured ARM7 IPC reply. */
    nnk_ipc_tick();
    /* Phase-4Q probe: the IE write history.  At the barrier native held
     * IE=0x00000009 where melonDS holds 0x00040019 (IPCRecv bit18 / Timer1
     * bit4), so this logs every IE change with the writing pc. */
    { static uint32_t last_ie; static int init, n;
      if (init && ie != last_ie && n < 60) {
          fprintf(stderr, "iewr step=%llu pc=0x%08X ie 0x%08X -> 0x%08X "
                  "if=0x%08X ime=%u\n", (unsigned long long)steps, last_pc,
                  last_ie, ie, iflags, ime);
          n++;
      }
      last_ie = ie; init = 1; }
    /* Phase-4P: cart-DMA bus ownership — the CPU does not execute while a
     * mode-5 channel drains a transfer (melonDS CPUStop_DMA9_n). */
    nnk_card_stall();
    /* Phase-4P probe: who writes DMA2CNT (the card-DRQ channel) and what the
     * card model looks like at that instant.  Polled, so no call site is
     * needed in the generated io_write32. */
    { static uint32_t seen[4]; static int init, wr_n;
      for (int i = 0; i < 4; i++) {
          uint32_t v = dma_cnt[i];
          if (init && v != seen[i] && wr_n < 80) {
              fprintf(stderr, "dmawr step=%llu pc=0x%08X ch=%d old=0x%08X new=0x%08X "
                      "sad=0x%08X dad=0x%08X cardpos=%u/%u count=%d late=%d drq=%d "
                      "next=%lld romcnt=%08X\n",
                      (unsigned long long)steps, last_pc, i, seen[i], v,
                      dma_sad[i], dma_dad[i], card_pos, card_len, card_count,
                      card_late, card_drq,
                      (long long)(card_next == CARD_NEVER ? -1 : (long long)card_next),
                      romcnt);
              wr_n++;
          }
          seen[i] = v;
      }
      init = 1;
    }
    /* Phase-4P probe: the guest's ROMCNT-busy wait (0x020168A0 ldr / 0x020168A4
     * tst #0x80000000 / 0x020168A8 bne) spinning without the transfer's end
     * event ever firing.  One-shot state dump after 100k consecutive loop
     * dispatches. */
    { static unsigned long stall_n; static int stall_done;
      unsigned long long ticks_at_dump = (unsigned long long)card_cycles;
      if (last_pc == 0x020168A0u || last_pc == 0x020168A4u || last_pc == 0x020168A8u) {
          stall_n++;
      } else stall_n = 0;
      if (!stall_done && stall_n == 100000u) {
          stall_done = 1;
          fprintf(stderr, "busyprobe step=%llu card_cycles=%llu(%llu) card_next=%lld "
                  "endpend=%d romcnt=%08X spicnt=%08X card_pos=%u/%u count=%d late=%d drq=%d "
                  "dma2=%08X dad2=%08X if=%08X ie=%08X\n",
                  (unsigned long long)steps, (unsigned long long)card_cycles,
                  ticks_at_dump,
                  (long long)(card_next == CARD_NEVER ? -1 : (long long)card_next),
                  card_end_pending, romcnt, spicnt, card_pos, card_len, card_count,
                  card_late, card_drq, dma_cnt[2], dma_dad[2], iflags, ie);
      }
    }
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
        /* Phase-4O: mode 5 = game card DRQ (ARM9 CNT bits 27-29; melonDS
         * CheckDMA -> CheckDMAs(0, 0x05)).  Phase-4N's mode-3 guess never
         * matched any measured arm and is replaced. */
        if (mode == 5u) { nnk_dma_card(i); continue; }
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

/* ---- Phase-4Q: ARM7 IPC-FIFO responder (measured request -> reply) -------
 *
 * ROOT CAUSE OF THE u16[0x0206E93A] BARRIER (native step 18,554,864): the
 * guest's card/stream driver starts an operation through the SDK API at
 * 0x0200FA38, which sets handler-mask bit 1 (pc 0x0200FB20) and then SENDS
 * the ARM7 two IPC-FIFO requests at pc 0x0200BAD4.  melonDS's ARM7 answers
 * the first one; this runtime's scripted responder knew only the Phase-4C
 * request (0x037A404D) and dropped both:
 *
 *   io: unscripted IPCFIFO request 0x80004046 at pc=0x0200BAD4
 *   io: unscripted IPCFIFO request 0x40400006 at pc=0x0200BAD4
 *
 * so no reply ever reached the ARM9, the IPC-recv IRQ never fired, the
 * driver's "done" event never ran its finish tail (0x0200F784..0x0200F7A0)
 * and the mask bit never cleared; the guest spun at 0x0200FEFC to the step
 * budget while melonDS proceeds.
 *
 * melonDS ground truth (p4q_mel_io.txt, whole-run send vocabulary of
 * W32 0x04000188): 0x037A404D (scripted, Phase-4C), 0x80004046, 0x40400006,
 * 0x000001AB x3, 0x000000AB, 0x0000002B, 0x81BA702B, 0x81B39C07, 0x81B36C07,
 * 0x81B34E07, 0x81B34807.  Exactly ONE reply word is read back by the ARM9:
 * 0xC0204006 (R32 0x04100000 pc=0x0200BB44 ts=28,709,423), 2,076 cycles
 * after the request 0x80004046 (W32 0x04000188 pc=0x0200BAD4 ts=28,707,347)
 * and delivered through the IPC-recv IRQ (IF bit18: the BIOS stub acks
 * 0x00040000 at ts=28,709,384).  The ts unit equals the ARM9 cycle scale
 * (card start->start gap in the same trace 10,900..11,400 ts vs the measured
 * 10,850 ARM9 cycles), so the modelled latency is 2,076 runtime cycles.
 *
 * 0xC0204006 decoded against the guest's handler at 0x0200F5CC: handler id
 * (word & 0x1F) = 6 -> the driver's IPC entry, bit5 = 0 (dispatch path),
 * type ((word >> 14) & 0x7F) = 1, sub-index ((word >> 6) & 0xFF) = 0 ->
 * 0x0200F710 case r0==1 -> the finish tail that writes +0x36=2 and clears
 * mask bit 1 at 0x0200F7A0.  The register-path twin 0x80004046 (bit5 = 1,
 * same type 1) is the ARM9's "notify me when done" request. */
static uint32_t nnk_ipc_reply_word;
static uint64_t nnk_ipc_reply_at;       /* runtime cycles when the reply lands */
static int nnk_ipc_reply_pend;
static unsigned nnk_ipc_handled_n;      /* log cap */
void nnk_ipc_send(uint32_t v);
static void nnk_ipc_tick(void);

void nnk_ipc_send(uint32_t v) {
    if (v == 0x80004046u) {
        nnk_ipc_reply_word = 0xC0204006u;
        nnk_ipc_reply_at = cycles + 2076u;   /* measured request->reply gap */
        nnk_ipc_reply_pend = 1;
        if (nnk_ipc_handled_n < 8u) {
            fprintf(stderr, "ipc: ARM7 request 0x%08X -> reply 0x%08X in 2076 "
                    "cycles step=%llu pc=0x%08X\n", v, nnk_ipc_reply_word,
                    (unsigned long long)steps, last_pc);
            nnk_ipc_handled_n++;
        }
        return;
    }
    if (nnk_ipc_handled_n < 8u) {
        fprintf(stderr, "ipc: ARM7 request 0x%08X (no measured reply in the "
                "melonDS capture) step=%llu pc=0x%08X\n", v,
                (unsigned long long)steps, last_pc);
        nnk_ipc_handled_n++;
    }
}

static void nnk_ipc_tick(void) {
    if (nnk_ipc_reply_pend && cycles >= nnk_ipc_reply_at) {
        nnk_ipc_reply_pend = 0;
        if (arm9_recv_n < 16u) arm9_recv_fifo[arm9_recv_n++] = nnk_ipc_reply_word;
        iflags |= 0x00040000u;               /* IRQ_IPCRecv: the ARM7 answered */
        fprintf(stderr, "ipc: reply 0x%08X queued if=0x%08X ie=0x%08X ime=%u "
                "cpsr=%02X step=%llu\n", nnk_ipc_reply_word, iflags, ie, ime,
                (unsigned)cpsr_ctl, (unsigned long long)steps);
    }
}
