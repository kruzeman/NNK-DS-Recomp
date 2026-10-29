# IO_MAP.md — Ni no Kuni DS (B2KJ): ARM9 boot IO ground truth (Phase-3A)

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

## Boot IO coverage table (ground truth vs native runtime)

"Native status" is the behaviour of the generated recompiler harness
(`nnktool recompile-all`): the flat guest window is `0x02000000..0x03800000`;
everything at `0x04000000+` is **outside** the window — reads return 0,
writes are dropped. CP15 `mcr/mrc` are no-ops. Nothing below is implemented
yet; the table is the honest target list for the IO-model phase.

| addr | register (melonDS source) | boot access (count, first pc) | native status |
|---|---|---|---|
| `0x04000000` | DISPCNT | R32 x2 @`0x02002BF0` | stub (reads 0) |
| `0x04000004` | DISPSTAT/VCOUNT | R16 x2421 @`0x02000808` (vblank poll + engine polls) | stub (reads 0 -> poll passes instantly) |
| `0x040000B0..0x040000D0` | DMA0/1/2 SAD/DAD/CNT (0xB0,0xB4,0xB8,0xBC,0xC0,0xC4,0xD0) | 7 CNT writes incl. `0x08000080` (DMA0 enable, timing=start-of-vblank) @`0x01FF84C8` (ITCM code) | stub — no DMA engine |
| `0x04000100` | TM0CNT_L/H (timer 0) | W16 x3 @`0x020082D8` | stub — no timers |
| `0x04000180` | IPCSync9 | RW16 x537, first R16=7 @`0x0200B984` (ARM9<->ARM7 sync handshake) | stub — reads 0 (no ARM7 responder) |
| `0x04000184` | IPCFIFOCNT | RW16 x7, first W16=0xC408 @`0x0200B95C` | stub — no IPC FIFO |
| `0x04000188` | IPCFIFO_WR (to ARM7) | W32 x170 @`0x0200BAD4` | stub — writes dropped |
| `0x040001A0` | SPICNT (cartridge SPI) | RW16 x340, first R16=0x8000 @`0x020168B4` | stub |
| `0x040001A4` | ROMCNT (card bus status) | RW32 x22, first R32=0 @`0x0200F558` | stub |
| `0x040001A8` | CARD_COMMAND[0..3] | W32 x170 @`0x020168F4` (card commands) | stub |
| `0x040001AC` | CARD_COMMAND[4..7] | W32 x170 @`0x020168FC` | stub |
| `0x04000204` | EXMEMCNT | RW16 x57, first R16=0xE880 @`0x02006268` | stub |
| `0x04000208` | IME | RW16/32 x39, first W32=0x04000000 @`0x02000804` (entry stub; bit0=0 -> IRQs off) | stub |
| `0x04000210` | IE | RW32 x13 @`0x020060C4` | stub — no IRQ controller |
| `0x04000214` | IF (ack) | RW32 x4 @`0x02006124` | stub |
| `0x04000244` | VRAMCNT_E/F/G + WRAMCNT | W8=3 @`0x02009220` (maps ITCM-adjacent banks) | stub |
| `0x04000280` | **DIVCNT** | RW16 x22, first R16=0 @`0x0200F560` | stub — DIV unit not modelled |
| `0x04000290/0x04000294` | DIV_NUMER low/high (64-bit) | RW32 x22 each @`0x0200F558` | stub |
| `0x040002B0` | SqrtCnt | RW16 x22 @`0x0200F574` | stub — sqrt unit not modelled |
| `0x040002B8/0x040002BC` | SQRT_RESULT low/high | RW32 x22 each @`0x0200F568` | stub |
| `0x04000300` | POSTFLG (PostFlag9) | R16=1 @`0x0201C54C` | stub (reads 0; value 1 = booted) |
| `0x04100000` | geometry command port (GXFIFO write side; melonDS `case 0x04100000`) | R32=0x4D @`0x0200BB44` | stub |
| `0x04100010` | GXSTAT (geometry engine status) | R32 x20489, first R32=0x20152253 @`0x02016B3C` (GE busy-wait poll) | stub (reads 0 -> poll passes instantly) |

Notes and honest caveats:

1. **The first 10 000 ARM9 PCs touch only two IO addresses** — the entry
   stub's `W32 IME=0x04000000` (`0x02000804`) and the vblank poll
   `R16 VCOUNT` (`0x02000808`, read 0 in both melonDS and native, so the
   flat model is *accidentally* faithful there). Every other register above
   is reached **after** the decompressor/auto-load stage.
2. The boot's real IO sequence after auto-load: IPC sync with the ARM7
   (`0x0200B984`), card-bus init + SPI, timer0 setup, 7 DMA channels armed
   (one **from ITCM code at `0x01FF84C8`**), ~180 k DIV_NUMER polls and
   ~20 k GXSTAT polls — i.e. the next barriers need DIV, IPC/ARM7, timers,
   DMA and the GE status model, in that order of observed traffic.
3. `0x04000004` VCOUNT polling appears 2421 times; the flat model returns 0
   which happens to equal melonDS's value at boot, but any poll that expects
   a *non-zero* scanline will diverge the instant it is reached.
4. Writes to `0x01FF8000..0x01FF87FF` (ITCM) and `0x027E0000` (RAM alias /
   stack area in the auto-load list) are **outside** the current flat
   window; melonDS maps them (ITCM) or aliases them into main RAM. This is
   the next model gap after the IO registers themselves — see CODE_MAP.md
   Phase-3A.

## Native runtime status summary (honest)

| unit | status |
|---|---|
| CP15 (MPU/caches, `mcr p15`) | executed as no-ops (matches melonDS for the boot's cache maintenance: no observable effect on the PC trace) |
| SWI/BIOS HLE | 0x09 Div, 0x0B CpuSet, 0x0C CpuFastSet, 0x0D Sqrt implemented (FreeBIOS-exact); boot path uses 0 SWIs |
| IME/IE/IF, IPCSync/FIFO, DIV, SQRT, timers, DMA, EXMEMCNT, DISPSTAT/VCOUNT, DISPCNT, VRAMCNT, POSTFLG, GXSTAT | **stubbed**: IO space outside the flat window (reads 0, writes dropped) |
| ARM7 | not executed; no IPC responder |

Nothing in the table above is "made to look successful": an unmodelled
register simply reads 0 / drops the write, and any boot code that polls a
non-zero hardware flag will diverge there — exactly what the next IO-model
phase has to implement (DIV first, then IPC/ARM7 responder, timers, DMA,
IRQ controller, GE status).
