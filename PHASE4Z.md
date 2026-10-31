# Phase-4Z — the module call at 39,001,655 was NOT a thread-wake divergence:
# two on-demand-decoder defects (Thumb STRB/LDRB + T-bit recovery), the named
# melonDS ARM9 pc-trace captured (stop-report)

Follows Phase-4Y (merged `ca15053`).  Scope: model the OS thread wake-ordering
at the IPC mailbox, complete the 111,616 B NPCK module download, pass
`0x02988AA0` along melonDS's path, the picture — or the honest stop-report.
**The named probe was run; it shows the wake order already matches; the barrier
is two concrete decoder defects in the on-demand re-decoder.  Both fixed; the
guest now runs melonDS's exact path through the module dispatch and gets
41 new distinct PCs further, but stops in the *next* overlay block; no picture
(honest).**

## 0. Headline

* **Named probe executed — melonDS ARM9 PC trace over ts 50,490,118..54,349,998**
  (1,658,980 PCs, `$S/p4z_melpc.txt`), captured with a new ts-gated hook
  (`tools/add_arm9trace_ts.py`, `MELONDS_ARM9TRACE_TSLO/_TSHI`, lines carry
  `ts=` in the same unit as `p4q_mel_io.txt`, rebuild `build-ptrace/`).
* **Wake ordering already matches (measurement, not inference).**  After the
  5th `0x6B` reply (ts 50,498,023) melonDS's ARM9 enters the overlay handler
  **645 ts later, at ts 50,498,668** (`0x020D1620..0x020D1642 -> 0x020D19B8`),
  and native enters it **575 steps after its last reply (step 39,001,613)**.
  The caller is *not* parked in either run; melonDS runs the same dispatch and
  returns normally.
* **melonDS path through the dispatch (ground truth):**
  `0x020D19B8 (msg!=1/94 -> default) -> blx r3 @0x020D1A1C -> vtable thunk
  0x020D22BC..0x020D22CE (Thumb: ldr/ldr/cmp/beq/movs/strb/ldr/bx) ->
  0x0204D938 (ARM) -> 0x0204D968..0x0204D978 -> 0x020D1A1E pop ->
  0x020D1646 (handler epilogue) -> 0x020D1B6E...` — 390 dispatcher-region PCs,
  first ts 50,498,668, last ts 51,316,365.  The card driver then wakes at
  **ts 51,788,894** and streams the module (51.79M..54.26M); the display
  switch `0x0207CB10` lands at ts 54,276,936.
* **melonDS never enters `0x02988xxx`:** 0 PCs in `0x02988000..0x029A4000`
  over the whole window, and 0 IO accesses with `pc=0x02988*` in the entire
  6M-event capture.  The old "module call" was native-only.

## 1. Root cause — first differing pc + class (two defects, both measured)

**Class: on-demand re-decoder state/ISA defect at overlay holes** (the
`nnk_smc_exec_any` path, `tools/nnk_smc.inc`), *not* OS wake-ordering.

`nnk_pc_is_thumb()` is generated from compiled cases whose emitter-written
fall-through is `addr+2`.  A PC the recompiler never rooted has no entry and
reads **ARM**, and for the overlay's Thumb bytes the ARM decode *silently
succeeds* — the Thumb fallback never runs (`state_fallbacks=0` in the baseline
run), so `npc = pc+4` garbage walked straight into the unloaded module.

Baseline native vs melonDS at the same dispatch:

```
melonDS  22bc 22be 22c0 22c2 22c8 22ca 22cc 22ce -> 0x0204D938 ... -> 1a1e -> 1646
native   22bc 22c0 22c4 22c8 22cc 22d0 22d4 22d8 ... 2402 2404 -> 1a1e -> 1a22 ... -> 0x02988AA0
```

1. **Thumb `LDRB/STRB` (format 9, `0x7000..0x7FFF`) missing.**
   `if ((hw & 0xF000u) == 0x6000u)` excluded the whole byte half, and the
   offset was always `imm5<<2` (wrong for byte forms).  Measured at
   `0x020D22CA = strb r3,[r0,#15]`: Thumb decode failed -> ARM fallback ->
   `npc=+4` (native skipped `0x020D22CC`) and the following `bx r3`/literal
   words were executed as ARM (`state_fallbacks` 1->2 across the runs).
   **Fix:** mask `0xE000 == 0x6000`, byte forms take `imm5` as a byte offset.
2. **T-bit recovery for unclassified PCs.**
   `blx r3`/`bx` strip bit0 into `pc`; the source register still carries it on
   the next dispatch.  **Fix:** `smc_state_from_regs(pc)` — when the map says
   "not Thumb", the register holding the tagged target decides, else the last
   resolved `smc_t_sticky` decides, else ARM (unchanged).  Consulted only on
   the on-demand path (`nnk_smc_exec_any`), so classified PCs keep their old
   behaviour.  (A first attempt also applied it in `nnk_smc_try`; that
   regressed the run to step 13,146,795 / 5,128 distinct PCs and was reverted
   — `nnk_smc_try` sees *rooted* PCs, where the map is authoritative.)

**Evidence of the fix (60M run p4z_run6):** native now executes melonDS's
byte-for-byte path `0x020D22BC..0x020D22CE -> 0x0204D938 -> 0x0204D978 ->
0x020D1A1E`, which it never did before, and **distinct PCs 15,742 -> 15,783**,
halt step 39,001,656 -> **39,001,760**.

## 2. The next barrier (named, so the chain continues without re-derivation)

* **First differing pc:** native `0x020D1A1E` (`pop {r4,r5,r6,pc}`) returns
  **`0x020D1A21`** instead of melonDS's `0x020D1647` (the `bl` at `0x020D1642`
  link).  Native then walks the literal/data words `0x020D1A20..0x020D1A8E`
  and halts at step **39,001,760, `untranslated pc=0xE51C1008`**
  (halt state: `r13=00000000 r14=E51C1008 r5=02143174 r1=00000005`).
* **Class:** stack/LR integrity across the overlay dispatch — either the
  `push {r4,r5,r6,lr}` at `0x020D19B8` did not store `0x020D1647`, or
  `sp` moved between push and pop (note `r13=0` at halt and `r7=E1A04000`,
  an ARM `mov r0,r0` word — instruction bytes ended up in registers, i.e. some
  instruction in the `0x0204D938` return path still runs with the wrong state
  or a wrong-width memory access).  Two fallbacks fire in this run
  (`state_fallbacks=2`) — trace them with `NNK_SMC_TRACE=<n>`.
* **Named follow-up probe:** a melonDS ARM9 PC trace for ts 50,498,742..50,498,870
  already exists (`$S/p4z_melpc.txt` lines ~2591..2685): the missing native
  span is exactly `0x0204D938..0x0204D978` -> `0x020D1A1E`; dump `sp`/`r14`
  and the 6 stack words at native step ~39,001,700 (`NNK_DEBUG_MEM=0x027E39xx`)
  and diff them against melonDS's `0x020D19B8` frame.

## 3. Honest negatives

* **No picture.**  All frames uniform: `p4z_sh.00010.*` = solid 0 (black),
  `p4z_sh.00070.*` = solid 255 (white) — 14 PPMs, exactly 2 distinct images;
  the display switch (`DISPCNT=0x0021021B` at `0x0207CB10`) is still ahead
  (melonDS ts 54,276,936 ~ native step ~42.8M).  Nothing claimed.
* **Module still not in RAM:** `p4t: last B7 ... starts=9093 last_op=0xB8` —
  the card stream does not resume, because the guest stops at the barrier
  above (melonDS's driver wake is ts 51,788,894, ahead of the display switch).

## 4. Verification (branch build `out/split/arm9_gx`)

```
baseline (Phase-4Y code): halt step 39,001,656 pc 0x02988AA0, distinct_pcs 15,742
state-fix only:           halt step 39,001,649 pc 0x020D2404 (strb still ARM-fallback)
+ format-9 fix (p4z_run6): halt step 39,001,760 pc 0x020D1A8E, distinct_pcs 15,783,
                           melonDS path 0x020D22BC..0x020D22CE -> 0x0204D938 -> 0x020D1A1E taken
verify_phase2a.sh (SKIP_MELONDS=1) on the branch code: ran to completion
    (sections A, A2 "RESULT: PASS", B, D, E, F; set -e would have aborted
    otherwise) — strict common prefix 1000/1000 melonDS PCs (100.0%),
    window-filtered 1000/1000, in-order after resync 1000/1000 ($S/p4z_verify.log)
```

## 5. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/thread-wake   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
python3 tools/add_arm9trace_ts.py                # ts-gated ARM9 pc trace (idempotent)
(cd /home/hermes/tools/melonDS/build-ptrace && make -j12 melonDS)
cd /home/hermes/NiNoKuni && XDG_CONFIG_HOME=$S/melonds_cfg SDL_AUDIODRIVER=dummy \
  MELONDS_ARM9TRACE_PATH=$S/p4z_melpc.txt MELONDS_ARM9TRACE_N=60000000 \
  MELONDS_ARM9TRACE_LO=0 MELONDS_ARM9TRACE_HI=0x04000000 \
  MELONDS_ARM9TRACE_TSLO=50490000 MELONDS_ARM9TRACE_TSHI=54350000 \
  MELONDS_ARM9TRACE_EXIT=1 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
cd /home/hermes/NiNoKuni/worktrees/thread-wake
NNK_GX=1 NNK_IMAGE_C=/home/hermes/NiNoKuni/work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  bash tools/build_split.sh out/split/arm9_gx
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 NNK_GX=headless \
  NNK_GX_SHOT=$S/p4z_sh NNK_GX_SHOT_EVERY=10 NNK_TRACE_FROM=38987000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
SKIP_MELONDS=1 bash tools/verify_phase2a.sh
```

Evidence: `$S/p4z_melpc.txt` (melonDS ts-gated ARM9 PC trace, the named probe),
`$S/p4z_run{1,6}.{out,err}` (native baseline + fixed), `$S/p4z_sh.*.ppm`
(uniform frames), `$S/p4z_build*.log`, `$S/p4z_verify.log`.
Changed in this phase: `tools/nnk_smc.inc` (format-9 mask/offset + T-bit
recovery), `tools/add_arm9trace_ts.py` (new probe).
