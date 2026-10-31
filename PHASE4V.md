# Phase-4V — the "SDL quit at step 27,371,209" was a UMULL RdLo/RdHi destination swap: a runaway digit store smashed the DTCM IRQ vector

Follows Phase-4U (merged `88684e2`) and the salvaged WIP probe commit `e268a26`
(cherry-picked as `ff8cf76`).  Scope: root-cause the deterministic stop at
native step 27,371,209, fix it with measured semantics, push toward the
display init / picture.

## 0. Headline

* **The Phase-4U "SDL_QUIT" diagnosis was wrong.**  A headless run cannot
  receive SDL_QUIT at all: `pump_events` (the only place `g->quit` is set,
  `gxrender/src/gx_display.c:1336`) runs only when `g->windowed`, and
  `nnk_display_tick()` returns `gxd_should_quit(gxd)`, which stays 0
  headless.  The stop at 27,371,209 is guest-driven and identical in both
  modes.
* **Real chain (measured):** nnktool's decoder passed
  `Kind::Mul { rd: rdhi, rn: rdlo }` for UMULL/SMULL/UMLAL/SMLAL while the
  codegen assigns `r[rd] = _lo; r[rn] = _hi;` — **every static-image long
  multiply had its two destination registers swapped** (and the UMLAL/SMLAL
  accumulate pair with them).  The runtime SMC re-decoder
  (`tools/nnk_smc.inc:329-341`) always had the correct rdlo/rdhi order,
  which is why Phase-4K's live-redecode fix looked healthy.
* **Measured manifestation:** the SDK u64→string divide-by-10 loop at
  `pc 0x0201E814` (`umull r1, r0, r8, r2` + `lsrs r0, r0, #3`) computed
  `low32(x·0xCCCCCCCD)>>3` instead of `high32(x·0xCCCCCCCD)>>3`.  With the
  correct magic in the literal pool (`0x0201EA00 = CD CC CC CC`, confirmed
  byte-identical against the melonDS memory capture) the quotient sequence
  diverged, so the digit store loop (`strb r0,[ip,r9]` at `0x0201E82C`,
  buffer `ip = sp+24 = 0x027E3888`) ran for thousands of iterations up
  through DTCM top, **overwriting the IRQ user vector at `0x027E3FFC`**
  (old `0x01FF8000` = the ITCM stub) at step **25,214,182**.
* **Consequence:** all **1,944,485** following IRQ entries were rejected
  (`user vector 0x429C335E rejected`, one per step: 27,371,209 − 25,426,724
  + 1), the guest idled, and the SMC re-decoder halted on a corrupted code
  word at `pc=0x0201E12C` (`smc: unsupported live instruction … halts=1`),
  whose `step_limit = steps` rewrite made the final line read
  `halt=step budget exhausted` at exactly step 27,371,209.
* **Fixed and verified:** the run passes the barrier — 30M/60M/300M-step
  runs with **0 vector rejections, 0 runaway stores, no SMC halt**,
  B7 starts 4,670 → 9,093, distinct PCs 15,158 → 15,542.

## 1. Measurements that killed the alternatives

* Headless SDL: `pump_events` is inside `if (g->windowed)` (gx_display.c
  ~1552); `gxd_open` never calls `SDL_Init` headless (same file ~1413).
  Kills "SDL/environment quit" as the headless mechanism.
* Vector slot timeline (probe in `tools/nnk_smc.inc`, salvaged WIP): boot
  setup writes `0x027E3FFC` once at step 3,628,039 (pc `0x020008FC`); then
  no writer until the runaway `strb` stream from pc `0x0201E82C` starting
  step 25,213,642, 9 steps per byte, crossing `0x027E3FFC` (old
  `0x01FF8000`) at step 25,214,182 and continuing past DTCM top.
* Register dumps at the loop top (`NNK_DEBUG_AT_PC=0x0201E814`):
  `r2=0xCCCCCCCD`, `r3=0x0000000A` are correct, but the quotient sequence
  `r8: 1 → 0x19999999 → 0x070A3D70 → 0x002D0E56 → 0x0001205B → 0x06666D9B → …`
  diverges.  `0x19999999` is exactly `(1 × 0xCCCCCCCD) low32 >> 3`; the
  correct quotient is `high32 >> 3 = 0`.  Buffer base `r12=0x027E3888` with
  `r13=0x027E3870` (sp+24 ✓).
* Generated-code evidence: the case at `0x0201E814` emitted
  `r[0] = _lo; r[1] = _hi;` for `umull r1, r0, r8, r2` (RdLo=r1, RdHi=r0) —
  swapped; `decode.rs:579` fed `rd: rdhi, rn: rdlo` into a codegen whose
  convention is `rd`=lo/`rn`=hi (recompile.rs:456-458).
* melonDS ground truth (`MELONDS_REGPROBE_PCS=0x0201E814`, full register +
  DTCM dumps): the same loop converts `203 → 20 → 2 → 0` in three
  iterations with the digit index r9 advancing; DTCM top intact; literal
  pool `0x0201EA00 = CD CC CC CC` identical to ours (kills "wrong memory
  image / wrong magic constant").
* Long-multiply exposure ordering (`tools/p4v_mulorder.py` over the 30M
  melonDS PC trace): the first long-multiply executions are at pc
  `0x02002074` (trace line 13,529,048) — so register-level divergence was
  possible from ~step 13.5M on; the first MEASURED control-flow divergence
  is the divide-by-10 loop at step 25,197,004 (dump1: `r8=1`).  447
  long-multiply case pcs exist in the image; 26 execute in the trace
  window.

## 2. Fix

* `nnktool/src/decode.rs` — long-multiply decode now passes
  `Kind::Mul { rd: rdlo, rn: rdhi }` (+comment).  This also corrects the
  UMLAL/SMLAL accumulate order, which the codegen builds from
  `((uint64_t)r[rn] << 32) | r[rd]`.
* `tools/p4v_fix_umull.py` — repairs the ALREADY-GENERATED p4j dispatch
  image in place (its regen inputs, e.g. `seeds_p4j.tsv`, are not all
  preserved): 1,128 long-multiply case bodies rewritten to exactly the
  fixed codegen's text; the diff is strictly those bodies (2,256 lines,
  none outside `r[..] = _lo/_hi` and the accumulate pair).
* `tools/nnk_smc.inc` keeps its correct order; the two backends now agree.

## 3. Results (branch build `out/split/arm9_gx`)

```
30M run  : 0 vector rejects, 0 runaway stores, B7 starts 5343,
           distinct_pcs 15217, halt = step budget (clean)
60M run  : distinct_pcs 15542, 113 frames, card starts 9093,
           last card op step 38,981,422 (op B8), DISPCNT A = 0x0021111B
300M run : distinct_pcs 15542 (frozen), 541 frames, clean step budget
verify_phase2a.sh (SKIP_MELONDS=1): RESULT PASS,
           strict common prefix 1000/1000 melonDS PCs (100.0%)
```

## 4. No picture yet — honest stop-report + the named follow-up barrier

* Frames are still uniform white (22 shot pairs checked, 0 non-uniform;
  `tools/p4v_shotcheck.py`), `DISPCNT A=0x0021111B` unchanged — the melonDS
  display switch (`DISPCNT=0x0021021B`, pc `0x0207CB10`, ts 54,276,936) is
  still ahead.
* **Follow-up barrier (measured, not fixed here):** from ~step 39M the guest
  runs a VCOUNT-poll main loop — `ldrh r0,[ip,#6]; cmp r0,#0; bne` with
  `ip=0x04000000` polled at **VCOUNT (0x04000006)**, dumps read `0x1C` —
  and consumes a full frame per iteration (560,190 cycles/frame =
  CYCLES_PER_SCANLINE 2130 × 263; 300M steps / 541 frames = 554k steps per
  frame ≈ poll-only) with essentially zero frame work: distinct PCs freeze
  at 15,542.
* Prime suspect 1: **the runtime's IF model has no VBlank IRQ source.**
  The only `iflags |=` sites are the boot raise script (`irq_raise_log`,
  ends step 9,097,801), SPI bit19, IPC-reply bit18, card ROMEndTransfer and
  the Phase-4U timer ticks — nothing raises IF bit1 (VBlank).  If the
  guest's per-frame handler runs on the DISPSTAT-bit3 VBlank interrupt it
  never fires, which matches the zero-work frames exactly.  (Fix = raise
  IF bit1 at vc==192 when DISPSTAT bit3 is set, melonDS `NDSTrain`/GPU
  semantics — deliberately NOT shipped unmeasured in this task.)
* Prime suspect 2: **the ARM7 side is a finite script** — `arm7_evt_log`
  ends at step 3,768,189 and the boot IRQ script at 9,097,801; the
  post-load phase (card stream ends step 38,981,422; 9,093 starts) is far
  past the captured melonDS ground truth (2,279 B7s), so any ARM7 waker
  beyond that envelope is missing by construction.
* **First differing word / class (for the chain):** native `r1:RdLo/r0:RdHi`
  at every long multiply vs melonDS's `RdLo:r1/RdHi:r0`; first measurable
  instance at pc `0x0201E814` step 25,197,013 (`r8=0x19999999` where
  melonDS has `0`); class = static-recompiler instruction-semantics bug
  (destination-register swap), fixed.
* **Next task should:** (a) add measured VBlank/HBlank IRQ raising to
  `irq_tick`/`vcount` (compare DISPSTAT writes against an melonDS IO
  capture), (b) extend the melonDS capture (card + IPC + IRQ + IO) past ts
  ~70M to extend the ARM7 shims, then (c) chase the `0x0207CB10` DISPCNT
  switch and the first non-uniform frame.

## 5. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/sdl-quit2   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# fix validation (barrier step 25.2M..27.4M)
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=30000000 \
  NNK_GX=headless \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
SKIP_MELONDS=1 bash tools/verify_phase2a.sh      # RESULT PASS, 1000/1000
python3 tools/p4v_shotcheck.py                   # uniform frames, honest
```

Evidence: `$S/p4v_run26.{log,err}` (pre-fix probes), `$S/p4v_run_dbg.err`
(register dumps at the loop), `$S/p4v_mel_reg.txt` / `$S/p4v_mel_mem.txt`
(melonDS ground truth), `$S/p4v_fix_run30.err`, `$S/p4v_run60.{log,err}`,
`$S/p4v_run300.{log,err}` (post-fix runs), `$S/p4v_verify.log`,
`$S/p4v_image_p4j.orig.c` (pre-fix image for diffing).
