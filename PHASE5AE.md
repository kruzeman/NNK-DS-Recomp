# Phase-5AE — throughput: the per-dispatch bookkeeping was 58% of the runtime

Follows Phase-5AC (`9c94bf0`, merged as `2011fa7`).  The card asked for faster
runs so every future phase iterates faster: a movie-band run (2.36e9 steps) was
10–25 min of wall clock.  Result: **2.8x on the default (no frame dumps) path**,
measured at the same limit on the same machine, with guest-visible behaviour
unchanged (identical `distinct_pcs` / `halt_pc` / `steps` / SMC accounting, a
1000/1000 strict PC prefix, and byte-identical PPM frames).

## 1. How the time was measured (perf is not available here)

`/proc/sys/kernel/perf_event_paranoid = 4`, so `perf record` is refused
("Disallow kernel profiling") for both `cycles` and `cpu-clock`.  Instead:
**instrumented TSC region counters** in a measurement-only build, produced by
`tools/p5ae_prof_splice.py` (it rewrites the generated `nnk_main.c`, so the
shipped build is untouched), plus differential wall-clock runs.  A mark
attributes the interval that *ends* at it; every compiled instruction leaves the
dispatch block through `continue`, so "instruction body" and "loop head" share
one region.

Fixed workload for every number below: `/tmp/nnk_movie`, `NNK_GX=headless`,
`NNK_SMC_IMAGE="0x02000000:arm9.bin,0x020C0000:ovl_p4f4.bin"`,
`NNK_ROM_PATH` = the real ROM, no `NNK_GX_SHOT`.  Host: Xeon E5-2686 v4
(2.30 GHz), **load average 28 on 24 cores** for the whole phase (other agents'
DQ8 runs) — absolute numbers are depressed by contention; the before/after ratio
is what matters.

### 1.1 The profile, before (50M steps, mean 1305.8 host cycles per dispatched instruction)

| region | share | cyc/dispatch | what it is |
|---|---:|---:|---|
| dma + input tick | **58.4%** | 762.2 | `nnk_dma_tick()` + `nnk_input_tick()` |
| band lookup + call + `trace_pc` entry | 17.1% | 223.8 | band table, indirect call, switch |
| instruction body + loop head | 10.7% | 140.1 | stores/`card_tick`, rc tests, step check, IRQ ticks |
| `nnk_smc_try()` | 5.9% | 77.5 | dirty-band probe + re-decode |
| `nnk_display_tick()` | 3.2% | 41.7 | `vcount()` + gx present |
| dump-probe chain | 2.6% | 34.5 | checkpoint/debug/itcm/dc/pred |
| `p5u_hook()` | 1.9% | 25.3 | measurement probe |
| `nnk_smc_exec_any()` | 0.01% | 0.1 | decode-on-demand (122k here) |

The 58% was **two `getenv()` calls per guest instruction**
(`NNK_INPUT_FILE`, `NNK_INPUT_RECORD`): `getenv` is a linear `strcmp` walk over
the whole environment, ~300 cycles per miss with this run's ~40 variables.  Add
**three 64-bit divisions per instruction**: `vcount()` is
`(cycles / 2130) % 263` and is called from the VBlank-IF producer, the DMA tick,
`trace_pc` and the display tick — 40 cycles each — plus up to four more
divisions in the timer model (`ticks / period` per enabled timer).  So the
notice-screen loop spent more than half its time computing clock values nobody
read, not executing guest code.

### 1.2 After the fixes in this phase (50M steps, mean 699.7 cyc/dispatch)

| region | share | cyc/dispatch |
|---|---:|---:|
| band lookup + call + `trace_pc` entry | 31.6% | 220.8 |
| instruction body + loop head | 21.5% | 150.1 |
| dma + input tick | **21.2%** | 148.3 |
| `nnk_smc_try()` | 10.8% | 75.4 |
| `nnk_display_tick()` | 6.5% | 45.5 |
| dump-probe chain | 4.6% | 32.1 |
| `p5u_hook()` | 3.8% | 26.5 |

(These three tables include ~80 cycles/step of TSC mark overhead, so the profile
is slightly pessimistic for both builds in the same way.)

## 2. What was changed (all semantics-preserving)

1. **`tools/nnk_input.inc` — the two per-step `getenv()` calls are cached**
   (`static const char *p; static int init;`).  The environment cannot change
   inside a run.  Biggest single win.
2. **`vcount()` cached** (`nnktool/src/recompile.rs` + the frozen-image splice):
   the scanline for the 2130-cycle window containing `cycles` is cached;
   `cycles` is monotonic except for a card-stall jump, which the window check
   catches and recomputes.  Value is bit-identical (`(cycles/2130)%263`).
3. **`p4u_timer_tick()` — no division unless an overflow is possible**
   (`tools/nnk_dma.inc`): `ovf > p4u_tm_ovf[t]` is exactly
   `ticks >= (ovf+1)*period`, so the hot path is a multiply + compare.
4. **`nnk_display_tick()` returns early when the scanline has not changed**
   (prelude + splice): the only state is the `<192 -> >=192` frame edge, so
   presentation is unchanged; the per-step tail call into libgxrender
   (`gxd_should_quit`) disappears.  Window-close latency is now <2130
   instructions.
5. **`trace_pc()`'s debug/probe chain is gated** (splice only, see §4): the four
   env-gated dumps are skipped when none of their parsed addresses is
   configured (`ckpt_pc || dbg_pc || dc_npc || pred_npc`), and `p5u_hook()` is
   skipped unless `NNK_P5U` turned it on (`nnk_p5u_on`, set by `p5u_init`).
   `itcm_dump_once()` keeps an arms-length test because its first-dispatch line
   is part of every captured run.
6. **`nnk_smc_try()` probe fast path** (`tools/nnk_smc.inc`): the 32 KiB dirty
   *band* test became a **256-byte dirty block** test (a pure filter — the
   decision is still `w == iw`), and the per-probe walk of the generation-0
   segment table became a **per-band pre-resolved segment** (built once in
   `smc_arm()`; partially covered bands keep the slow path).

Measured effect of (6) alone: probes 146.6M → 141.4M over 150M steps — i.e. the
guest's stores really do land in the same 256-byte blocks as the running code,
so **narrowing the dirty block further is not worth it** (a 32-byte variant was
not pursued beyond this measurement).  The probe is still ~78 cycles/step
because it runs on ~94% of dispatches on this loop.

## 3. Numbers (same machine, same limit, medians of runs without frame dumps)

150M steps:

| build | runs (wall) | steps/s |
|---|---|---:|
| before (`2011fa7`, `work/split/base`) | 81.4 s, 85.2 s | 1.80 M/s |
| after (`work/split/final`, sources as committed) | 28.7 s, 30.6 s | **5.07 M/s** |

⇒ **2.82x** at 150M steps (median 83.3 s → 29.6 s).  Earlier runs of the
intermediate builds give 29.3 / 29.8 / 30.3 / 31.7 s — all inside the same band,
i.e. the last two changes (probe gate, display-tick gate) are below the ±4 %
noise of this loaded host.

Wall time to the movie band, `NNK_STEP_LIMIT=2360000000`, canonical movie
sources (`NNK_MOVIE_YUV`/`NNK_MOVIE_YUV2` paths):

**449 s (7 min 29 s) for 2,360,000,001 steps = 5.26 M steps/s.**
That run ended on the step budget with `distinct_pcs=22884`, `halt_pc=0x0207CEA4`,
`smc: probes=2347946702 stale=130972 redecoded=80421260 state_fallbacks=1
on_demand=80290288` — i.e. the ITCM decode-on-demand path (80.3M interpreted
instructions) is now the movie band's largest remaining fixed cost, and
`nnk_smc_try` probes ~100% of dispatches there.

## 4. Correctness proof for the optimisations

Same script, same limit, `work/split/base` (before) vs `work/split/opt_bin`
(after):

* 150M steps: both end `done steps=150000001 traced=150000000
  distinct_pcs=19470 hle_hits=7445 halt_pc=0x02009120 halt=step budget
  exhausted`, with identical `halt state:` register/flags line and identical
  `smc: … stale=130972 redecoded=253470 halts=0 state_fallbacks=1
  on_demand=122498` (only the `probes=` counter moved, by construction).
* 55M steps: `distinct_pcs=19454`, `halt_pc=0x02009118`, `hle_hits=7276`,
  `redecoded=149592`, `on_demand=18620` — identical on both.
* **Frame dumps byte-for-byte identical**: 150M-step runs of both binaries with
  `NNK_GX_SHOT_EVERY=25` produced 20 PPM files; `md5sum` of every file matches
  (`diff base2.md5 opt2.md5` empty).  The sampled frames are *not* all the same
  image (three distinct hashes for `f.00025`, `f.00050`, `f.00100`, `f.00225`),
  so the comparison is not vacuous.  A second pair at 55M steps (`EVERY=10`,
  20 files) is also identical.
* **`tools/verify_phase2a.sh` (SKIP_MELONDS=1) runs green end to end**, including
  the strict prefix — `1000 / 1000 melonDS PCs (100.0%)`, "melonDS trace fully
  reproduced" — against a mono image regenerated from the edited
  `nnktool/src/recompile.rs` (so the canonical prelude change compiles and does
  not move the PC sequence).
* **`cargo test`: 42 passed** — suite 41 -> 42 tests; the new class-41 test
  `phase_5ae_per_step_caches_are_bit_exact`, which proves the `vcount()` cache
  equals the division form over a unit walk, every scanline/frame boundary,
  forward jumps of many frames, a backward jump, and the 64-bit edges (where the
  window wraps and simply recomputes, matching C unsigned semantics), and
  asserts every source-level guard above is still in place.

## 5. Build path and what a regenerated image still needs

`tools/build_split.sh` now splices the frozen image twice:
`p5x_runtime_splice.py` (Phase-5X read shadow, mandatory) and then
`p5ae_runtime_splice.py` (this phase).  The splice is applied only when the
image lacks the `nnk_probe_mask` marker, so it is a no-op on a regenerated
image that already carries the canonical text.

* `nnktool/src/recompile.rs` carries the **canonical** `vcount()` cache and the
  `nnk_display_tick()` scanline gate (verified by the mono `verify_phase2a`
  build).
* The **`trace_pc()` probe gate lives only in the splice**: `dc_probe` /
  `pred_probe` / `p5u_hook` are themselves injected by `tools/p5r_pred_probe.py`
  / `tools/p5q_*` / `tools/p5u_probe_splice.py`, so the gate can only be applied
  after those insertions.  A future full `recompile-all` + re-splice therefore
  still needs `p5ae_runtime_splice.py` (or those splice tools must learn the
  gate).  This is honest debt, recorded here rather than papered over.

## 6. What cannot be sped up (measured, not guessed)

* **The 256-byte dirty probe granularity** — see §2(6): 146.6M → 141.4M probes;
  the remaining ~94% are genuine same-block events.
* **The guest-instruction path itself** is now the largest single cost
  (`band lookup + call + trace_pc entry` 220 cyc, `body + loop head` 150 cyc).
  It is generated code compiled at `-O0` (`NNK_SPLIT_CFLAGS` default) — that is
  a deliberate build-system choice (fast, cacheable objects for 32 TUs), and
  changing it recompiles every part, so it was left alone here.  It is the
  obvious next lever (a measured `-O1` experiment is the follow-up), together
  with the 80M-instruction ITCM decode-on-demand path in the movie band.
