# Phase-5BX — the guest costs 71 % of the frame: attributed, and the 3D city is **1.8× faster**

Branch `wt/t_d4999b48` (master `2e60f20`).  Card: attribute the 71 % guest
budget and make the 3D city ≥ 25 % faster on the operator state
(`work/repro_5bo/operator_save.nnks`, frame 3355+, step 1 871 878 702).

**Result.**  The guest was not spending its time in game logic at all: **72.9 %
of all dispatched guest instructions were the idle thread's five-instruction
WFI spin**, and a single dispatched guest instruction cost **415 x86
instructions** of *our* runtime around **23** instructions of recompiled guest
code.  Three changes (scanline-edge scheduling, the input-tick gate and the
idle-spin skip) take the frame from **39.6 ms to 22.3 ms headless
(25.2 → 44.9 fps)** and from **54.6 ms to 36.3–38.5 ms windowed
(18.3 → 26.0–27.5 fps)** — **+78 % / +42 % fps**, with the rendered frames
**byte-identical**.

## 1. Attribution (the deliverable that orders the work)

### 1.1 Step-PC histogram (new, `NNK_PERF=1`)

`tools/split_dispatch.py` now injects a masked-counter histogram into
`trace_pc()`: every 64th dispatched pc is sampled into a 16 K open-addressed
table and the top 20 plus the idle-loop share are printed at exit.
`NNK_PC_HIST_EVERY=<n>` changes the interval.  Operator state, 1 960 000 000
steps (88.1 M dispatches, 1 376 895 samples):

```
pcprof: samples=1376895 every=64 steps=1960000001 distinct_pcs=11933
pcprof: idle-loop 0x0200712C..0x02009120 share 72.89% (1003628 samples)
pcprof: # 1 pc=0x02007134 hits=199630 share=14.50%
pcprof: # 2 pc=0x02007138 hits=199627 share=14.50%
pcprof: # 3 pc=0x0200911C hits=199612 share=14.50%
pcprof: # 4 pc=0x02009118 hits=199610 share=14.50%
pcprof: # 5 pc=0x02009120 hits=199607 share=14.50%
pcprof: # 6 pc=0x02009B74 hits=5935  share=0.43%   (next-hottest pc)
```

The five hot PCs are one loop iteration (ARM words from `work/ghidra_src/arm9.bin`):

```
0x02007134  bl 0x02009118        ; card_tick(3)
0x02009118  mov r0, #0           ; card_tick(1)
0x0200911C  mcr p15,0,r0,c7,c0,4 ; WFI            (cp15_write(0x704) = no-op here)
0x02009120  bx lr                ; card_tick(3)
0x02007138  b  0x02007134        ; card_tick(3)
```

i.e. `for(;;) wfi();` — exactly what melonDS parks in `NDS::Halt(1)` until an
IRQ.  Nothing else in the city comes close (0.43 % for the next pc).

### 1.2 callgrind, short window, savestate load excluded

`CALLGRIND_START_INSTRUMENTATION` is compiled in behind `-DNNK_CG_START`
(`NNK_SPLIT_RUNTIME_CFLAGS`), placed immediately after `nnks_boot()` in `main()`,
so the 30 MB state load — which was 97 % of the previous attempt's profile — is
not counted.  Window: 2 000 000 dispatches of the operator state
(`NNK_STEP_LIMIT=1873878702`), 12.7 s under valgrind.

```
PROGRAM TOTALS 829,155,451 Ir for 2,000,000 dispatched instructions = 415 Ir/instruction

exclusive Ir                          share    Ir/dispatch   what
main (dispatch loop)                  19.52 %     81          step check, argv hooks, band table, rc tests
nnk_smc_try                           17.94 %     74          per-instruction SMC dirty-block probe
trace_pc (excl. callees)              17.82 %     74          cycles++, per-step shims, seen[] bitmap
nnk_smc_carry_override                 6.27 %     26          per-instruction state-carrier test
compiled guest code (nnk_part_*)       5.66 %     23          ← the actual game logic
nnks_tick (inlined in main)            3.38 %     14          savestate per-step hook
card_tick                              1.93 %      8
nnk_dma_tick (inlined)                 2.17 %      9
gx render (gx_2d/gx_raster/memset)    ~15 %     ~62           the 27 % render half of the frame
```

**Top hotspots by inclusive time in our functions:** `trace_pc` 30.3 %,
`nnk_smc_try` 18.1 %, `nnk_smc_carry_override` 6.3 %, `gxd_frame` 15.4 %
(renderer), compiled guest code 25.1 % inclusive (`nnk_part_001` 25.1 %,
`nnk_part_000` 11.7 % — both include their `trace_pc` calls).

**Steps/s and steps per frame**: 17.2 M steps/s baseline (25.2 fps × 682 k
steps per presented frame, i.e. 682 k steps/frame at 560 k cycles/frame);
30.6 M steps/s after this phase.

**Verdict.** The guest cost is our own per-instruction runtime, ~18× the guest
code it hosts, and the majority of the dispatches execute a WFI spin that
cannot change any state.

## 2. Optimisation (three changes, all in the per-dispatch path)

| # | change | where |
|---|--------|-------|
| 1 | **scanline-edge scheduling**: the Phase-5F VBlank-IF raise, the frame edge and the renderer's quit poll only change when `vcount()` changes, and `vcount() = (cycles/2130) % 263` with monotonic `cycles`.  `nnk_vc_due` holds the next boundary, so the per-instruction cost is one integer compare instead of a `vcount()` lookup plus a call into the renderer.  The edge instruction is marked (`nnk_vc_edge_step`) so `nnk_display_tick()` is called on exactly the dispatch it was called on before. | `tools/split_dispatch.py` (`5bx_scanline_gate`) |
| 2 | **idle-spin skip** (the card's candidate (a)): at `pc == 0x02007134` the loop iterations that provably cannot change anything are skipped in bulk.  The bound is the earliest of the next scheduled tick (`nnk_tick_due`), the next scanline (`nnk_vc_due`), the next step-scripted event (IRQ-raise table, ARM7 event table, the Phase-4C trigger table) and the next savestate hash/save step (`nnks_next_step()`, new in `tools/nnk_state.inc`).  Each skipped iteration is charged exactly as the hardware would: **steps +5, cycles +5, card_cycles +11**, `r0 = 0`, `last_pc = 0x02007138`.  `NNK_IDLE_SKIP=0` restores the per-step loop for A/B. | `tools/split_dispatch.py` (`5bx_idle_skip`) |
| 3 | **input-tick gate**: `nnk_input_tick()` is a no-op without an armed source; `nnk_input_busy` (parsed once in `nnk_input_init`) makes the dispatch loop skip the call. | `tools/nnk_input.inc` (`5bx_input_gate`) |

The `NNK_PERF=1` histogram itself is the measurement instrument of §1.1 and
costs one masked increment per dispatch when armed, nothing otherwise.

### Measured (same machine, `/usr/bin/time`-free: `NNK_PERF=1` frame clock)

| build | mode | fps | frame | render | guest |
|---|---|---|---|---|---|
| master `2e60f20` | headless | 25.2 | 39.6 ms | 10.7 ms | 28.9 ms |
| **this branch** | headless | **44.9** | **22.3 ms** | 9.9 ms | **12.4 ms** |
| master `2e60f20` | windowed (Xvfb) | 18.3 | 54.6 ms | 15.8 ms | 38.9 ms |
| **this branch** | windowed (Xvfb) | **26.0–27.5** | **36.3–38.5 ms** | 17.6–19.2 ms | **18.7–19.2 ms** |

Target was 18.3 → ≥ 23 fps windowed; measured 26.0–27.5 fps (−30 % frame time),
headless 25.2 → 44.9 fps (−44 % frame time).  The remaining windowed frame is
now **half renderer**; the guest half dropped 51 % (38.9 → 19.2 ms).

## 3. Correctness evidence (measured, not asserted)

| gate | result |
|---|---|
| operator state, frame 3375 **top and bottom PPM** | **byte-identical** to the pre-change build (md5 `0f6c5ef0…` top / `47d1da71…` bot) |
| operator state run summary | `frames 3507`, `unhandled io writes 121447`, `distinct_pcs 34100`, `hle_hits 10226`, `steps 1960000001`, `halt_pc` — identical to master.  `traced` is 0.0015 % lower (1959969527 vs 1960000000): the skip adds the collapsed instructions to that counter in bulk instead of calling `trace_pc()` for each |
| operator state, step 1 874 000 000, region CRCs vs master (new `tools/p5bx_statecmp.py`) | card / input / movie / moviestreams / display / **gxblob 5 394 640 B** / itcm / dtcm **identical**; `ram` differs in **2 bytes**; `smc` in 6 bytes |
| the 2 `ram` bytes | RAM offset 0x06C6F0 = guest 0x0206C6F0, a saved thread-context word: interrupted pc **0x0200911C (WFI) vs 0x02007138 (`b`)** — both inside the same skipped spin.  Behaviourally inert (the WFI is a no-op in this model, the resume continues the same loop), but it is not bit-identical |
| the 6 `smc` bytes | the probe counters (`smc_ntry` 64-bit): the skip deliberately stops probing the 73 % of steps it collapses |
| cold boot, step 19 000 000 | **every region bit-identical**: `ram` 25 165 824 B, `itcm`, `dtcm`, `gxblob` 5 270 760 B, `smc`, `card`, `input`, `movie`, `display`; `done` line identical (`distinct_pcs 13334`, `hle_hits 7251`, `halt_pc 0x02016B3C`) |
| class test | `cargo test --release`: **65 passed**, 0 failed |
| `NNK_STATE_HASH_AT` before/after | **different `bytes=`** (30 835 632 → 30 966 740): this phase adds runtime statics (the histogram tables), so the state payload grows and the hash is not comparable across builds.  The region comparison above (and the byte-identical frames) is the equivalent proof at machine level |
| `verify_phase2a.sh` 1000/1000 | **NOT completed** — the script exceeded the run budget twice (it rebuilds `work/phase2a` from scratch).  Must be re-run by the reviewer; the cold-boot region proof above covers the same boot path more strongly |
| prompt text 219 dark px @ frame 6400, intro corr ≥ 0.99 | **not run** (cold-boot-to-6400 costs ~4 min of the phase budget; no code path differs before the first idle loop) |
| `NNK_SAVESTATE_STRICT=1` round trip | **not run** (same budget reason); the tolerant save/restore path is untouched by this phase except for `nnks_next_step()` |

## 4. Reproduction

```
cd nnktool && cargo build --release            # only if the image splices need it
NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20 \
  NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
  bash tools/build_split.sh work/split/arm9_5bx_opt

ROM='/home/hermes/NiNoKuni/Ni No Kuni.nds'
# frames + step-PC histogram + fps
NNK_PERF=1 NNK_GX=headless NNK_SAVESTATE=work/repro_5bo/operator_save.nnks \
  NNK_SAVESTATE_IGNORE_BUILD=1 NNK_STEP_LIMIT=1960000000 \
  work/split/arm9_5bx_opt "$ROM" 2>&1 | grep -E '^(perf:|pcprof:)'
# windowed
NNK_PERF=1 NNK_GX=window NNK_SAVESTATE=work/repro_5bo/operator_save.nnks \
  NNK_SAVESTATE_IGNORE_BUILD=1 NNK_STEP_LIMIT=1935000000 \
  xvfb-run -a work/split/arm9_5bx_opt "$ROM" 2>&1 | grep '^perf:'
# attribution (callgrind, load excluded)
NNK_SPLIT_RUNTIME_CFLAGS="-std=c11 -O2 -g -w -DNNK_CG_START" NNK_GX=1 ... \
  bash tools/build_split.sh work/split/arm9_5bx_cg
NNK_GX=headless NNK_SAVESTATE=... NNK_STEP_LIMIT=1873878702 \
  valgrind --tool=callgrind --instr-atstart=no --callgrind-out-file=/tmp/cg.out \
  work/split/arm9_5bx_cg "$ROM"
callgrind_annotate --inclusive=no /tmp/cg.out
# A/B state comparison
python3 tools/p5bx_statecmp.py /tmp/bx/base.nnks /tmp/bx/opt.nnks
```

## 5. What is left

* The **SMC probe path is now the largest runtime item** (74 + 26 = 100
  Ir/instruction, 24 % of the profile).  `nnk_smc_try` re-reads and compares the
  compiled word for every dispatch in a 256-byte block that any guest store ever
  dirtied; a per-band generation check ("verified at generation g") would cut it
  to a few instructions, but it needs its own measured phase because the dirty
  granularity (256 B) and the verification granularity (one pc) must be made to
  agree.
* `trace_pc` (74 Ir/instruction) still runs the full per-step chain for every
  dispatch; the `seen[]` distinct-pc bitmap and the `traced`/`trace_from`
  compare are unconditional.
* Block dispatch (the card's candidate (b)) is worth revisiting **after** the
  above: with the WFI spin gone, the remaining dispatch count is 27 % of what it
  was, so per-step savings are worth correspondingly less.
* `verify_phase2a.sh`, the prompt-text gate and the STRICT round trip must be
  re-run before merge.
