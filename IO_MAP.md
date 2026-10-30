# IO_MAP.md — Ni no Kuni DS (B2KJ): ARM9 boot IO ground truth (Phase-3A) + native IO engine status (Phase-3A2)

Scope: which MMIO registers the ARM9 **boot path actually touches**, measured
from the local melonDS ground truth, and the native runtime's honest
implemented-vs-stubbed status for each.

## How this was measured (reproduce)

Ground truth = melonDS (interpreter, FreeBIOS, real ROM) with two env-gated
hooks applied to `src/NDS.cpp` / `src/ARM.cpp` and built into
`build-ptrace/` (never `build/`):

* `tools/melonds-arm9-ptrace.patch` — ARM9 guest-PC trace
  (`MELONDS_ARM9TRACE_PATH/_N/_EXIT`, Phase-2A);
* `tools/melonds-io-trace.patch` — ARM9 IO **bus** trace at the six
  `ARM9Read*/Write*` dispatch sites in `NDS.cpp`
  (`MELONDS_IOTRACE_PATH`, `MELONDS_IOTRACE_N`); one line per access:
  `R8|R16|R32|W8|W16|W32 0x<addr> 0x<value> pc=0x<PC>` (PC = R[15]-8 ARM /
  R[15]-4 Thumb, same convention as the PC hook).

```sh
MELONDS_ARM9TRACE_PATH=<pc.txt> MELONDS_ARM9TRACE_N=5000000 \
MELONDS_IOTRACE_PATH=<io.txt>  MELONDS_IOTRACE_N=300000 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
python3 work/phase3a/agg_io_trace.py <io.txt>     # per-register table below
```

Capture used here: **5 000 000 ARM9 PCs, 204 685 IO accesses, 34 distinct
word-aligned registers** (covers boot stub, in-place decompressor, auto-load
walker, C-runtime start and early game init).

## Boot IO coverage table (ground truth vs native runtime, Phase-3A2)

"Native status" is the behaviour of the generated recompiler harness
(`nnktool recompile-all` + the IO model in `RUNTIME_ALL`, nnktool/src/recompile.rs):
the flat guest window is `0x02000000..0x03800000`; since Phase-3A2 loads/stores
to `0x04000000..0x04100014` are routed to io_read/write{8,16,32} handlers with
melonDS-derived semantics. Anything not listed below still reads 0 / drops the
write (and is logged where it halts the run).

| addr | register (melonDS source) | boot access (count, first pc) | native status (Phase-3A2) |
|---|---|---|---|
| `0x04000000` | DISPCNT | R32 x1 @`0x02002BF0`, W32 x1 @`0x02002C14` | **implemented** (latched read/write; no rendering side effects in this harness) |
| `0x04000004`/`0x04000006` | DISPSTAT / VCOUNT | VCOUNT **R16 x2421** @`0x02000808` (vblank poll); DISPSTAT itself: **0 accesses** in the measured set | **implemented, coarse**: ~1 cycle/instruction, 2130 cycles/scanline; vblank/hblank flags + VCOUNT derived from the cycle counter so vblank polls terminate. IRQ-enable bits stored; **no IRQ delivery** (see summary). |
| `0x040000B0..0x040000D0` | DMA0/1/2 SAD/DAD/CNT (melonDS map: DMA0=B0/B4/B8, DMA1=BC/C0/C4, DMA2=C8/CC/D0 — a single 32-bit CNT per channel) | SAD/DAD/CNT writes incl. **DMA1 SAD=`0x08000080`** (main-RAM mirror source) + DMA1 DAD=`0x0206F500` + DMA1 CNT=`0x80000020` (enable, immediate, 0x20 halfwords) from ITCM code @`0x01FF84C8..0x01FF84D0`; DMA0 CNT W32 x3, DMA2 CNT W32 x10 (values 0 = disables) | **implemented**: SAD/DAD/CNT latched; enabled **immediate** (timing=0) transfers run on the CNT write — matches the one observed enabled transfer. vblank/hblank timing not hooked (none observed enabled in the measured set). *Phase-3A mislabel corrected: `0x08000080` @`0x01FF84C8` is DMA1 **SAD** (source = RAM mirror), not a "DMA0 enable with start-of-vblank timing" — melonDS `case 0x040000BC: DMAs[1].SrcAddr`.* The mirror source is outside the flat window (see caveats). |
| `0x04000100..0x0400010E` | TM0-3 CNT_L/H | W16 x3 (0x04000100 x1 @`0x020082E4`, 0x04000102 x2 @`0x020082D8`) | **implemented, coarse**: reload/ctrl latched; counter reads = reload + cycles/prescale while enabled. No cascade, no overflow IRQ. |
| `0x04000180` | IPCSync9 | RW16 x537 (R16 x529 + W16 x8), first R16=7 @`0x0200B984` (ARM9<->ARM7 sync handshake) | **implemented + ARM7 responder stub (measured)**: after each ARM9 echo write the responder holds the echoed nibble for the **observed per-round read counts (33, 81, 78, 84, 78, 80, 80)** then advances 7->6->...->0 — reproduces the full 537-access melonDS bus sequence value-for-value. On the terminating echo (send=0) the responder raises the ARM7 ready flag the boot waits on next (bit12 of `[0x02FFFF8C]`, the `wait(12,1)` at `0x02008E1C`, observed to succeed on the FIRST call in melonDS — before any FIFO traffic). |
| `0x04000184` | IPCFIFOCNT | RW16 x7 (R16 x6 + W16 x1), first W16=0xC408 @`0x0200B95C` | **implemented (subset)**: enable/clear-irq bits latched; reads expose send-empty (bit0) + recv-empty (bit8) status. No IRQ on FIFO events. |
| `0x04000188` | IPCFIFO_WR (to ARM7) | **W32 x1** @`0x0200BAD4` (value `0x037A404D`) — the *only* FIFO write in the measured boot (happens *after* the `wait(12,1)` at melonDS index 3,635,196 vs 3,633,671) | **implemented (scripted)**: the observed pair `0x037A404D -> answer 0x0000004D` is answered by the responder stub; any **other** request halts the run honestly (logged with pc) instead of guessing. *Phase-3A count corrected: was listed x170 (that is CARD_COMMAND's count).* |
| `0x04100000` | **IPCFIFO_RD** (ARM7->ARM9 FIFO data port; melonDS `case 0x04100000: return IPCFIFO7.Peek()`) | R32=0x4D @`0x0200BB44` | **implemented**: pops the responder FIFO; empty read raises the not-empty status bit (no IRQ). *Phase-3A mislabel corrected: this row was "geometry command port"; it is the IPC FIFO read port — the measured 0x4D is the FIFO answer.* |
| `0x040001A0` | SPICNT (cartridge SPI) | RW16 x340, first R16=0x8000 @`0x020168B4` | **implemented** (latched; bit15 gates card-bus starts). SPI devices (firmware etc.) not modelled — boot path only uses the card-bus half. |
| `0x040001A4` | ROMCNT (card bus status) | **R32 x180,033** (busy-poll storm, first R32=0x20000000 @`0x020168A0`) + W32 x170 | **implemented (subset)**: bit31 busy / bit23 DRQ poll semantics; a write with bit31 set + SPICNT.15 starts the transfer. *Phase-3A count corrected: was listed RW32 x22 (that is DIV's count); the ~180 k polls attributed to DIV_NUMER in the old note are these ROMCNT busy-polls.* |
| `0x040001A8/0x040001AC` | CARD_COMMAND[0..7] | W32 x170 @`0x020168F4`/`0x020168FC` (card commands) | **implemented**: 8-byte command buffer. Only cmd **0xB7** (read-by-address) is decoded; other opcodes are logged (`io: card cmd 0x..`) and start an empty transfer rather than fabricating data. |
| `0x04100010` | **CARD_DATA_RD** (card ROM data FIFO; melonDS `case 0x04100010: return NDSCartSlots[0]->ReadROMData(0)`) | R32 x20489, first R32=0x20152253 @`0x02016B3C` | **implemented**: cmd 0xB7 streams **real ROM bytes** from the commanded address in the raw `.nds` image (`NNK_ROM_PATH`, 512 MiB); datasize decoded from ROMCNT, busy/DRQ clear on completion. *Phase-3A mislabel corrected: this row was "GXSTAT GE busy-wait poll"; the ~20 k reads are the boot's card data stream (filesystem/overlay reads), not geometry-engine polls.* |
| `0x04000204` | EXMEMCNT | RW16 x57, first R16=0xE880 @`0x02006268` | **implemented** (latched read/write; no arbitration side effects). |
| `0x04000208` | IME | RW16/32 x39, first W32=0x04000000 @`0x02000804` (entry stub; bit0=0 -> IRQs off) | **implemented** (bit0 latched). No IRQ delivery — the measured boot runs with IME=0 (IRQs off), so nothing in the captured trace observes this. |
| `0x04000210` | IE | RW32 x13 @`0x020060C4` | **implemented** (latched). No delivery. |
| `0x04000214` | IF (ack) | RW32 x4 @`0x02006124` | **implemented** (write-1-to-clear on the latched flags). No sources fire. |
| `0x04000244..0x04000248` | VRAMCNT_E/F/G + WRAMCNT | W8=3 @`0x02009220` (maps ITCM-adjacent banks) | **implemented (stored-only)**: byte-wide reads/writes latch; **no remapping side effects** (VRAM banks/WRAM mapping not re-routed) — documented gap. |
| `0x04000280` | **DIVCNT** | RW16 x22, first R16=0 @`0x0200F560` | **implemented**: mode latched; division computed when DIV_DEN is written. |
| `0x04000290/0x04000294` | DIV_NUMER low/high (64-bit) | RW32 x22 each @`0x0200F558` | **implemented** (latched; 32/32, 64/32, 64/64 modes; quotient + remainder published). |
| `0x040002B0` | SqrtCnt | RW16 x22 @`0x0200F574` | **implemented** (mode latched; result computed on input write). |
| `0x040002B8/0x040002BC` | SQRT_RESULT low/high | RW32 x22 each @`0x0200F568` | **implemented**. |
| `0x04000300` | POSTFLG (PostFlag9) | R16=1 @`0x0201C54C` | **implemented** (reads 1 after the entry stub writes it). |
| `0x04001000` | (1 read @`0x02000CD8` + 1 write @`0x02000CE0`, value 0) | unmodelled | **stub, harmless**: melonDS read 0x00000000 — the native read-0 matches; the write of 0 is dropped (no observable effect). Not in the Phase-3A table; listed for completeness. |
| `0x04000304` | PowerControl | measured in the set | **implemented** (latched). |
| `0x04000600` | GXSTAT (geometry engine status) | **0 accesses in the captured boot IO trace** | not implemented — the boot never touches it in the measured window; if it is reached later, reads return 0 (a GE-busy poll would pass instantly) and that divergence will surface honestly. |

Notes and honest caveats:

1. **The first 10 000 ARM9 PCs touch only two IO addresses** — the entry
   stub's `W32 IME=0x04000000` (`0x02000804`) and the vblank poll
   `R16 VCOUNT` (`0x02000808`, read 0 in both melonDS and native, so the
   flat model was *accidentally* faithful there). Every other register above
   is reached **after** the decompressor/auto-load stage.
2. The boot's real IO sequence after auto-load: IPC sync with the ARM7
   (`0x0200B984`), card-bus init + SPI, timer0 setup, DMA1 armed **from ITCM
   code at `0x01FF84C8`** (SAD=RAM mirror, immediate 0x20-halfword copy),
   a **180 k-read ROMCNT busy-poll storm** and a ~20 k-read **card data
   stream** at `0x04100010` (Phase-3A called these "DIV_NUMER polls" and
   "GXSTAT polls" — both corrected above against melonDS `NDS.cpp`).
3. `0x04000004` VCOUNT polling appears 2421 times; the Phase-3A flat model
   returned 0 which happened to equal melonDS's value at boot. Phase-3A2
   models coarse scanline timing instead, so polls that expect a *non-zero*
   scanline terminate for the right reason.
4. Writes to `0x01FF8000..0x01FF87FF` (ITCM) and `0x027E0000` (RAM alias /
   stack area in the auto-load list) are **outside** the current flat
   window; melonDS maps them (ITCM) or aliases them into main RAM. This is
   the next model gap after the IO registers themselves — see CODE_MAP.md
   Phase-3A.
5. The **ARM7 is still a stub**, not an emulator: the IPCSync handshake and
   the one observed FIFO request/answer pair are scripted to the values
   measured in the bus trace; an unscripted request halts the run with a
   log line instead of being guessed.

## Native runtime status summary (honest, Phase-3A2)

| unit | status |
|---|---|
| CP15 (MPU/caches, `mcr p15`) | executed as no-ops (matches melonDS for the boot's cache maintenance: no observable effect on the PC trace) |
| SWI/BIOS HLE | 0x09 Div, 0x0B CpuSet, 0x0C CpuFastSet, 0x0D Sqrt implemented (FreeBIOS-exact); boot path uses 0 SWIs |
| DISPSTAT/VCOUNT, DISPCNT, timers, DMA (immediate), IPCSync+ARM7 responder stub, IPCFIFO (scripted pair), SPICNT/ROMCNT/CARD_COMMAND/CARD_DATA (cmd 0xB7 streams the real ROM), IME/IE/IF, DIV, SQRT, EXMEMCNT, POSTFLG, VRAMCNT/WRAMCNT (stored-only), PowerControl | **implemented** with the semantics and simplifications per the table |
| IRQ delivery (IME-gated exception dispatch) | **not implemented** — IE/IF/IME latch but no asynchronous dispatch; the measured boot runs with IME=0 so the captured trace does not exercise it |
| DMA vblank/hblank timing triggers | **not implemented** — transfers run on the CNT-enable write (see table row) |
| Unmapped IO (incl. anything outside `0x04000000..0x04100014`) | reads 0 / drops writes, exactly as before — never made to look successful |
| ARM7 | not executed; scripted responder only (IPCSync: hold-33-reads-then-advance, measured from the bus trace; one FIFO pair); full ARM7 comes later |
| Boot depth with the Thumb dispatch image (Phase-3A3) | halt advanced from `0x0201C178` (Phase-3A2) to **`0x0201C4B8`** — the `blx` -> Thumb interworking at `0x0201C178` -> `0x020004A8` is now translated (73 120 dispatch cases / 2 020 funcs, full v5TE Thumb set + interworking); 10 M PC trace vs melonDS: 3 633 818 / 10 000 000 = 36.3 % strict prefix, first divergence index 3 633 818 (melonDS `0x0201C4C0` vs native `0x0201C4AC`) |

Nothing in the table above is "made to look successful": an unmodelled
register reads 0 / drops the write, an unscripted IPC FIFO request halts the
run with a log line, and an unmodelled card command logs its opcode — any
boot code that polls a non-zero hardware flag diverges visibly at that point.

## Phase-4A — the 0x0201C4B8 poll loop broken; next halt = ITCM execution

**Root cause of the Phase-3A3 halt.**  Subroutine `0x0200BA4C` (called by the
loop at `0x0201C4B8`) is the SDK `test_bit(bit=r0, word_index=r1)`: it tests
bit r0 of `[0x02FFFC00 + r1*4 + 0x388]`.  The loop is `wait(13,1)` → bit 13
of **`[0x02FFFF8C]`** — *not* an IO register: it is main RAM at 4 MiB-mirror
offset `0x3FFF8C`, the **ARM7's IPC event-mask word** (melonDS routes both
CPUs' `0x02000000..0x03FFFFFF` to `MainRAM[addr & 0x3FFFFF]`; the ARM7's SDK
mirrors the same cell at `0x027FFF8C`, its own mask at `+0x38C`, the ARM9's
mask at `[0x02FFFF88]`).  The real ARM7's IRQ-driven init posts event bits
there one at a time; our scripted ARM7 never did → infinite poll.

**Ground truth (new melonDS harness, all env-gated, captured this phase):**

* `MELONDS_MEMWATCH_PATH` (NDS.cpp, all six write dispatchers): logs every
  write into main-RAM `0x3FFF80..0x3FFFB0` with BOTH CPUs' PCs and
  `a9idx` = the ARM9 PC-trace index at that moment (via
  `ARM9TracePCCount()` in ARM.cpp).  The ARM7 posts (15 writes, from
  `pc7=0x037FE3D4`, a handler in shared WRAM) arrive in the order
  bit12, bit13, bit16, bit17, bit7, bit11, bit14, bit5, bit10, bit6, bit9,
  bit8, bit4 at a9idx 3,633,523 / 3,633,806 / 3,635,544 / 3,635,618 /
  3,650,209 / 3,650,651 / 3,650,668 / 3,652,929 / 3,654,408 / 3,768,189×4.
  bit13 lands while the ARM9 is *inside* `test_bit` of wait(13,1) iteration 2
  (`pc9=0x0200BA64`) — melonDS exits that loop on iteration 3.
* `MELONDS_ARM7TRACE_PATH` (ARM.cpp, ARMv4 interpreter): ARM7 PC trace.
* `MELONDS_ITCM_DUMP` (ARM.cpp): one-shot 32 KiB ITCM dump at first ITCM
  execution — fires exactly at `pc=0x01FF845C`.

**Fix (nnktool `recompile.rs` RUNTIME_ALL + emitted `run()` prologue).**  An
**ARM7 init-event chain** (`arm7_evt_tick`, once per dispatch after
`++steps`) writes the measured word values into `[0x02FFFF8C]` at step
thresholds = the measured `a9idx` values (`steps` == a9idx throughout the
matched prefix).  Both the splitter (prologue kept verbatim) and mono builds
carry it; stderr logs each post (`arm7 evt step=… val=…`).

**Result (honest depth).**  `tools/build_split.sh` binary on the stored
padded image: the wait(13,1) loop **exits on iteration 3 exactly like
melonDS**, streams re-align; native reaches **step 3,634,038 / trace
3,634,037 PCs**, new halt at **`0x020093E4` → `bl 0x01FF845C` (ITCM)**:
"dispatch to untranslated pc".  cmp_pc_traces vs the 10 M melonDS capture:
**strict common prefix 3,634,037 / 10,000,000 — 100 % of the native trace
matches melonDS** (was 3,633,818 with the loop-exit divergence at 3,633,818
melonDS `0x0201C4C0` vs native `0x0201C4AC` — gone).  Root cause of the new
halt: **ITCM code execution** — the boot copies/constructs code into
`0x01FF8000..0x01FFFFFF` and calls it (the dumped routine at `0x01FF845C` is
the SDK DMA-start helper: OS lock (`bl 0x02008D20`), program channel
SAD/DAD/CNT at `0x040000B0+ch*12` — which the runtime's DMA1 engine already
executes — plus a flag-gated completion wait).  The runtime has no ITCM
region: the guest's fill stores are dropped (out of window) and the fetch
has no case.  The bytes are **not** in the ARM9 stored/decompressed images
nor the ARM7 image (signature search) — they are constructed at runtime;
`tools/add_itcmwatch.py` (not yet applied) adds the CP15.cpp write watch that
identifies the writer, and `tools/find_itcm_src.py` searches for sources.

**Next model gaps after ITCM, in order (measured):** (1) ITCM region +
dispatch of ITCM PCs; (2) **IRQ delivery** — at a9idx ≈ 3,650,000 the
melonDS ARM9 runs in its BIOS IRQ handler (`pc9=0xFFFF01DC`, IME=1); the
native runtime latches IE/IF/IME but never dispatches exceptions; (3) the
ARM7 is still scripted — event-chain clock = `steps`, so timing degrades
after the first post-divergence (each post logged).
