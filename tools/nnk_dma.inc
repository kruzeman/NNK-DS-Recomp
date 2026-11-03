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

/* Phase-5BF: log every SHIM-executed DMA that lands in the AOBJ (sprite tile)
 * window 0x06400000..0x0643FFFF.  NOTE (measured): the prompt's tile upload is
 * NOT in this path — it is an *immediate* word DMA, which the prelude's
 * io_do_dma() runs (that is where the half-length copy happens, see
 * PHASE5BF.md §2).  This log covers the timed-mode transfers the Phase-4L shim
 * owns, so the two halves of the DMA model can be told apart.
 * Env NNK_AOBJ_TRACE=1. */
static int nnk_dma_aobj_on = -1;
static unsigned nnk_dma_aobj_n;
static void nnk_dma_aobj_log(int i, uint32_t cnt, uint32_t count, int word,
                             uint32_t s, uint32_t d)
{
    uint32_t v0 = 0, v1 = 0;
    if (nnk_dma_aobj_on < 0) {
        const char *e = getenv("NNK_AOBJ_TRACE");
        nnk_dma_aobj_on = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
    }
    if (!nnk_dma_aobj_on) return;
    if (d < 0x06400000u || d >= 0x06440000u) return;
    if (!nnk_is_io(s)) { v0 = ld32(s & ~3u); v1 = ld32((s & ~3u) + 4u); }
    nnk_dma_aobj_n++;
    fprintf(stderr, "aobjdma #%u ch=%d step=%llu pc=0x%08X cnt=0x%08X count=%u %s "
            "sadir=%u dadir=%u src=0x%08X dst=0x%08X end=0x%08X src0=%08X,%08X\n",
            nnk_dma_aobj_n, i, (unsigned long long)steps, last_pc, cnt, count,
            word ? "word" : "half", (cnt >> 23) & 3u, (cnt >> 21) & 3u,
            s, d, d + (uint32_t)count * (word ? 4u : 2u), v0, v1);
}

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
    nnk_dma_aobj_log(i, cnt, count, word, s, d);
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

/* Phase-4U: timer-overflow IRQ raise (melonDS NDSTimer::Run -> NDS.SetIRQ).
 *
 * MEASURED (p4q_mel_io.txt vs p4t_run60b.err): melonDS delivers 5,779
 * IRQ_Timer1 acks (W32 0x04000214 0x00000010 at the ITCM stub 0x01FF8038,
 * first at ts 29,302,442, steady period 268,939 ts) plus 376 Timer0/2 acks,
 * while native delivers ZERO timer IF bits after step 9,097,800 — the
 * runtime's only iflags sources were the measured irq_raise_log (ends at
 * step 9,097,800), ROMEndTransfer (bit19) and the scripted IPC replies
 * (bit18).  Native IF values in the whole run: 0x00000000/0x00040000/
 * 0x00080000/0x00080008/0x000C0000 — never bit4.
 *
 * BARRIER (Phase-4U): after the last common card read (B7 #1556,
 * 0x0DA6C400 at native step 23,486,383 / melon ts 35,250,160) the SDK
 * ready-thread queue (linked list from *(0x0206C684+0x24), state at
 * node+0x64, walked at pc 0x02006E80) drains C770 -> EA48 -> F540 and then
 * finds ONLY the idle node 0x0206C6B0 ready (native step 23,494,370): the
 * guest arms TM1 (pc 0x0200850C/8514) and parks in WFI at 0x02009118
 * forever, while melonDS's armed Timer1 fires 128 ts later (IF=0x00280010,
 * acked at ts 35,263,214) and the driver goes on to issue B7 #1557
 * (0x0DA6C600) at ts 35,272,550.
 *
 * Model: overflow of an enabled timer with its IRQ bit set raises
 * IF bit (3+channel), exactly melonDS NDSTimer::Run.  Latched on the enable
 * edge in cycles; period = 0x10000 - reload (melonDS reloads RELOAD on
 * overflow).  Gated to steps > 9,097,800 so the measured boot raise script
 * (irq_raise_log) stays byte-exact for the strict-prefix verification. */
static uint64_t p4u_tm_base[4];
static uint64_t p4u_tm_ovf[4];
static uint8_t p4u_tm_on[4];
static void p4u_timer_tick(void) {
    int t;
    if (steps <= 9097800u) return;
    for (t = 0; t < 4; t++) {
        uint32_t ctrl = tm_ctrl[t];
        if (!(ctrl & 0x80u)) { p4u_tm_on[t] = 0; continue; }
        if (!p4u_tm_on[t]) {
            p4u_tm_on[t] = 1;
            p4u_tm_base[t] = cycles;
            p4u_tm_ovf[t] = 0;
            continue;
        }
        if (!(ctrl & 0x40u)) continue;
        {
            unsigned shift = (ctrl & 3u) == 0 ? 0u
                          : ((ctrl & 3u) == 1 ? 6u
                          : ((ctrl & 3u) == 2 ? 8u : 10u));
            uint32_t period = 0x10000u - tm_reload[t];
            uint64_t ticks, ovf;
            if (!period) period = 0x10000u;
            ticks = (cycles - p4u_tm_base[t]) >> shift;
            /* Phase-5AE: divide only when an overflow is actually possible.
             * `ovf > p4u_tm_ovf[t]` is exactly `ticks >= (ovf+1)*period`, so
             * the hot path is a 64x32 multiply + compare instead of a 64-bit
             * divide.  This function runs once per dispatched guest instruction
             * and used to divide up to four times per step (TSC profiler: the
             * timer model was the second largest fixed per-step cost after the
             * per-step getenv() pair). */
            if (ticks < (uint64_t)(p4u_tm_ovf[t] + 1u) * (uint64_t)period) continue;
            ovf = ticks / period;
            if (ovf > p4u_tm_ovf[t]) {
                p4u_tm_ovf[t] = ovf;
                iflags |= 1u << (3 + t);
            }
        }
    }
}
/* Phase-4X: VBlank IRQ source (melonDS GPU::StartVBlank -> NDS.SetIRQ).
 *
 * BARRIER (Phase-4V stop-report, measured): from ~step 39M the guest runs a
 * VCOUNT-poll main loop and does ZERO frame work — distinct PCs freeze at
 * 15,542 — because nothing ever raises IF bit0 (VBlank).  The guest had ALREADY
 * enabled the source: native IE=0x00000019 (VBlank bit0 + Timer0/1) with
 * ime=1 at step 195,974,659 (p4r probe line), so delivery was armed and
 * waiting on an IF bit that never came.
 *
 * melonDS ground truth (p4q_mel_io.txt, 6M-event IO capture):
 *   W16 0x04000004 0x00000000 pc=0x02002AB0 ts=19,908,916   (DISPSTAT clear)
 *   W16 0x04000004 0x00000008 pc=0x02002BB8 ts=19,909,621   (bit3 = VBlank IRQ EN)
 *   first ack  W32 0x04000214 0x00000001 pc=0x01FF8038 ts=20,015,653
 *   consecutive ack deltas 560,177..560,193 ts, median 560,190 = one frame
 *   (2130 cycles/scanline x 263 scanlines) — the measured VBlank cadence.
 * The runtime's `cycles` clock is in the same units (vcount() = cycles/2130
 * % 263; gxd presents one frame per <192 -> >=192 edge), so the raise rides
 * the same edge.  Semantics: at the edge into vc==192, IF bit0 raises iff
 * DISPSTAT bit3 is set — exactly melonDS StartVBlank.  Gated to steps >
 * 9,097,800 so the measured boot raise script (irq_raise_log) stays
 * byte-exact for the strict-prefix verification (same gate as Phase-4U;
 * the measured DISPSTAT enable itself happens far later, ~step 13.2M). */
static uint32_t p4x_last_vc = 0xFFFFFFFFu;
static unsigned p4x_vbl_n;
static void p4x_vblank_tick(void) {
    uint32_t vc;
    if (steps <= 9097800u) return;
    vc = vcount();
    if (p4x_last_vc < 192u && vc >= 192u) {
        if (dispstat & 0x8u) {              /* DISPSTAT bit3: VBlank IRQ EN */
            iflags |= 1u;                   /* IRQ_VBlank, melonDS GPU */
            p4x_vbl_n++;
            if (p4x_vbl_n <= 4u || (p4x_vbl_n & 255u) == 0u) {
                fprintf(stderr, "p4x: VBlank IRQ raise n=%u step=%llu cyc=%llu "
                        "if=0x%08X ie=0x%08X ime=%u\n",
                        p4x_vbl_n, (unsigned long long)steps,
                        (unsigned long long)cycles, iflags, ie, ime);
            }
        }
    }
    p4x_last_vc = vc;
}

/* Phase-5BG diagnostic — NNK_REGWATCH=<reg>[,<reg>...]
 *
 * The dispatch loop calls this once per instruction, with `r[]` holding the
 * state AFTER the previous instruction and BEFORE the one at `pc`, and
 * `last_pc` naming that previous instruction.  So a register that differs
 * from the value latched at the previous call was written by `last_pc`, and
 * the line names it: "prev=<pc of the writer>".  A callee-saved register
 * that changes value across a call tree therefore names the exact
 * instruction that clobbered it, without a full per-step register dump.
 *
 *   NNK_REGWATCH=4            watch r4
 *   NNK_REGWATCH=4,13,14      watch several
 *   NNK_REGWATCH_FROM=<step>  only report from this step on
 */
static int nnk_rw_state = -1;           /* -1 = not read yet, 0 = off, 1 = on */
static int nnk_rw_reg[8];
static int nnk_rw_n;
static uint32_t nnk_rw_last[16];
static unsigned long long nnk_rw_from;

static void nnk_regwatch(uint32_t pc)
{
    int i;
    if (nnk_rw_state < 0) {
        const char *s = getenv("NNK_REGWATCH");
        nnk_rw_state = 0;
        if (s && s[0]) {
            const char *q = s;
            while (*q && nnk_rw_n < 8) {
                int v;
                while (*q == ' ' || *q == ',') q++;
                if (!*q) break;
                v = (int)strtol(q, (char **)&q, 0);
                if (v >= 0 && v < 16) nnk_rw_reg[nnk_rw_n++] = v;
            }
            s = getenv("NNK_REGWATCH_FROM");
            if (s && s[0]) nnk_rw_from = strtoull(s, NULL, 0);
            for (i = 0; i < 16; i++) nnk_rw_last[i] = r[i];
            if (nnk_rw_n) {
                nnk_rw_state = 1;
                fprintf(stderr, "regwatch: watching %d register(s) from step %llu\n",
                        nnk_rw_n, nnk_rw_from);
            }
        }
        if (!nnk_rw_state) return;
    }
    if (steps < nnk_rw_from) return;
    for (i = 0; i < nnk_rw_n; i++) {
        int k = nnk_rw_reg[i];
        if (r[k] != nnk_rw_last[k]) {
            fprintf(stderr, "regwatch step=%llu pc=0x%08X r%d 0x%08X -> 0x%08X "
                    "writer=0x%08X\n",
                    (unsigned long long)steps, pc, k, nnk_rw_last[k], r[k],
                    last_pc);
            nnk_rw_last[k] = r[k];
        }
    }
}

void nnk_dma_tick(void) {
    /* Phase-4X: VBlank IF raise (edge into vc==192) before the delivery check. */
    p4x_vblank_tick();
    /* Phase-4U: timer overflows may raise IF before the delivery check. */
    p4u_timer_tick();
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
/* Phase-4S: the ARM7 request EFFECT (measured card-phase completion) */
static void nnk_p4s_request(uint32_t v);
static void nnk_p4s_tick(void);

/* Phase-4X: the class-0x0B IPC request/reply handshake (measured).
 *
 * BARRIER (measured in both runs): at native step 38,987,833 the guest sends
 * IPC words 0x0000002B then 0x81BA702B (pc 0x0200BAD4) and waits for the
 * ARM7's answer; the native ARM7 shim had no model for it ("no measured
 * reply in the melonDS capture" — the Phase-4Q table predates the long IO
 * capture), so the mailbox receive never completes, every thread parks, and
 * the card stream dies at step 38,981,422 (9,093 starts, last op B8).
 *
 * melonDS ground truth (p4q_mel_io.txt, the WHOLE 6M-event capture): exactly
 * ONE handshake round — 6 sends at pc 0x0200BAD4, 5 replies 0x0000006B read
 * at the mailbox loop pc 0x0200BB44:
 *   send 0x0000002B        ts 50,484,317
 *   send 0x81BA702B        ts 50,484,371   (parameter word: bit31 set)
 *   recv 0x0000006B        ts 50,485,392   (+1,075 from the first send)
 *   send 0x000000AB        ts 50,485,825   (+1,416 reply->send)
 *   recv 0x0000006B        ts 50,487,241   (+1,416)
 *   send 0x000001AB        ts 50,487,701   (+2,472)
 *   recv 0x0000006B        ts 50,490,173   (+2,472)
 *   send 0x000001AB        ts 50,491,108   (+3,331)
 *   recv 0x0000006B        ts 50,494,439   (+3,331)
 *   send 0x000001AB        ts 50,495,455   (+2,568)
 *   recv 0x0000006B        ts 50,498,023   (+2,568)
 * The exchange is strictly alternating (the guest sends the next request
 * only after the previous reply), and (v & 0x1F) == 0x0B with bit31 clear
 * marks the command words (the 0x81BA702B parameter carries bit31 and gets
 * no reply of its own).  Modelled: one 0x6B reply per command at the
 * measured per-exchange gap (first 5 exact, then 2,500 = the measured mean);
 * nothing is faked — the guest's mailbox handler really receives the word.
 * After the round melonDS's card stream resumes (B7s at ts 53M) and the
 * overlay-resident display switch follows at ts 54,276,936 (pc 0x0207CB10). */
static uint64_t nnk_p4b_at;
static int nnk_p4b_pend;
static const uint64_t nnk_p4b_gap[5] = { 1075u, 1416u, 2472u, 3331u, 2568u };
static unsigned nnk_p4b_n;
static void nnk_p4b_request(uint32_t v) {
    uint64_t gap = nnk_p4b_n < 5u ? nnk_p4b_gap[nnk_p4b_n] : 2500u;
    nnk_p4b_at = cycles + gap;
    nnk_p4b_pend = 1;
    nnk_p4b_n++;
    if (nnk_p4b_n <= 8u)
        fprintf(stderr, "p4b: class-0x0B request 0x%08X -> reply 0x0000006B in "
                "%llu cycles step=%llu\n", v, (unsigned long long)gap,
                (unsigned long long)steps);
}

void nnk_ipc_send(uint32_t v) {
    if ((v & 0x1Fu) == 7u) nnk_p4s_request(v);   /* Phase-4S: class-7 card req */
    if ((v & 0x1Fu) == 0x0Bu && !(v & 0x80000000u))
        nnk_p4b_request(v);                      /* Phase-4X: class-0x0B cmd */
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

/* ---- Phase-4R state + forward decls (defined below, used by nnk_ipc_tick) */
/* Phase-5AI: the host queue is no longer a bounded drop buffer.  It models the
 * ARM7's send side: a word that is produced STAYS here until the ARM9 drains
 * the recv FIFO (hardware: the ARM7's send blocks when the FIFO is full, it
 * does not lose the word).  Measured worst case in this project is a guest that
 * stops draining for good (Phase-5AI: the guest halts at 0x02009110 with
 * CPSR.I=1), which parks at most this many words — 4,096 words is ~707 frames
 * at the measured 5.79 words/1e6 cycles, i.e. far past any real backlog — and
 * then the producer STALLS (nnk_p4r_stalled) instead of dropping. */
#define NNK_P4R_QMAX 4096
static struct { uint32_t word; uint64_t at; } nnk_p4r_q[NNK_P4R_QMAX];
static unsigned nnk_p4r_qn;
static unsigned nnk_p4r_emitted, nnk_p4r_drop_log;
static int nnk_p4r_armed;
static int nnk_p4r_stalled;              /* Phase-5AI: ARM7 blocked, nothing lost */
static struct { uint32_t word; uint64_t period; uint64_t next; } nnk_p4r_stream[3];
static void nnk_p4r_arm(uint64_t t0);
static void nnk_p4r_schedule(void);

/* ---- Phase-5AI: the ARM9 drain probe (NNK_IPC_PROBE=1) -------------------
 * The Phase-4R producer is the model of the ARM7's periodic sends; the ARM9
 * side is a 16-word recv FIFO (arm9_recv_fifo) that the guest drains with a
 * read of 0x04100000 (the SDK mailbox loop at pc 0x0200BB44, or the BIOS
 * IPCRecv IRQ stub).  A guest that stops reading therefore fills the FIFO and
 * the host-side 32-entry producer queue drops words.
 *
 * This probe measures WHICH of the two happened: it watches the FIFO depth at
 * every dispatch, remembers the step/cycle of the last observed drain, and
 * complains when a full frame (560,190 cyc, the measured VBlank period) goes
 * by without one — printing the guest's pc and cpsr, i.e. where it is stuck
 * and whether it has IRQs disabled (cpsr I = 0x80).  Diagnostic only: it is
 * off unless NNK_IPC_PROBE=1 and changes no emulated state. */
static int p4r_probe, p4r_probe_init;
static unsigned p4r_prev_depth;
static uint64_t p4r_last_read_step, p4r_last_read_cyc, p4r_probe_next;
static unsigned p4r_nodrain_log, p4r_drain_log;
static void nnk_p4r_probe_tick(void) {
    if (!p4r_probe_init) {
        const char *s = getenv("NNK_IPC_PROBE");
        p4r_probe_init = 1;
        p4r_probe = (s && s[0] == '1') ? 1 : 0;
        p4r_prev_depth = arm9_recv_n;
        p4r_last_read_cyc = cycles;
    }
    if (!p4r_probe) return;
    if (arm9_recv_n < p4r_prev_depth) {      /* the guest popped a word */
        if (p4r_nodrain_log) {               /* report the resume */
            fprintf(stderr, "p4r: drain RESUMED after %llu cyc depth=%u "
                    "pc=0x%08X cpsr=%08X step=%llu\n",
                    (unsigned long long)(cycles - p4r_last_read_cyc),
                    (unsigned)arm9_recv_n, last_pc, (unsigned)cpsr_ctl,
                    (unsigned long long)steps);
            p4r_nodrain_log = 0; p4r_probe_next = 0;
            if (++p4r_drain_log >= 12u) p4r_probe = 2;  /* stop logging */
        }
        p4r_last_read_step = steps;
        p4r_last_read_cyc = cycles;
    }
    if (p4r_probe == 1 && cycles >= p4r_last_read_cyc + 560190u &&
        (!p4r_probe_next || cycles >= p4r_probe_next)) {
        if (p4r_nodrain_log < 40u) {
            fprintf(stderr, "p4r: NO DRAIN for %llu cyc depth=%u qn=%u "
                    "pc=0x%08X cpsr=%08X lr=0x%08X mode=%02X "
                    "last_read_step=%llu step=%llu cyc=%llu\n",
                    (unsigned long long)(cycles - p4r_last_read_cyc),
                    (unsigned)arm9_recv_n, (unsigned)nnk_p4r_qn, last_pc,
                    (unsigned)cpsr_ctl, r[14], (unsigned)(cpsr_ctl & 0x1Fu),
                    (unsigned long long)p4r_last_read_step,
                    (unsigned long long)steps, (unsigned long long)cycles);
            p4r_nodrain_log++;
        }
        p4r_probe_next = cycles + 560190u * 4u;
    }
}

static void nnk_ipc_tick(void) {
    nnk_p4r_probe_tick();              /* Phase-5AI: drain probe (opt-in) */
    nnk_p4s_tick();                    /* Phase-4S: card-phase completion */
    if (nnk_ipc_reply_pend && cycles >= nnk_ipc_reply_at && arm9_recv_n < 16u) {
        /* Phase-5AI: a reply the guest is waiting on is never dropped and never
         * overruns the 16-word recv FIFO — it waits for a free slot (the
         * hardware send would block the same way). */
        nnk_ipc_reply_pend = 0;
        if (arm9_recv_n < 16u) arm9_recv_fifo[arm9_recv_n++] = nnk_ipc_reply_word;
        iflags |= 0x00040000u;               /* IRQ_IPCRecv: the ARM7 answered */
        fprintf(stderr, "ipc: reply 0x%08X queued if=0x%08X ie=0x%08X ime=%u "
                "cpsr=%02X step=%llu\n", nnk_ipc_reply_word, iflags, ie, ime,
                (unsigned)cpsr_ctl, (unsigned long long)steps);
        nnk_p4r_arm(cycles);                 /* Phase-4R: measured rhythm starts */
    }
    if (nnk_p4r_armed) {
        nnk_p4r_schedule();
        /* deliver every due word; the FIFO-full case retries next dispatch */
        while (nnk_p4r_qn && cycles >= nnk_p4r_q[0].at) {
            if (arm9_recv_n >= 16u) break;
            uint32_t w = nnk_p4r_q[0].word;
            arm9_recv_fifo[arm9_recv_n++] = w;
            iflags |= 0x00040000u;           /* IRQ_IPCRecv, as melonDS */
            nnk_p4r_emitted++;
            if (nnk_p4r_emitted <= 8u || (nnk_p4r_emitted & 255u) == 0u) {
                /* Phase-4R probe: the guest's queue state at delivery time.
                 * 0x0206CA5C = the wait's snapshot word (ldr r1,[r6,#4] with
                 * r6=0x0206CA58); the sequence is *( *(0x0206E580) ). */
                uint32_t snap = ld32(0x0206CA5Cu);
                uint32_t p = ld32(0x0206E580u);
                uint32_t seq = p ? ld32(p) : 0u;
                fprintf(stderr, "ipc: p4r ARM7 -> 0x%08X queued if=0x%08X "
                        "ie=0x%08X ime=%u cpsr=%02X recv_n=%u snap=%08X "
                        "seq=%08X ptr=%08X seqobj=%08X step=%llu\n",
                        w, iflags, ie, ime, (unsigned)cpsr_ctl,
                        (unsigned)arm9_recv_n, snap, seq, p,
                        ld32(0x0206CAA0u),
                        (unsigned long long)steps);
            }
            memmove(nnk_p4r_q, nnk_p4r_q + 1,
                    (--nnk_p4r_qn) * sizeof nnk_p4r_q[0]);
        }
    }
    /* Phase-4X: deliver the pending class-0x0B handshake reply. */
    if (nnk_p4b_pend && cycles >= nnk_p4b_at) {
        nnk_p4b_pend = 0;
        if (arm9_recv_n < 16u) {
            arm9_recv_fifo[arm9_recv_n++] = 0x0000006Bu;
            iflags |= 0x00040000u;           /* IRQ_IPCRecv, as melonDS */
            fprintf(stderr, "p4b: ARM7 -> 0x0000006B queued if=0x%08X "
                    "ie=0x%08X ime=%u step=%llu cyc=%llu\n",
                    iflags, ie, ime, (unsigned long long)steps,
                    (unsigned long long)cycles);
        } else {
            nnk_p4b_pend = 1;                /* FIFO full: retry next dispatch */
        }
    }
    p4r_prev_depth = arm9_recv_n;      /* Phase-5AI: depth the guest can pop */
}

/* ---- Phase-4R: the ARM7 periodic IPC producer (measured rhythm) ----------
 *
 * PHASE-4Q's STOP-REPORT BARRIER, NOW MEASURED.  After the reply 0xC0204006
 * the guest calls the SDK wait at 0x0200ACC8..0x0200AD20: it snapshots the
 * OS message-queue sequence (bl 0x0200B3F4 -> word behind *(0x0206E580)),
 * then loops { restore-IRQ (0x02008D34), swiDelay(50) (wrapper 0x02008D9C ->
 * loop body 0x02008D90, 21 iterations), disable-IRQ (0x02008D20), re-read
 * the sequence } while the sequence is unchanged.  The sequence only moves
 * when the SDK IPC mailbox loop (0x0200BB00..0x0200BB8C, FIFO read port
 * 0x04100000 at pc 0x0200BB44) dispatches a word to a registered handler
 * (table indexed by word & 0x1F) — i.e. when the ARM7 SENDS A MESSAGE.
 * melonDS's ARM7 does so on a strict cadence; this responder answered only
 * the scripted one-shot, so the sequence never moved and the guest spun in
 * the wait for the whole step budget (halt state r0=0x00000008 r14=0x02008DB4
 * = the tail of one 21-iteration chunk, entered at step 18,578,180 right
 * after the request send 0x81B34E07 at step 18,578,090).
 *
 * melonDS ground truth (p4q_mel_io.txt; the IO trace is ARM9-side only, so
 * the ARM7's sends are seen as ARM9 recv-FIFO reads R32 0x04100000, all at
 * pc 0x0200BB44, ts in ARM9-cycle units).  Offsets below are relative to
 * the barrier reply read at ts 28,709,423:
 *
 *   word        n       first ts        period (measured)      handler
 *   0xC0240006  2,770   29,132,895      560,190 = one frame    id 6 (driver)
 *                           (+423,472; consecutive deltas 559,154..560,235)
 *   0x00008007  4,631   29,213,146      335,104 (med)          id 7
 *                           (+503,723; min 332,734 max 337,398 = the ARM7's
 *                            100 Hz tick, 33,513,982/100 = 335,140)
 *   0x00008087  1,578   30,208,472      983,040 (med)          id 7
 *                           (+1,499,049; the 3-tick variant)
 *
 * The stream is not idle traffic: the guest ANSWERS each 100 Hz tick with
 * the next card request (send 0x81B36C07 at ts 29,218,077 = +5,061 after the
 * tick at 29,213,146; send 0x81B39C07 at ts 29,552,703 = +4,402 after the
 * tick at 29,548,301) and the ARM7's per-frame 0xC0240006 is the "done"
 * heartbeat of that protocol — so the cadence drives both the wait above and
 * the card streaming that follows it (melonDS re-arms the bus at ts
 * 29,024,936 and issues the first ROMCTRL command at ts 29,034,972, both
 * well before the first reply; the delay gap never exceeds 141,230 ts).
 *
 * Rare words NOT modelled here (measured, out of this phase's window):
 * 0x0000006B x5, a burst at ts 50,485,392..50,498,023. */
static void nnk_p4r_enqueue(uint32_t word, uint64_t at) {
    if (nnk_p4r_qn >= NNK_P4R_QMAX) {        /* unreachable: schedule() stalls */
        if (nnk_p4r_drop_log < 8u) {         /* first, and it must be loud:   */
            fprintf(stderr, "ipc: p4r INTERNAL queue overflow 0x%08X at=%llu "
                    "step=%llu (schedule() must back-pressure first)\n",
                    word, (unsigned long long)at, (unsigned long long)steps);
            nnk_p4r_drop_log++;
        }
        return;
    }
    nnk_p4r_q[nnk_p4r_qn].word = word;
    nnk_p4r_q[nnk_p4r_qn].at = at;
    nnk_p4r_qn++;
}

static void nnk_p4r_arm(uint64_t t0) {
    if (nnk_p4r_armed) return;
    nnk_p4r_armed = 1;
    /* offsets/periods measured from the barrier reply (see block comment) */
    nnk_p4r_stream[0].word = 0xC0240006u; nnk_p4r_stream[0].period = 560190u;
    nnk_p4r_stream[0].next = t0 + 423472u;
    nnk_p4r_stream[1].word = 0x00008007u; nnk_p4r_stream[1].period = 335104u;
    nnk_p4r_stream[1].next = t0 + 503723u;
    nnk_p4r_stream[2].word = 0x00008087u; nnk_p4r_stream[2].period = 983040u;
    nnk_p4r_stream[2].next = t0 + 1499049u;
    fprintf(stderr, "ipc: p4r periodic producer armed t0=%llu step=%llu\n",
            (unsigned long long)t0, (unsigned long long)steps);
}

static void nnk_p4r_schedule(void) {
    int i;
    for (i = 0; i < 3; i++) {
        while (cycles >= nnk_p4r_stream[i].next) {
            /* Phase-5AI back-pressure: the ARM7's send FIFO is full, so the
             * ARM7 BLOCKS — the word is not produced yet and `next` stays put
             * (nothing is lost, no reordering inside a stream; the catch-up
             * burst on drain is what hardware does when the FIFO frees up). */
            if (nnk_p4r_qn >= NNK_P4R_QMAX) {
                if (!nnk_p4r_stalled) {
                    nnk_p4r_stalled = 1;
                    fprintf(stderr, "ipc: p4r ARM7 send BLOCKED (FIFO full: "
                            "qn=%u) — back-pressure, 0 words dropped "
                            "step=%llu cyc=%llu\n", (unsigned)nnk_p4r_qn,
                            (unsigned long long)steps,
                            (unsigned long long)cycles);
                }
                return;
            }
            if (nnk_p4r_stalled) {
                nnk_p4r_stalled = 0;
                fprintf(stderr, "ipc: p4r ARM7 send RESUMED step=%llu cyc=%llu\n",
                        (unsigned long long)steps, (unsigned long long)cycles);
            }
            nnk_p4r_enqueue(nnk_p4r_stream[i].word, nnk_p4r_stream[i].next);
            nnk_p4r_stream[i].next += nnk_p4r_stream[i].period;
        }
    }
}

/* ---- Phase-4S: the ARM7 request EFFECT — the card phase behind the requests
 *
 * THE PHASE-4R BARRIER, NOW MEASURED IN BOTH RUNS.  The SDK wait at
 * 0x0200ACC8 compares the snapshot *(0x0206CA5C) against the mailbox sequence
 * *(*(0x0206E580)) — i.e. against *(0x0206CAA0), the object the ARM9's own
 * init (pc 0x0200AC78) installs the pointer for right after the reply
 * 0xC0204006.  melonDS's ARM7 performs the requested card phase and
 * INCREMENTS THAT SEQUENCE (writer pc7 = 0x03802590, ARM7 WRAM), which is
 * what ends the wait; native's scripted ARM7 acked the request (Phase-4Q) but
 * never did the work, so the sequence stayed 0 and the wait never woke.
 *
 * MEASURED (melonDS; new instruments: MemWatch window + the ts on the
 * register probe; both captures in the scratch dir):
 *
 *   event                                              ts (ARM9 cycles)
 *   reply 0xC0204006 read                             28,709,423   (t0)
 *   ARM9 installs *(0x0206E580) = 0x0206CAA0, inits   ~28,717,247
 *   send 0x81B34807 (class 7)                         28,720,406
 *   send 0x81B34E07 (class 7)                         28,732,873
 *   wait 0x0200ACC8 entered (flags=1)                 28,732,934
 *   ARM7 writes *(0x0206CAA0) 0 -> 1 (pc7 0x03802590) 28,873,928   (t0+164,505)
 *   wait exits: r0=1 r1=0                             28,874,017
 *   ARM7 writes 1 -> 2                                29,213,542   (t0+504,119)
 *   ARM7 writes 2 -> 3                                29,548,696   (t0+839,273)
 *   ARM7 writes 3 -> 4                                29,883,744   (t0+1,174,321)
 *
 *  The later increments ride the ARM7's 100 Hz tick (335,104 ts measured in
 *  Phase-4R): they land +396/+446/+390 ts after each 0x00008007 word, i.e.
 *  the completion of the request the guest sent at the PREVIOUS tick (the
 *  guest sends 0x81B36C07 at 29,218,077, +5,061 after tick 1, and its
 *  completion is the 29,548,696 bump — 4,007 ts after that send would be
 *  wrong, so the rule modelled below is "next tick slot", not a fixed delay).
 *  Every wait exit lands one poll period (~89..109 ts) after the write: the
 *  wait's own delay chunk is 109 ts (regprobe: compares at 28,733,053,
 *  28,733,162, ...).
 *
 *  Modelled effect (nnk_p4s_*): class-7 request -> one completion at
 *  t0+164,505 (measured), then one per next tick slot (+396), each doing the
 *  real shared-RAM write the ARM7 does.  Nothing is faked: the sequence word
 *  really increments and the guest's wait really re-reads it.
 */
#define NNK_P4S_MAX 4
static uint64_t nnk_p4s_at[NNK_P4S_MAX];
static unsigned nnk_p4s_n;
static unsigned nnk_p4s_done, nnk_p4s_log;
static int nnk_p4s_first;

static void nnk_p4s_arm_at(uint64_t at) {
    if (nnk_p4s_n >= NNK_P4S_MAX) return;
    if (nnk_p4s_n && at <= nnk_p4s_at[nnk_p4s_n - 1]) return;  /* keep order */
    nnk_p4s_at[nnk_p4s_n++] = at;
}

static void nnk_p4s_fire(void) {
    uint32_t p = ld32(0x0206E580u);          /* the ARM9's sequence pointer */
    uint32_t obj = p ? p : 0x0206CAA0u;      /* measured ARM7 write target   */
    uint32_t s = ld32(obj);
    st32(obj, s + 1u);
    nnk_p4s_done++;
    if (steps >= 500000000ull && getenv("NNK_P4S_TRACE"))
        fprintf(stderr, "p5ay: arm7 bump 0x%08X->0x%08X step=%llu cyc=%llu "
                "pc=0x%08X lr=0x%08X snap=%u rec=%u pub=%u free_head=0x%08X\n",
                s, s + 1u, (unsigned long long)steps,
                (unsigned long long)cycles, last_pc, r[14],
                ld32(0x0206CA5Cu), ld32(0x0206CA6Cu), ld32(0x0206CA70u),
                ld32(0x0206CA58u));
    if (nnk_p4s_done <= 8u || (nnk_p4s_done & 63u) == 0u) {
        fprintf(stderr, "p4s: ARM7 card-phase effect seq 0x%08X -> 0x%08X "
                "obj=0x%08X ptr=0x%08X snap=0x%08X step=%llu cyc=%llu\n",
                s, s + 1u, obj, p, ld32(0x0206CA5Cu),
                (unsigned long long)steps, (unsigned long long)cycles);
    }
    if (!p && nnk_p4s_log++ < 4u)
        fprintf(stderr, "p4s: sequence pointer *(0x0206E580) is 0 — the ARM9 "
                "init (pc 0x0200AC78) has not run; bumped 0x0206CAA0 anyway "
                "step=%llu\n", (unsigned long long)steps);
}

static void nnk_p4s_request(uint32_t v) {
    /* Phase-5AY: the mailbox sequence is a HANDSHAKE, not a free-running
     * counter.  The ARM9 publishes a ring (0x0200AE88), sends the class-7
     * request, then waits for the ARM7 to advance *(*(0x0206E580)); its
     * reclaim (0x0200ACC8) consumes exactly one increment and then advances
     * the pool's own sequence (+0x04 = 0x0206CA5C).  Phase-4S measured the
     * wait exiting 89..109 ts after the write — the ARM7's word is never more
     * than ONE ahead of the pool's.  Arming a completion while the previous
     * one is still unconsumed banks a credit, and a banked credit lets
     * RECLAIM run twice on one publish: the second ring it takes is still the
     * chain it just appended, so `tail->next = head-of-ring` closes the
     * Phase-5AX 2-cycle.  Measured in the operator's state (probe below):
     * seq=0xA0 vs poolseq=0x9F right before the 546,142,360 cycle. */
    {
        uint32_t p = ld32(0x0206E580u);          /* the sequence pointer    */
        uint32_t obj = p ? p : 0x0206CAA0u;      /* the ARM7's write target */
        uint32_t shared = ld32(obj), pool = ld32(0x0206CA5Cu);
        if (shared != pool) {
            if (getenv("NNK_P4S_TRACE"))
                fprintf(stderr, "p5ay: request 0x%08X deferred: arm7 word "
                        "0x%08X still unconsumed (pool 0x%08X) step=%llu\n",
                        v, shared, pool, (unsigned long long)steps);
            return;
        }
    }
    if (!nnk_p4s_first) {
        nnk_p4s_first = 1;
        /* measured: first completion = reply + 164,505 (A); next = tick slot */
        if (nnk_p4r_armed) {
            uint64_t t0 = nnk_p4r_stream[0].next - 423472u;
            nnk_p4s_arm_at(t0 + 164505u);
            nnk_p4s_arm_at(nnk_p4r_stream[1].next + 396u);
        } else {
            nnk_p4s_arm_at(cycles + 143205u);
        }
        if (nnk_p4s_log++ < 4u)
            fprintf(stderr, "p4s: class-7 card request 0x%08X -> completion "
                    "at +%llu (measured A/B) step=%llu\n", v,
                    nnk_p4r_armed ? 164505u : 143205u,
                    (unsigned long long)steps);
        return;
    }
    /* one completion per request cycle: the next tick slot if none pending */
    if (!nnk_p4s_n && nnk_p4r_armed && nnk_p4r_stream[1].next) {
        nnk_p4s_arm_at(nnk_p4r_stream[1].next + 396u);
        if (nnk_p4s_log++ < 4u)
            fprintf(stderr, "p4s: class-7 card request 0x%08X -> completion "
                    "at next tick slot cyc=%llu step=%llu\n", v,
                    (unsigned long long)(nnk_p4r_stream[1].next + 396u),
                    (unsigned long long)steps);
    }
}

static void nnk_p4s_tick(void) {
    while (nnk_p4s_n && cycles >= nnk_p4s_at[0]) {
        nnk_p4s_fire();
        memmove(nnk_p4s_at, nnk_p4s_at + 1,
                (--nnk_p4s_n) * sizeof nnk_p4s_at[0]);
    }
}
