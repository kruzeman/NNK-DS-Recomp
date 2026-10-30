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

## Phase-4B — ITCM/TCM memory model + execute-from-ITCM; barrier = ARM9 IRQ delivery

Task t_7553cbbb, 2026-10-30.  Builds on Phase-4A (merge 10c1ae3).  All
numbers below are from `tools/build_split.sh` binaries run on the stored
padded image; every capture lives under
`/home/hermes/.hermes/profiles/coder/cache/scratch/`.

### What the runtime models now (new in 4B)

1. **CP15 TCM configuration** (melonDS `CP15.cpp` semantics, `id =
   (crn<<8)|(crm<<4)|opc2` exactly as melonDS's ARMInterpreter encodes it):
   control reg `0x100` (bit16 DTCM enable, bit18 ITCM enable, mask
   `0x000FF085`), DTCM base reg `0x910`, ITCM size reg `0x911`.  The
   emitter now emits `cp15_read(0xNNN)` / `cp15_write(0xNNN, rX)` instead of
   the old constant-0 `cp15_reg` read / no-op write.  **Initial state = the
   ARM9 BIOS's post-init values**, measured with the new env-gated
   `MELONDS_CP15TRACE_PATH` hook (`tools/add_cp15trace.py`):
   `control 0x00052078 -> stored 0x00052000`, `0x911 = 0x20` (ITCMSize =
   `0x02000000`), `0x910 = 0x0300000A` (DTCM base `0x03000000`, 16 KiB).
   The boot stub then re-programs both TCMs at `0x02000A84..0x02000B28`
   through `cp15_write` — all four writes now land with values byte-equal to
   the melonDS CP15 trace.
2. **ITCM + DTCM data routing** in `ld8/16/32`, `st8/16/32`, in melonDS's
   data-path order (ITCM, DTCM, then the bus): ITCM hit =
   `itcm[addr & 0x7FFF]` when `addr < itcm_size` (melonDS's single check —
   with the measured size `0x02000000` the `0x01FF8000..0x01FFFFFF` window
   maps 1:1 and main RAM stays outside it), DTCM hit =
   `dtcm[addr & 0x3FFF]` when `(addr & mask) == base` (the disabled base
   `0xFFFFFFFF` can never match, so the same expression works with TCMs
   off).
3. **Execute-from-ITCM**: the loader's auto-load block (`dst=0x01FF8000`,
   `len=0x540`, source `0x0206C5C0` in the decompressed image — verified
   byte-identical to the melonDS `MELONDS_ITCM_DUMP` capture for the full
   `0x540`) is translated by a **second nnktool image** and spliced into the
   flat-window image:
   `tools/extract_itcm_blob.py` -> `tools/mk_itcm_seeds.py <extended trace>`
   (74 distinct observed ITCM PCs as ARM seeds) ->
   `nnktool recompile-all <blob> <itcm.c> --base 0x01FF8000` (153 cases) ->
   `tools/merge_itcm_cases.py <main.c> <itcm.c> <merged.c> <trace>` (reports
   observed-PCs-without-a-case; 0 here).  `tools/split_dispatch.py` now
   gives the exactly-one band below `GBASE` (`(0x01FF8000-0x02000000)>>15 =
   -1`) its own part `nnk_part_itcm.c` + a dedicated dispatch branch
   (`pc >= NNK_ITCM_LO && pc < NNK_ITCM_HI`); `nnk_rt.h` always declares
   `nnk_part_itcm` so the header bytes (a cache key) stay stable.
4. **GBA slot open-bus** (empty slot): reads of `0x08/0x09` return
   `((addr>>1) & 0xFFFF) | openbus[(EXMEMCNT>>2)&3]` with melonDS's table
   `{0xFE08,0,0,0xFFFF}`, deselection (EXMEMCNT bit7) returns 0, `0x0A`
   SRAM returns `0xFF` — `GBACart.cpp`/`NDS.cpp` semantics.  `EXMEMCNT`'s
   **BIOS initial value `0xE880`** is measured (first read-back before any
   guest write: `R16 0x04000204 0x0000E880 pc=0x02006268` in the melonDS IO
   trace), so the guest's read-modify-write chain reproduces melonDS
   exactly (`0xE880 -> 0xE800 -> 0xE80C -> 0x680C -> ...`, logged as
   `io: EXMEMCNT=... gba_decay=...` on stderr).
5. The `NNK_STEP_LIMIT` probe knob now actually works (`run()` tested the
   `STEP_LIMIT` macro; it tests the `step_limit` variable the env sets).

### Ground truth captured this phase (scratch, regenerable)

* `melonds_arm9_trace_itcm.txt` — 10 000 000 PCs, **extended window
  `LO=0x01FF8000 HI=0x02400000`** (the window now covers ITCM *and* the
  main-RAM window, so one file compares against both views);
  `melonds_arm9_trace_37m.txt` (3 703 225) and the Phase-4A-window
  `melonds_arm9_trace_10m.txt` unchanged.
* `melonds_cp15.txt` — 81 057 CP15 writes/TCM reads (BIOS at `0xFFFEFFFC`
  then the boot stub at `0x02000A84..0x02000B28`).
* `itcm_dump2.bin` (melonDS ITCM at first exec) vs
  `itcm_native_dump.bin` (runtime `NNK_ITCM_DUMP` at first ITCM dispatch):
  **`cmp` identical (32 768 bytes)** — the loader copy the pre-translated
  ITCM cases execute is byte-exact ground truth.
* `memwatch_3ffc30.txt`, `memwatch_6f53e.txt` — widened MemWatch bands
  (`tools/widen_memwatch.py`); `memwatch_preentry.txt` — **114 972
  pre-entry main-RAM writes** (all `a9idx=0`: ARM9 BIOS + ARM7 boot
  initialization, incl. the `0x027FFE00` "NINOKUNI" header block and the
  `0x027FFC00` firmware user-settings block) via `tools/watch_preentry.py`
  — *captured but deliberately NOT applied yet* (see gaps).
* IO trace `melonds_io_trace_boot.txt` (Phase-3A capture) pinned the ITCM
  DMA register writes (`W32 0x040000BC/0x040000C0/0x040000C4` at
  `pc9=0x01FF84C8..0x01FF84D0`) and the EXMEMCNT sequence.

### Divergences found, root-caused and fixed (poll/exit discipline)

| # | strict prefix | first divergence | root cause (measured) | fix |
|---|---|---|---|---|
| 0 | 3 634 037 (Phase-4A) | `bl 0x01FF845C` untranslated | no ITCM region, no ITCM cases | (1)+(2)+(3) |
| 1 | **3 634 239** | melonDS `0x0201C660` vs native `0x0201C1B8` | `popeq {..,pc}` at `0x0201C1B4` not taken: the checker `0x0201C198` reads `ldrh [0x02FFFC30]` expecting `0xFFFF`, native sees 0.  The value chain: ITCM DMA1 halfword fill (SAD `0x08000080`, DAD `0x0206F500`, count 32, fired at `0x01FF84D0`) reads **GBA-slot space**, which is empty-slot open-bus `0xFFFF` in melonDS (EXMEMCNT `0x680C` -> decay `0xFFFF`) but unmapped-0 in the runtime; the fill lands in `0x0206F500..0x0206F53E` (memwatch `pc9=0x01FF84D0 a9idx=3634066`), the game copies `0x0206F53E -> 0x02FFFC30` at `0x0201C628`, the checker gates on it | (4) GBA bus + EXMEMCNT init |
| 2 | **3 635 491 (final)** | melonDS `0x01FF8000` vs native `0x0200064A` | **first ARM9 IRQ delivery** — see stop report | not implemented (below) |

### Honest stop report — exact barrier and cause

**Match length: strict common prefix = 3 635 491 / 10 000 000 (36.4 % of
the extended-window melonDS capture; 100 % of the native prefix reaches the
barrier).**  Cross-check: against the old main-RAM-only capture the
window-filtered prefix is 3 635 447, and 3 635 491 - 44 = 3 635 447 — the
44 ITCM lines in the native prefix account for the difference exactly.
`in-order matched after resync` = 3 635 491 (no resync possible past the
barrier).  No unexplained divergence remains: both divergences above are
root-caused with capture evidence and fixed.

**Barrier (index 3 635 491; main-RAM window index 3 635 447): ARM9
interrupt delivery.**  melonDS leaves the matched stream into the SDK's
**ITCM IRQ entry stub `0x01FF8000`** (its first in-window handler PC; the
vector/BIOS hop at `0xFFFF0018..0xFFFF01DC` sits outside the window and is
not logged), then the game's IRQ handler at `0x0200BAE8`.  The native
runtime cannot take this interrupt at all, for three measured reasons:

* **no IRQ sources**: IE/IME are latched (`IE = 0x00040008` incl. bit 18,
  `IME = 1` since `pc=0x02000CC0`, from the melonDS IO trace) but nothing
  ever raises `IF`, so `ime && (ie & if)` is never true;
* **no exception entry**: the CPSR model is single-bank (no SPSR, no IRQ
  banked LR) and no code path jumps to `ExceptionBase+0x18`;
* **no BIOS**: `0xFFFF0018..` is outside every translated region, so even a
  naive vector jump would halt.

**What the native run does instead:** it spins in the SDK wait loop
`0x0201C6BC: ldrh r0,[r4]` with `r4 = 0x0206F460` (called from
`lr = 0x0200BADC`), pacing with `svc 0x03 WaitByLoop` at `0x02000648`
(28 052 107 HLE hits), until the 200 M step budget:
`halt_pc=0x0201C6B4 halt=step budget exhausted`.  `[0x0206F460]` stays 0
because in melonDS it is the *IRQ handler path* that raises it — i.e. the
wait loop is waiting for exactly the interrupt the runtime never delivers.
**Display init is behind this barrier, so no guest frame reaches
`gx_display` this phase** (task option: honest stop-report).

### Next model gaps, in order (measured)

1. **IRQ delivery**: raise `IF` from real sources (first candidate: the
   ARM7-initiated IPC interrupt — `IE=0x00040008` bit18, ARM7 posts start
   at `a9idx 3 635 5xx`), then exception entry (LR_irq/SPSR/IRQ mode), then
   the BIOS stub hop — either HLE the FreeBIOS-equivalent jump-to-user-
   handler (user pointer -> `0x01FF8000`, already translated) or translate
   a BIOS region — and `subs pc, lr, #4` return semantics (needs SPSR).
2. **Pre-entry BIOS/ARM7 memory state**: `memwatch_preentry.txt` has all
   114 972 writes measured; apply them as `guest_init` initial state (the
   `0x02FFFC30` user-settings cell happened not to be read before the game
   overwrote it, so the prefix matches without them — later boot will read
   them).
3. **ARM7 executed (or a longer script)**: only the measured event chain +
   IPC sync/one FIFO pair exist; post-divergence `steps`-clock drift is
   documented and every post is logged.
4. CP15 cache/TLB/PU writes stay no-ops (nothing observed requires them).

### Reproduce (Phase-4B)

```
# melonDS ground truth (patches: tools/add_cp15trace.py,
# tools/widen_memwatch.py, tools/watch_preentry.py + the Phase-4A ptrace
# patch): extended-window 10M capture
MELONDS_ARM9TRACE_PATH=$S/melonds_arm9_trace_itcm.txt \
MELONDS_ARM9TRACE_LO=0x01FF8000 MELONDS_ARM9TRACE_HI=0x02400000 \
MELONDS_ARM9TRACE_N=10000000 MELONDS_ARM9TRACE_EXIT=1 \
MELONDS_CP15TRACE_PATH=$S/melonds_cp15.txt MELONDS_ITCM_DUMP=$S/itcm_dump2.bin \
xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "<ROM>"
# ITCM image + merge + split build + run
python3 tools/extract_itcm_blob.py /home/hermes/NiNoKuni/work/ghidra_src/arm9.bin $S/itcm_blob.bin
python3 tools/mk_itcm_seeds.py $S/melonds_arm9_trace_itcm.txt 0x540 $S/seeds_itcm.tsv
nnktool recompile-all $S/itcm_blob.bin $S/itcm_image.c --base 0x01FF8000 \
    --entry 0x01FF8000 --seeds $S/seeds_itcm.tsv
nnktool recompile-all work/ghidra_src/arm9.bin work/phase3a/arm9_thumb.c \
    --base 0x02000000 --entry 0x02000800 --seeds work/phase3a/seeds_phase3a3.tsv
python3 tools/merge_itcm_cases.py work/phase3a/arm9_thumb.c $S/itcm_image.c \
    work/phase3a/arm9_thumb_itcm.c $S/melonds_arm9_trace_itcm.txt
NNK_IMAGE_C=work/phase3a/arm9_thumb_itcm.c \
NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=20000000ull" tools/build_split.sh
NNK_ROM_PATH="<ROM>" work/split/arm9_split work/phase3a/arm9_stored_padded.bin \
    > $S/native.txt 2> $S/native_hle.txt
python3 tools/cmp_pc_traces.py --lo 0x01FF8000 --hi 0x02400000 \
    $S/melonds_arm9_trace_itcm.txt $S/native.txt
```
