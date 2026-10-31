# Phase-5A — the br-link store divergence at 0x020D19B8: the Thumb PUSH/POP
# format-13 mask swallowed every POP (runtime re-decoder), fixed and locked

Follows Phase-4Z (merged `7977340`).  Scope: measure the link value at
`bl` 0x020D1642 / `push {r4,r5,r6,lr}` 0x020D19B8 / `pop {r4,r5,r6,pc}`
0x020D1A1E, fix with measured semantics, lock the class with a cargo test, the
picture — or the honest stop-report.  **The link was never wrong: the `bl`
linked 0x020D1647 and the `push` stored it correctly; the `pop` never ran.  The
runtime's on-demand re-decoder matched format 13 with `hw & 0xF600`, a mask
that leaves the L bit (bit 11) out, so every `pop {...,pc}` (0xBDxx) matched
the PUSH arm first and the POP arm was unreachable dead code.  Fixed to
`0xFE00`; the epilogue now returns to 0x020D1646, the guest runs 260,641 steps
past the old halt and follows melonDS's exact path for the next 62 PCs.**

## 0. Headline

* **Root cause (measured, not inferred): Thumb-1 format 13 PUSH/POP mask.**
  `tools/nnk_smc.inc:smc_exec_thumb` tested `(hw & 0xF600u) == 0xB400u` for
  PUSH before `(hw & 0xF600u) == 0xBC00u` for POP.  0xF600 covers bits
  15,14,13,12,10,9 — **bit 11 (`L`) is not in the mask**, so PUSH/POP are
  indistinguishable under it: `0xBD70 & 0xF600 == 0xB400`.  The PUSH arm
  therefore caught every POP and the POP arm was never executed.  Fix: mask
  `0xFE00` (bits 15..9) on both arms.
* **The `bl` link and the `push` were already correct — this was never a
  link-address bug.**  Measured with `NNK_DEBUG_AT_PC`:
  * `bl` 0x020D1642 → `push` 0x020D19B8 (step 39,001,613):
    `r14=020D1647`, `r13=027E3AB8` — the link is exactly melonDS's Thumb
    return `addr+4|1`.
  * The frame the push wrote (dumped at the pop, `NNK_DEBUG_MEM`):
    `0x027E3AA8: 00000000 02143158 00000000 020D1647` = r4, r5, r6, **LR =
    0x020D1647**.  The stack slot was right all along.
  * `pop` 0x020D1A1E (step 39,001,731): `r13=027E3AA8` (the push's frame, top
    word still 0x020D1647), `r14=0204C33C` (the callee's own return).  In the
    Phase-4Z code the next dispatched PC was **0x020D1A20** — the fall-through
    into the literal pool, i.e. the pop had pushed a *second* frame instead of
    popping the first.  With the mask fixed the next PC is **0x020D1646**, the
    Thumb address of 0x020D1647.
* **The Phase-4Z reading "the pop returns 0x020D1A21" was the fall-through**:
  executing 0xBD70 as a PUSH leaves `npc = pc+2 = 0x020D1A20`, and 0x020D1A20|1
  is 0x020D1A21 — the same value the buggy `blx` link produced, which is why
  the two hypotheses were indistinguishable in a PC trace alone.
* **Consequence of the fix (60M run `p5a_runE`):** the guest returns to
  0x020D1646, executes melonDS's handler epilogue
  (`0x020D1646/48/4A -> 0x020D1B6E/70/76/78 -> BL -> 0x020D18B4...`) and runs
  **260,641 steps past the old halt**, distinct PCs **15,783 -> 16,360**,
  halt step 39,001,760 -> **39,262,401**.
* **No picture (honest), and that is now measured to be CORRECT at this
  point** — see §4: melonDS's own composed screens are uniform white for its
  first 102 frames, exactly like native's.

## 1. The fix

```diff
--- a/tools/nnk_smc.inc
+++ b/tools/nnk_smc.inc
-    if ((hw & 0xF600u) == 0xB400u) {                 /* PUSH */
+    if ((hw & 0xFE00u) == 0xB400u) {                 /* PUSH */
-    if ((hw & 0xF600u) == 0xBC00u) {                 /* POP */
+    if ((hw & 0xFE00u) == 0xBC00u) {                 /* POP */
```

The POP arm itself was already correct (list bit 8 → 0x8000 = pc, `*npc = tv
& ~1u`, `r13 = sp + idx*4`); it was simply unreachable.  The Rust half
(`nnktool/src/decode.rs`, format 14) re-tested the L bit and so never
mis-executed a live word — but its `0xF600` mask is now `0xFE00` with both arms
matched explicitly (`0xB4xx` PUSH, `0xBCxx` POP), so the two halves key on the
same field.  That change is covered by the tests below (matching only 0xB400
would silently turn every pop in a regenerated image into `Kind::Undef` — the
new test caught exactly that during development).

## 2. Class name and generalisation

**Class: format-mask too narrow — a discriminating encoding bit left out of
the comparison, with the earlier arm winning.**  This is the third member of
the "mask" family after Phase-4K's long-multiply mask and Phase-4F's LDRD/
LDRSB keying: the arm is present and its body is right, but the *selector*
does not distinguish the encodings it is meant to separate.  Checking for it
is cheap: for every `(hw & MASK) == VAL` arm in `smc_exec_thumb`, confirm that
the remaining encodings in that format space (here 0xB4xx/0xB5xx push vs
0xBCxx/0xBDxx pop, 0x49xx LDR-literal, 0xCxxx LDM/STM) map to different
values under MASK.

## 3. Verification (branch build, `out/split/arm9_fix`)

```
baseline (Phase-4Z code)  halt step 39,001,760 pc 0x020D1A8E distinct_pcs 15,783
                          (pushed the epilogue's frame; fell through 0x020D1A20)
+ mask fix (p5a_runE)      halt step 39,262,401 pc 0x02178E9E distinct_pcs 16,360
                          pop 0x020D1A1E -> 0x020D1646 (was 0x020D1A20)
                          state_fallbacks 2 -> 2, on_demand 844 -> 1,413
                          melonDS PCs matched after the dispatch: 62 (see §5)
cargo test (nnktool)      14 passed (3 new class-5 tests; the source-level one
                          goes red when the 0xF600 mask is put back:
                          "regression: smc_exec_thumb is masking format 13
                          with 0xF600 again (bit 11 hidden)")
verify_phase2a.sh         RESULT PASS, rc 0 on this branch (SKIP_MELONDS=1):
                          strict common prefix 1000/1000 melonDS PCs (100.0%),
                          window-filtered 1000/1000, in-order after resync
                          1000/1000
```

## 4. The picture: uniform frames are CORRECT at this point (operator steer)

Native frames (`NNK_GX_SHOT`, headless but through the merged gxrender
compositor): frames 10 and 20 solid **black (0,0,0)**, frames 30..70 solid
**white (255,255,255)**, both screens; 76 frames presented at the halt.
PNGs in `/home/hermes/NiNoKuni/shots/p5a_sh1.000{20,70}.{top,bot}.png`.

melonDS at the same boot stage (`gxrender/traces/2dsmoke1/f*.scr`, the
Phase-1B 2D-state capture of this ROM's boot, 119 frames of composed screens):
frames **0..101 uniform white (255,255,255)**, frames 102..117 a master-
brightness fade 239 -> 0, frame 118 black top / dark bottom (3 colours).  So at
frame ~76 — native's run point — **melonDS's screens are also uniform white**:
the display path is already correct at this boot stage, and "no content yet"
is the ground truth, not a rendering failure.  Content (the fade into the
title/menu) starts at melonDS frame ~102, which is still ahead of the guest
(the Phase-4Z display switch 0x0207CB10 sits at melonDS ts 54.27M).

Per-pixel diff, native frame N vs melonDS `f000NN.scr` (top and bottom screen,
RGB, 256x192, `p5a_framediff.py`):

```
frame 70 (native white)  vs f00070: 0 differing pixels, max channel delta 0   (top AND bottom)
frame 10 / 20 (native black) vs f00010/f00020: 49152/49152 pixels differ, delta 255
```

Honest difference, stated rather than hidden: native's first ~20-29 frames are
black where melonDS fills white while the LCDC is still uninitialised; from
frame ~30 (DISPCNT/palette brought up) both are solid white and pixel-identical.
So the earlier "solid black -> solid white" observation is the same LCDC
bring-up melonDS shows, phase-shifted by ~20 frames.

## 5. The next barrier (named, measured)

Aligning the native trace from the dispatch (260,669 PCs, `p5a_trace.out`)
against melonDS's trace from 0x020D1646 (`p4z_melpc.txt`, 1,656,296 PCs),
skipping melonDS's second-halfword entries for 32-bit Thumb instructions
(2 of them, e.g. 0x020D1B7A for the BL at 0x020D1B78):

```
matched 62 PCs, first divergence at mel index 64:
  melonDS  ... 0x02049BD4 0x02049BD8 0x02049BDC 0x02049BF4 ...
  native   ... 0x02049BD4 0x02049BD8 0x02049BDC 0x02049BE0 0x02049BE4 0x02049C18 ...
```

Disassembly (arm9.bin): `0x02049BD0 str r1,[r0,#12]`, `0x02049BD4 ldr
r2,[r0,#12]`, `0x02049BD8 cmp r2,#0`, `0x02049BDC beq 0x02049BF4` — melonDS
takes the branch (**r2 == 0**), native does not: measured at that hit
(`NNK_DEBUG_AT_PC=0x02049BD4`, 4th hit, step 39,001,791)
`r0=021A7554 r1=00000000 r2=00002C00` — native runs `cmp r2,#1` / `beq
0x02049C18` with **`[r0+12] = 0x00002C00`** where melonDS has 0.  So the
divergence is a *value* in the object at `[r0+12]`, not an instruction:
something wrote 0x2C00 where melonDS has 0 between the dispatch and here
(the same routine ran earlier at step 17,589,773 with the same object,
r2 = 0 then 0x2C00 on its 2nd/3rd entry, so the field is a rolling
queue/transfer state).  Same family as the Phase-4S/4T barriers (a modelled
service/queue state that answers differently), and this is where the next
phase should start: dump `r0`/`r1` at 0x02049BD0 (`NNK_DEBUG_AT_PC`), walk
back to the writer of `[0x021A7554+12]` (`NNK_DEBUG_MEM`/memwatch on the
store), and diff the melonDS value at the same point.

After that, native runs 260k steps into the overlay region 0x0214xxxx-0x0217xxxx
and halts honestly at **0x02178E9E, word 0x1C36F000** ("smc: unsupported live
instruction", `img=0x00000000`) — melonDS executes **no** PC in
0x02140000..0x0217FFFF at all in its capture, so that tail is native-only.

## 6. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/br-link      # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  bash tools/build_split.sh out/split/arm9_fix
# the three measured stages (push/pop frame dump, then the post-fix pop)
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=39002000 \
  NNK_GX=headless NNK_TRACE_FROM=39001640 NNK_DEBUG_AT_PC=0x020D1A1E \
  NNK_DEBUG_N=1 NNK_DEBUG_MEM="0x027E3AA8:32" NNK_DEBUG_MEM_PATH=$S/p5a_frame_fix.txt \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_fix work/phase3a/arm9_stored_padded.bin
# the full run + frames
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 \
  NNK_GX=headless NNK_GX_SHOT=$S/p5a_sh1 NNK_GX_SHOT_EVERY=10 \
  NNK_TRACE_FROM=38987000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_fix work/phase3a/arm9_stored_padded.bin
(cd nnktool && cargo test)                       # 14 tests
SKIP_MELONDS=1 bash tools/verify_phase2a.sh
```

Evidence (`$S` = the scratch dir above): `p5a_runA/B2.{out,err}` (baseline,
push register dump), `p5a_frame_baseline.txt` vs `p5a_frame_fix.txt` (the
stack frame before the pop, in both builds), `p5a_runE.{out,err}` (60M run,
frames, 16,360 distinct PCs), `p5a_sh1.*.ppm` (+ PNGs in
`/home/hermes/NiNoKuni/shots/`), `p5a_trace.out` (260,669-PC native trace),
`p5a_mel_from_dispatch.txt`, `p5a_cmpalign.py` (the alignment), `p5a_verify.log`,
`p5a_build*.log`.

Changed in this phase: `tools/nnk_smc.inc` (format-13 mask `0xF600 -> 0xFE00`
on both arms), `nnktool/src/decode.rs` (same mask, both arms matched
explicitly), `nnktool/src/decoder_regression_tests.rs` (class 5, 3 tests),
`DECODER_TESTS.md`, this document.
