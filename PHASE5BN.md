# Phase-5BN — the per-step tick is now event-driven: **2.0x**, behaviour byte-identical

Branch `wt/t_0edf9cba` (master `9f66a76`).  The per-step hook `nnk_dma_tick()`
(VBlank edge, timer overflow, IPC tick, card stall, 4-channel DMA loop,
`vcount()`) ran on **every** guest step and cost about half the wall time of a
run.  It now runs only when it has something to do — 449,848 times in 200 M
steps (0.22 %) — and the emulated machine is unchanged *byte for byte*.

## 1. What changed

| file | change |
|---|---|
| `tools/nnk_dma.inc` | `nnk_tick_due` (next `cycles` at which the tick has work) + `nnk_tick_deadline()`, called at the end of every tick; `nnk_io_touch()` / `nnk_io_touch_read()` pull the deadline back to `cycles` for stores/reads that re-time the model; `nnk_tick_report()` (`NNK_TICK_PROBE=1`) |
| `tools/split_dispatch.py` | the dispatch line becomes `if (cycles >= nnk_tick_due) nnk_dma_tick();`; `nnk_io_touch(a)` is spliced into `io_write32/16/8` and `nnk_io_touch_read(a)` into `io_read32`; forward declarations in `nnk_main.c`'s prologue; tick report in `main()`'s tail |

### The deadline (`nnk_tick_deadline`, the whole contract)

`min(`
next VCOUNT scanline edge `= (cycles/2130 + 1) * 2130` (VBlank IRQ, VCOUNT
match, vblank/hblank DMA triggers),
next timer overflow `= base + ((ovf+1)*period) << shift` for each enabled
IRQ timer,
the pending Phase-4Q reply / Phase-4X handshake / Phase-4R producer word /
Phase-4S completion cycle,
`cycles` while a mode-5 card channel has the CPU halted or a word waiting, or
a shim transfer (mode 0/7) is still armed,
`cycles` for the states that cannot be scheduled exactly: a delivery blocked
on a full 16-word recv FIFO (retry next dispatch), ARM7 send back-pressure,
and the opt-in per-step probes (`NNK_PROBES=1`, `NNK_IPC_PROBE=1`).
`)`

Every deadline is the exact `cycles` value at which the tick's own `>=` test
first becomes true, so the tick runs on the same dispatch the per-step version
would have run it on.  `nnk_io_touch()` covers the registers the tick depends
on — `DMAxSAD/DAD/CNT` (0x040000B0..0xDF), `TMxCNT_L/H` (0x04000100..0F),
`DISPSTAT`, `AUXSPICNT/ROMCTRL/card CMD` (0x040001A0..AF), `IPCSYNC/FIFOCNT/
FIFO` (0x04000180..8B), `IME/IE/IF`, `KEYINPUT/KEYCNT` — and
`nnk_io_touch_read()` the FIFO drains (0x04100000/04/10) that can free a
blocked reply slot.  GX-FIFO/palette/VRAM/OAM stores do **not** disturb the
schedule (they are the overwhelming majority of IO writes).

## 2. Speed (this machine, `/usr/bin/time`, headless, same binary tree)

| steps | master `9f66a76` | this branch | speedup |
|---|---|---|---|
| 200 M | **18.24 / 18.56 s** | **9.08 / 9.08 s** | **2.03x** |
| 600 M | **53.38 s** | **24.92 / 24.93 s** | **2.14x** |

≈ 22 M guest steps/s (was ≈ 10.8 M).  `NNK_TICK_PROBE=1` on the 200 M run:
`tick: 449848 ticks for 202754082 cycles (0.2% of steps)`.

## 3. Nothing changed — measured, not asserted

| gate | master `9f66a76` | this branch |
|---|---|---|
| 200 M `done` line | steps=200000001 traced=200000000 distinct_pcs=19460 hle_hits=7535 halt_pc=0x02007138 | **identical** |
| 200 M frames / unhandled io writes | 363 / 162,973 | **363 / 162,973** |
| 600 M `done` line | traced=600000000 distinct_pcs=19460 hle_hits=8249 halt_pc=0x02056B1C | **identical** |
| 600 M frames / io writes | 1077 / 277,419 | **1077 / 277,419** |
| 32 M halt-state registers | r0..r14, nzcv | **identical** |
| 32 M SMC counters (probes/stale/redecoded/on_demand) | 23545032/153075/153083/8 | **identical** |
| savestate, `NNK_SAVESTATE_STRICT=1` 5 M → 6 M | — | fnv `0x39951F98` crc `0xC6A28280` **both** runs |
| savestate, STRICT 30 M → 32 M | — | fnv `0xC64564A1` crc `0x147E9C9C` **both** runs |
| `NNK_PROBES=1` (30 M) probe lines | 70 lines | 70 lines, **`diff` empty** |

**Whole-state proof.** A savestate saved at 5 M and at 30 M by both builds was
compared region by region (`tools/nnks_inspect.py`): `smc`, `card`, `input`,
`movie`, `display`, `gxblob` (5.27 MB of rendered display), `ram` (25 MB),
`itcm`, `dtcm` have **identical CRCs**.  Only region `io` differs, and it
differs *only* because `nnk_tick_due` + `nnk_tick_n` (~16 B) were inserted at
field offset 2756: `cmp` shows bytes `[0,2756)` identical and the remaining
82,424 B identical modulo that 16-byte insertion.  So the CPU, the whole
guest RAM/ITCM/DTCM, the card model, the IPC/FIFO state, the timers and the
rendered picture are byte-for-byte what master produces.

Prompt/town gate (cold boot, `NNK_GX_SHOT_EVERY=25`):

* `work/repro_5bi/start_cutscene.nnk` → frame 6400 bottom **dark band 219 px**
  (the 5BI/5BJ/5BM number).
* that script + `Right` 6400..6406 + `A` 6420..6426 (`NNK_INPUT_TRACE` shows both
  applied and the guest reading `KEYINPUT = 0x03EF`, then `0x03FE`) → top screen
  first non-black **frame 6525 mean=26.1**, **frame 6575 mean=153.5** (town map),
  bottom screen first content **frame 7475 mean=29.5** — the 5BM timeline.

## 4. What is left in the per-step loop (not required for this card)

With the tick scheduled, the remaining fixed per-step work is the two
`vcount()` calls the image prelude still makes per dispatch
(`nnk_display_tick()` and the Phase-5F VBlank producer — a 64-bit division
each), `trace_pc` (+ its `p5u`/checkpoint hooks), `nnk_smc_try` /
`nnk_smc_carry_override`, `nnk_regwatch`, `irq_tick`, `arm7_evt_tick`,
`nnk_input_tick`, `nnks_tick`.  Folding the two VCOUNT checks onto the same
deadline (they only need the scanline edges, which the tick now computes) is
the next measured win available; it was left alone because the card's target
(≥ 1.6x) is met at 2.0x and the display path is the exactness-critical one.

## 5. Gates

| gate | result |
|---|---|
| `cd nnktool && cargo test --release` | **65 passed, 0 failed** |
| `sh tools/verify_phase2a.sh` | **rc=0**, melonDS prefix **1000 / 1000** (all three comparisons) |
| savestate round trips (STRICT) | 5 M→6 M and 30 M→32 M **bit-equal** |
| prelude audit (`tools/build_split.sh`) | **65/65 required features present, AUDIT OK** (63 pre-existing + the two new Phase-5BN rows `5bn_tick_scheduling` / `5bn_tick_io_hook`) |
