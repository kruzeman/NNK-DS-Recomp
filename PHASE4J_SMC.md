# Phase-4J — SMC: runtime image-state tracking for self-modifying code

Follows Phase-4I (merged `1deba2c`).  Scope: replace the "union of overlay
images" (always one decrypt pass behind) with the general mechanism — when the
guest stores into a translated code window, the affected cases stop being
served from the compile-time image and are re-decoded from the CURRENT live
RAM, with per-band generation counters.  Then verify per pass against melonDS.

**Outcome: the mechanism works and is measured (44 live words re-decoded and
executed from RAM, 0 unsupported, 0 halts, the previously fatal stub thunk at
`0x0215B34C..0x0215B36C` now executes), but the run still stops at
`0x0215B77C` — that word is still ciphertext in BOTH native RAM and the
compiled image, i.e. the guest has not yet run the decrypt pass that writes it.
Honest stop-report in §5.**

## 1. What "recompile from the current image" means here

A C recompiler cannot invoke the Rust emitter at run time, so the mechanism is
split into the two halves that make it a general SMC path rather than a patch:

| half | implementation |
|---|---|
| invalidation | generation-0 image snapshot + per-32 KiB-band **generation counters**, bumped by every guest store (`st8/st16/st32`, which DMA and SWP also route through) — `smc_note()` in `tools/nnk_smc.inc` |
| re-compilation | a runtime **decoder/executor** for the live word (`smc_exec_arm`, `smc_exec_thumb`) that mirrors the emitter's semantics exactly (same `sh_op`/`sh_op_reg` barrel shifter, same 64-bit carry/overflow computation, same `card_tick` cost table, same `halt_reason` discipline) |

The dispatch loop calls `nnk_smc_try(&pc)` **before** the compiled band switch:

```
if (pc in the main-RAM code window && band generation != 0) {
    iw = compile-time word at pc          (the generation every case was built from)
    w  = live word at pc
    if (w != iw) -> compiled case is INVALID: trace_pc, decode+execute w, continue;
}
```

Words that still match keep their compiled bodies, so the compiled fast path
is untouched for every non-self-modifying page.

Two details this needed, both measured:

* **The generation-0 image is not the file the runtime loads.**  The ARM9 binary
  is stored compressed; the guest decompresses it into main RAM at run time
  (`0x02027BA8` = `018080B9` in `arm9_stored_padded.bin`, `E12FFF1E` in the
  recompiler's `arm9.bin`).  Comparing against the loaded file marked ~196M
  instructions "stale" and derailed the run into data.  The compiled
  generation is therefore taken from the **recompiler inputs**:
  `NNK_SMC_IMAGE="0x02000000:<arm9.bin>,0x020C0000:<ovl image>"`.
* **State of a re-decoded PC.**  `tools/split_dispatch.py` derives it from the
  image it is splitting: a case whose emitted fall-through is `addr+2` is a
  16-bit Thumb instruction (exact, 5132 cases / 860 runs in this image →
  `nnk_smc_map.h`); everything else is treated as ARM with a Thumb fallback
  when the ARM decode is undefined.

## 2. Files

* `tools/nnk_smc.inc` (new) — the runtime half: segments, generation counters,
  ARM decoder/executor (full data-processing / multiply / extra + signed loads /
  LDM-STM with user-bank form / B, BL, BLX / SWP / MRS, MSR / CLZ / CP15
  MCR-MRC / SWI HLE), Thumb-1 subset, and `nnk_smc_try`.
* `nnktool/src/recompile.rs` — emits the hooks: `smc_note()` in
  `st32/st16/st8`, `#include "nnk_smc.inc"`, `smc_arm()` after the guest image
  is loaded, `smc_stats()` after `run()`.
* `tools/split_dispatch.py` — copies the fragment next to the generated TU,
  emits `nnk_smc_map.h` (Thumb map) and the dispatch hook.
* `tools/build_split.sh` — the SMC fragment and map are part of the runtime
  cache key.
* `tools/add_smc_hooks.py` (new) — splices exactly the emitter's hooks into an
  image C generated **before** the recompiler change, so this phase does not
  need a 37 MB regeneration just to recompile the same instruction set.

Instrumentation: `NNK_SMC=0` disables (compiled dispatch = Phase-4I behaviour),
`NNK_SMC_TRACE=n` logs the first n re-decodes with `step`, compile-time word,
live word, band generation and next pc; `smc_stats()` prints probe/stale/
re-decoded/halt counts and the per-band generations at exit.

## 3. Measured behaviour

Run (stored guest image, live gx bridge):

```sh
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

```
smc: armed, 2 generation-0 segments, 768 bands
smc: redecoded step=13142563 pc=0x0215B34C img=0x9E635DFE word=0xE18FC00F state=arm gen=23128 -> 0x0215B350
smc: redecoded step=13142564 pc=0x0215B350 img=0x7F5E6DB2 word=0xE01CC00C state=arm gen=23128 -> 0x0215B354
...
smc: redecoded step=13142571 pc=0x0215B36C img=0xB4142C50 word=0xE8BD8000 state=arm gen=23128 -> 0x0215B6E0
smc: redecoded step=13142572 pc=0x0215B6E0 img=0x96C19DFE word=0xE92D000F state=arm gen=23128 -> 0x0215B6E4
smc: probes=9770011 stale=44 redecoded=44 halts=0 state_fallbacks=0
done steps=13142612 ... halt_pc=0x0215B77C halt=dispatch to untranslated pc
```

* The stub thunk the guest rebuilt at run time (`0x0215B34C..0x0215B36C`, the
  exact plaintext melonDS has: `E18FC00F E01CC00C 03A0C000 128CC01C
  E59CC014 E24CCC21 E92D1000 E18FC00F E8BD8000`) is **executed from live RAM**.
  With `NNK_SMC=0` the same image runs into `0x0215B350`, which the compiled
  generation decodes as an unimplemented SWI (floor: step 13,142,564) — so SMC
  moved the frontier past it with **no per-pass image supplied**.
* All 44 stale words decoded and executed: `halts=0`, `state_fallbacks=0`,
  i.e. no silent success and no ARM/Thumb guessing in this run.
* band generations are unbounded (`gen=23128..23130` for the arena band at
  this point) — every store into the window advances them; there is no
  two-generation union anywhere.

## 4. Ground truth per pass (melonDS MemWatch/regprobe capture)

`build-ptrace/melonDS` with `MELONDS_REGPROBE_PCS=0x0215B468,0x0215B408`,
`MELONDS_REGPROBE_MEM=0x02159FE0:0x21E0`, `EXIT_HIT=11` produced **12 arena
dumps** (one before each of the 11 decryptor entries plus the loader-return
point) — the pass-boundary ground truth (`p4j_arena_h1..h12.bin`, verbatim in
`p4j_arena.txt`).  Pass diffs measured from them:

```
hit 1->2: 85 words (first 0x0215B920)   hit 5->6: 313 words (first 0x0215A7C0)
hit 3->4: 140 words (first 0x0215B528)  hit 6->7: 585 words (first 0x0215A660)
hit 4->5: 47 words (first 0x0215A660)   hits 8..12 repeat the 1..6 cycle
```

Native RAM at the halt matches melonDS's arena state at **decryptor entry 6**
(`h6`/`h12`, 8486 of 8672 bytes identical).  The 48 differing words are
`0x0215B310..0x0215B32C`, where native already holds the melonDS plaintext
(`0FC08FE1 …`) and melonDS at that generation still holds ciphertext — i.e.
native's fifth pass writes a range melonDS writes later, not a wrong value.
Every word where native and melonDS agree is byte-exact, and every plaintext
native produced equals melonDS's.

## 5. Honest stop point and the exact next step

**First word that stops the run: `0x0215B77C` = `0x56BA5EE9`.**

* native live word = `56BA5EE9`, compiled image word = `56BA5EE9`,
  melonDS at the same arena generation = `56BA5EE9` — **all three agree**, so
  SMC correctly did *not* re-decode it (no stale case: the word really is
  still ciphertext).
* The compiled body for ciphertext `56BA5EE9` computes `pc = 0xEB03F7BA`,
  which is untranslated → halt `dispatch to untranslated pc`, `halt_pc =
  0x0215B77C`, step 13,142,612.

Class: **decrypt-schedule divergence**, not image-state tracking.  Native runs
the same first five decrypt taps as melonDS, in the same order
(`NNK_DEBUG_AT_PC=0x0215B468`: `r0 = 0x0215C110, 0x0215B75C, 0x0215A75C,
0x0215AC68, 0x0215B434`; melonDS: identical first five), then reaches
`0x0215B77C` while melonDS requests the remaining five passes first (overlay
PC indices 86203..93396 of `p4h_mel_ovl.txt`).  SMC tracks *bytes*; it cannot
make the guest request a decrypt pass it has not reached yet.

Bounded next step: find the loader decision that ends the descriptor loop early
in native.  The loop is `0x0215B4EC..0x0215B51C`, reading the ids at
`0x0215B410` (`0x13`, `0x14`, then zeros) and calling `bl 0x0215B408` per
descriptor; melonDS gets ten taps out of it, native five.  The comparison point
is the loop tail `0x0215B51C` (registers + the id word), which the
regprobe/memwatch capture method in §4 already supports.

Frames: none rendered with content (DISPCNT still 0 — the run never reaches
display init), so there is no screenshot claim in this report.

## 6. Verification

* `tools/verify_phase2a.sh` with `SKIP_MELONDS=1`: PASS (log `p4j_verify.log`).
* No ROM bytes committed.  Committed: `tools/nnk_smc.inc`,
  `tools/add_smc_hooks.py`, the `recompile.rs` / `split_dispatch.py` /
  `build_split.sh` hooks and this document.  Guest images, generated image C,
  arena dumps and melonDS captures stay under gitignored `work/` and scratch.
