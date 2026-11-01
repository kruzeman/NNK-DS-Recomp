# Phase-5AN — a Thumb `blx rM` keeps the target's bit0 (BLX rM interworks like BX)

Follows Phase-5AM (merged as master `c48106f`, class 48).  5AM removed the
0x449F ov17 loop and left the guest at step **559,830,681** with
`halt=dispatch to untranslated pc` at pc `0x020DEE48` (next pc 0).

**5AN clears that barrier.**  The word at 0x020DEE48 (`bd30`,
`pop {r4,r5,pc}`) was never the problem: the function at ov17 `0x020DEE24` was
entered in **ARM state** because the live re-decoder forced the ARM state for
the `blx rM` half of Thumb format 5.  The ARM reading of the callee's first four
bytes (`0xB083B530`) is one decodable instruction, so it silently swallowed
**both** halves of the prologue (`push {r4,r5,lr}` + `sub sp,#12` = 24 bytes of
frame) and the epilogue `add sp,#12; pop {r4,r5,pc}` popped the caller's stack
data — pc came back 0.  With `smc_branch_mode(tv)` (bit0, as melonDS does) the
guest runs **297,477 steps further**: new frontier **step 560,128,158**.

## 1. What 0x020DEE48 is, and why it was unreachable

```
ov17 (base 0x020D0A00)   Thumb
0x020DEE24  push {r4,r5,lr}     0xB530
0x020DEE26  sub  sp,#12         0xB083
0x020DEE28..3E  body: ldr r5,[sp,#24..32]; str; bl 0x020DDB0C
0x020DEE42  bl   0x020DEE7C
0x020DEE46  add  sp,#12         0xB003
0x020DEE48  pop  {r4,r5,pc}     0xBD30     <-- the reported halt pc
```

The pc trace (native, `NNK_TRACE_FROM=559829000`) around the call:

```
... 0x020e2b62 0x020e2b64 0x020e2b66      <-- `blx r6`, r6 = 0x020DEE25
    0x020dee24  0x020dee28 0x020dee2a ...  <-- 0x020DEE26 is MISSING
    ... 0x020dee96 (callee `bx lr`) 0x020dee46 0x020dee48 -> pc=0
```

`0x020E2B5C..66` loads the callee out of the object's vtable:

```
0x020E2B5C  ldr r6,[r0,#0]     ; r0 = object+0x50
0x020E2B60  ldr r6,[r6,#8]     ; = 0x020DEE25  (Thumb, tagged)
0x020E2B66  blx r6
```

Halted register file (`halt state:`): `r6=020DEE25 r13=027E3A60 r14=020DEE47
halt_pc=0x020DEE48`, next dispatch pc `0x00000000`.

## 2. Root cause: `smc_branch_mode` was fed the MASKED address for BLX rM

`tools/nnk_smc.inc`, Thumb format 5 case 3 (`bx rM` / `blx rM`, 0x4700..0x47FF):

```c
*npc = tv & ~1u;
smc_branch_mode((hw & 0x0080u) ? (*npc) : tv);   /* <-- BEFORE: BLX rM -> ARM */
smc_branch_mode(tv);                              /* <-- AFTER                */
```

Phase-5X wrote that arm on the belief that "BLX rM is always ARM state".  That is
the **BLX `<imm>`** rule (where the encoding, not the register, selects ARM).  For
`blx rM` the state is `R[m]` bit0, exactly as for `bx rM`.  melonDS is the
reference and its two handlers are the same two lines
(`src/ARMInterpreter_Branch.cpp:49..59`):

```c
void A_BX(ARM* cpu)      { cpu->JumpTo(cpu->R[cpu->CurInstr & 0xF]); }
void A_BLX_REG(ARM* cpu) { u32 lr = cpu->R[15] - 4;
                           cpu->JumpTo(cpu->R[cpu->CurInstr & 0xF]);
                           cpu->R[14] = lr; }
```

`JumpTo` sets the Thumb flag from bit0, so a tagged Thumb pointer stays Thumb.

The failure was silent because the ARM reading of `0x020DEE24` is a legal
instruction: word `0xB083B530` = `andlts r3, r0, r0, lsr r5`, pc += 4.  Nothing
halted at the call — the *stack* was wrong 36 instructions later.

## 3. melonDS cross-check — it DOES execute this function (Thumb)

`tools/melonds-p5al-probe.patch` StreamProbe, same operator script, 2600 frames
(`work/phase5an/probe1.probe.txt`, `MELONDS_STREAMPROBE_PC=0x020DEE24,
0x020DEE46,0x020DEE48,0x020DEE78,0x0207A238,0x020DDE70`):

```
sp hit=1  pc=0x020DEE24 ts=55964641  cpsr=0x6000003F r6=0x020DEE25 lr=0x0207CB7C
sp hit=2  pc=0x020DEE46 ts=55985432  lr=0x020DEE47
sp hit=3  pc=0x020DEE48 ts=55985432  lr=0x020DEE47
sp hit=4  pc=0x020DEE78 ts=55985439
sp hit=29 pc=0x020DEE24 ts=5940481598 cpsr=0x6000003F r6=0x020DEE25 lr=0x0207A228
sp hit=32 pc=0x020DDE70 ts=5940481802 lr=0x0207A23C   <-- ov6 0x0207A238 call
```

`cpsr=0x6000003F` has the T bit set: melonDS runs 0x020DEE24 in **Thumb** with
`r6=0x020DEE25`, then reaches 0x020DEE46 and 0x020DEE48 normally.  So this was
our decode bug, not a divergence — GOAL 1's cross-check answers "our decode is
wrong".

Second correction to 5AM's §5: **melonDS does enter the ov17 chunk dispatcher**
`0x020DDE70`, from `lr=0x0207A23C` (i.e. ov6 `0x0207A238`), at
ts 5,940,481,802.  5AM's "0 StreamProbe hits over 2600 frames" was a probe
window/`MELONDS_STREAMPROBE_N` budget artifact (its hits 400 cap was consumed by
the other PCs), so the ov6→ov17 call is *not* by itself a divergence.

## 4. The fix (both implementations) and the class-49 test

* `tools/nnk_smc.inc` — Thumb format 5 case 3: `smc_branch_mode(tv)` for both
  halves.  This is the live decoder, i.e. the code that actually moved the guest.
* `nnktool/src/recompile.rs` — the static emitter already notes the raw tagged
  register (`nnk_smc_note_branch(_t)`, Phase-5K); class 49 pins it so the two
  implementations cannot disagree again.
* `nnktool/src/recompile.rs` — `halt_reason()` now keeps the **first** reason.
  Every halt path also does `step_limit = steps`, so the next loop iteration used
  to overwrite the real reason with `step budget exhausted` (this is why §5's
  halt is reported that way in the current image; the source is fixed for the
  next generation).
* Class 49 `phase_5an_thumb_blx_rm_interworks_on_bit0_like_melonds`:
  `decode_thumb(0xB530)`/`0xB083` are one halfword each, Thumb stride 2 reaches
  0x020DEE26 while the ARM stride 4 lands on the measured 0x020DEE28, the
  emitter keeps `r[6]` + `nnk_smc_note_branch(_t)` + the Thumb link
  `0x020E2B69`, and the C arm must contain `smc_branch_mode(tv);` and not the
  masked BLX form.  Phase-5X's own guard (c) was updated to the corrected rule.
  `cargo test` = **53 passed**.

## 5. The new frontier (step 560,128,158) — data executed as an ARM SWI

```
done steps=560128158 halt_pc=0x0213D2AC halt=step budget exhausted
halt state: r0=00000001 r1=0000DF72 r2=00000002 r3=00000002 r4=020774B4
            r5=02295304 r6=022CD028 r7=02143158 r13=027E39D8 r14=0202C1A0
hle swi imm24=0x746E65 num=0x74 lo=0x65 st=arm at=0x0213D2AC
```

The last executed pc is `0x0213D2AC` in the overlay arena, and it was decoded in
**ARM** state as a SWI whose immediate is `0x746E65` — the ASCII bytes
`65 6E 74` = `"ent"` followed by `0xEF`, i.e. a string in a data region, not
code.  The HLE has no SWI 0x74, so `hle_swi` returns 0; the live decoder's
`smc: swi not implemented` halt then got overwritten by `step budget exhausted`
(§4, fixed in source).  So the honest frontier reason is **"ARM-state dispatch
into data at 0x0213D2AC (word 0xEF746E65), executed as SWI 0x74"**, and the next
step is to find which branch produced that pc — that is a new class, not a
regression of this one.

The guest has NOT reached the "Skip the cutscene? Yes/No" dialog.  GOAL 2 is not
landed; its premise changed: the ov6→ov17 call happens in melonDS too, so the
remaining gap is not "we enter a path melonDS never enters".

## 6. Regression checks (all green)

| check | measurement |
|---|---|
| movie band, unscripted (`NNK_STEP_LIMIT=2400000000`, 4291 frames presented, shots every 25) | top-screen luma corr **0.9986..0.9990**, MAE 1.53..2.96 vs `op_u` refs f16..f37, frames **4125..4275** — same band and same numbers as Phase-5AD |
| notice screen | f100 bottom: pirate art + "We do not condone piracy" |
| Mobiclip splash | f500 top: "mobiclip VIDEO CODEC", bottom "Licensed by Nintendo" |
| boot order after it | f300 LEVEL5, f700 "New Game" menu |
| ROM-only | `./work/split/arm9_5an "Ni No Kuni.nds"` — no derived files, `smc: generation-0 ... derived from the ROM` |
| speed | 200M steps: **29.40 s** (this build) vs **29.97 s** (5AM master) = 1.9% faster, well inside the 20% band |
| `cargo test` | 53 passed |
| `verify_phase2a.sh` | strict common prefix **1000 / 1000**, resync 1000/1000, rc=0 |

## 7. Evidence

```
work/phase5an/trace1.err + trace3.txt   pre-fix halt at 559,830,681 (untranslated pc=0)
work/phase5an/trace3.txt (l.1265)       the EE24 -> EE28 jump (0x020DEE26 skipped)
work/phase5an/trace4.err                NNK_SMC_TRACE of the same window
work/phase5an/probe1.probe.txt          melonDS: 0x020DEE24/46/48/78 + 0x020DDE70
work/phase5an/fix1.err / fix2.err       post-fix run: 560,128,158, 1006 frames
work/phase5an/fix2.*.ppm(.png)          f100 notice (pirate, "We do not condone
                                        piracy"), f300 LEVEL5, f500 Mobiclip
                                        splash, f700 title menu, f1000 frontier
work/phase5an/bb.err                    unscripted 2.4e9-step run, 4291 frames
work/phase5an/band_bbshots/ + png/      the 0.9986..0.9990 band captures
work/phase5an/verify2a.log              verify_phase2a: 1000/1000, rc=0
work/phase5an/speed_*.err               200M-step wall clock, both builds
```

No game bytes were committed.  The only guest constants in the test are Thumb
encodings (`0xB530`, `0xB083`, `0x47B0`) and measured addresses.
