# Phase-5AW — the prompt press: two defects, both fixed

Follows Phase-5AT (merged `064ab93`).  Build:
`NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh work/split/arm9_5aw2`
(prelude audit green, `cargo test`: **59 passed**).

## 1. Result

The card's reproducer **no longer halts**:

```
NNK_SAVESTATE=work/repro_5au/operator_dialog.nnks NNK_SAVESTATE_IGNORE_BUILD=1 \
  NNK_INPUT_FILE=work/repro_5au/press_a_at_frame1990.nnk \
  NNK_STEP_LIMIT=1240000000 NNK_GX=headless work/split/arm9_5aw2 ROM
done steps=1240000001 traced=1240000001 distinct_pcs=26416 hle_hits=8907
     halt_pc=0x0200AD78 halt=step budget exhausted
gx: 2220 frames presented
```

Two independent defects stood between the A press and the save path, and both
are fixed in the runtime/recompiler (no halt masking, no address
special-case):

| # | defect | where | how it showed |
|---|---|---|---|
| 1 | an ARM exception return dropped the restored **SPSR T bit**, so the resumed `lr-4` was decoded in the stale Thumb state | `recompile.rs` emitter + `nnk_smc.inc` carrier + a generated splice for the frozen image | halt `0x022CA20C` (my scripted measurement) |
| 2 | the ROM-derived **live decoder sign-extended a Thumb `b` displacement from the wrong bit**, so every *backward* `b.n` landed `+0x800` too high | `nnk_smc.inc` format 17 | halt `0x0212CCF6` (the operator's measurement) |

After the two fixes the press is consumed, the save path runs, and the guest
parks in the pointer-list spin at `0x0200AD78` — the *existing* wait frontier
Phase-5AT already named for a savestate run, not a decode or dispatch halt.
The bottom screen fades to black on the way (only frame 2000 is non-blank in
`work/phase5aw/verify/after2/`, the save transition), so this task's visual
evidence is "the prompt overlay is gone and the guest enters the save path",
not a new dialogue line.

## 2. Defect 1 — the exception return carried no instruction state

Trace (`NNK_TRACE_FROM` + `NNK_SMC_TRACE_FROM` around the last 3,000 steps):

```
0x0200729c  subs pc, lr, #4            <- ARM exception return, SPSR=0x2000001F (T=0)
0x01ffc684  (resumed pc = lr-4)
```

* The SPSR is `0x2000001F`: mode SYS, **T = 0**, so the resumed instruction at
  `lr-4 = 0x01FFC684` is **ARM**.
* `0x01FFC684` is ITCM, live-decoded, and the previous live decode had been a
  Thumb stream.  The exception-return case restored the mode byte and NZCV but
  **dropped the T bit**, so `smc_pick_state()` fell through to the sticky guess:

  ```
  0x01FFC684  ARM   e089314a  add r3, r9, sl, asr #2
              Thumb 314a     adds r1, #0x4A       <- what ran
                    e089     b.n 0x01FFC79C
  ```

* The ARM epilogue at `0x01FFC4EC` (`pop {pc}` at `0x01FFC548`) then ran
  **without its `push {lr}` prologue**, popped the codec pointer `0x022C9F38`
  and executed the movie data table as ARM until the word `0xFFFFFFD3` at
  `0x022CA20C` did not decode.

The resumed pc is a dynamic `lr-4` that no compiled case, no ROM-derived
pcform run and no register/stride guess can classify: the restored SPSR is the
only authority.

## 3. The fix for defect 1

* `nnktool/src/recompile.rs`: every emitted ARM exception-return block
  (`subs`/`movs pc, lr` with S=1, rd=15, privileged mode) calls
  `nnk_smc_note_exc_return(pc, _spsr)` after restoring the CPSR byte and NZCV.
* `tools/nnk_smc.inc`: the hook records `smc_branch_mode((target & ~1u) |
  ((spsr >> 5) & 1u))` and marks it live (hardware state may overrule a
  compiled case).  The live decoder's own exception return does the same.
* `tools/split_dispatch.py` declares the hook; `tools/p5aw_exc_return_splice.py`
  applies it to the frozen image's 13 exception-return case bodies, run
  marker-gated by `tools/build_split.sh` with a manifest row that quotes the
  splice's own marker (`NNK_PRELUDE_DISABLE_SPLICE=5aw_exc_return_state` is the
  negative control — verified: `AUDIT FAILED -- 5aw_exc_return_state MISSING`,
  build refused).

## 4. Defect 2 (the operator's `0x0212CCF6`) — a backward `b.n` at +0x800

With defect 1 fixed the run reached the operator's exact halt frame
(`0x0212CCF6`, `r0=027E3A9D`, `r13=027E3C28`, `r14=0212CCDD`, `nzcv=0010`).
`0x0212CCF6` is the shared return thunk of the Thumb function entered at
`0x0212B93C` (ov17 at `0x020D0A00`, i.e. `ov017.bin + 0x5AF3C`; that whole page
range is **not** in the compiled image, so it runs in the live decoder):

```
212b93c  b5f8  push {r3,r4,r5,r6,r7,lr}
212b93e  b0ff  sub sp, #0x1FC
212b940  b0a7  sub sp, #0x9C          ; frame 0x298 + 24 = 0x2B0
212b95e  f001 f9c8  bl 0x212ccf2       ; "return via thunk": Thumb-1 `b` is only
212bc2e  f001 f83c  bl 0x212ccaa       ;  +-2 KB, so the epilogue is CALLED
212ccf2  b07f  add sp, #0x1FC
212ccf4  b027  add sp, #0x9C
212ccf6  bdf8  pop {r3,r4,r5,r6,r7,pc}
```

The pop read `0x027E3C10..0x027E3C27` — the prologue's own save area for
`P = 0x027E3C28` — and all six words were zero, so `pc = 0`.
`NNK_STOREWATCH=0x027E3C10:0x027E3C28` (limit 200 000) logged **no store in the
whole run**, while the same watch on `0x027E3800:0x027E3C40` is full — the
prologue of that function never ran, and the body was being entered in the
middle.

The entry is now measured.  `NNK_DC_PC` on the body's first instructions gives
`pc=0x0212BC20 hit=1 step=1140779401 lr=0x0212B923`, and the trace around that
step is the whole answer:

```
0x020d3dd0              ; pop {r4,pc} of the compiled ARM helper 0x020D3DAC
0x0212b922 .. 0x0212b932 ; F_prev's tail (ldr/adds/str/.../cmp/bge)
0x0212b932  e56d        ; b.n 0x212b410   <- offset -0x526
0x0212bc10  ...          ; ACTUAL: 0x212bc10 = 0x212b410 + 0x800
```

`0x212B932`'s branch is the bug.  Thumb format 18 is `11100 imm11`; the
displacement is **signed 11 bits, then doubled**, so the value to sign-extend
has 12 bits.  The live decoder had

```c
int32_t bo = (int32_t)((hw & 0x7FFu) << 1);
*npc = pc + 4u + (uint32_t)((bo << 21) >> 21);
```

and `bo << 21` shifts `bo`'s bit 11 — the sign bit, which only exists *after*
the doubling — out of the 32-bit word.  For `0xE56D`: `bo = 0xADA`, the correct
displacement is `-0x526`, the old idiom produced `+0x2DA`, and the branch landed
at `0x212B410 + 0x800 = 0x212BC10`, i.e. inside the neighbouring function's body.
That function then ran its shared return thunk and popped a frame its prologue
had never pushed: `pc = 0`, `dispatch to untranslated pc`.

The compiled path never had this bug (`decode.rs`:
`(((h & 0x7FF) << 5) as i16) >> 4`), which is why the defect only appeared in
overlay pages the ROM-derived live decoder serves.

## 5. The fix for defect 2

```c
int32_t bo = (int32_t)((hw & 0x7FFu) << 1);
*npc = pc + 4u + (uint32_t)((bo << 20) >> 20);   /* sign-extend 12 bits */
```

One line, no address special-case, and it is the class fix: every backward
Thumb `b.n` in every live-decoded page was affected.  Format 15 (conditional,
doubled imm8 -> 9 bits, `<<23 >>23`) and the BL/BLX pair (sign at bit 24,
`<<7 >>7`) were already correct and are pinned by the test.

Class test 59 (`phase_5aw_live_decoder_sign_extends_a_thumb_backward_branch`)
pins the semantics three ways: `decode_thumb(0xE56D, …0x0212B932)` must be
`b.n 0x0212b410`; the arithmetic must give `-0x526` for the correct idiom and
`+0x2DA` for the old one (so the old form cannot look equivalent again); and
the live decoder source must carry `<<20 >>20` while still carrying the
unaffected `<<23 >>23` / `<<7 >>7` forms.  Suite 58 → **59**.

## 6. Measurements

| check | result |
|---|---|
| card reproducer, final build | **no halt**: `done steps=1240000001 … halt_pc=0x0200AD78 halt=step budget exhausted`, 2220 frames, 13/13 input events applied, 26,416 distinct PCs |
| the same reproducer before the two fixes | halt `0x022CA20C` @1,112,592,480 (defect 1), then halt `0x0212CCF6` @1,140,846,949 with the operator's exact registers (defect 2) |
| post-press screens | `work/phase5aw/verify/after2/s.0*.bot.ppm`: frame 2000 carries the (mis-rendered) prompt overlay, every later capture is black — the save transition; the guest is in the pointer-list spin |
| `cargo test` | **59 passed** (57 before this card) |
| `tools/verify_phase2a.sh` | `rc=0`; decoder cross-check `RESULT: PASS`; melonDS-vs-native **1000/1000** strict prefix, 1000/1000 window-filtered, 1000/1000 in-order |
| prelude audit, negative control | `NNK_PRELUDE_DISABLE_SPLICE=5aw_exc_return_state` → `AUDIT FAILED -- 5aw_exc_return_state MISSING`, build refused |
| savestate round trip 5M→6M | `fnv=0xA258DF1B crc=0x4341C3DE` — identical on save and reload (build `0x0768D9EB`) |
| savestate round trip 30M→32M | `fnv=0xAC8F90B0 crc=0x6FFECEF1` — identical |
| ROM-only runtime (binary + card image, clean dir, no `work/`) | 20,000,001 steps, 13,328 distinct PCs, budget exhausted |
| speed | 700 M steps in **66.9 s** (19.1 s / 200 M; 5AT baseline 30.8 s / 200 M) |
| notice screen | fresh `work/phase5aw/verify/notice_01100_bot.png` — the fan-translation notice on the parchment background (29 non-blank) |

## 7. Honest gaps of this task

* **melonDS ground truth is still not loadable here.**  `work/repro_5au/melonds_state.mln`
  is savestate format major **13** (`MELN`, version word `0x0d`); every melonDS
  build in this environment is `SAVESTATE_MAJOR 14` (bumped at `ba317e2e`) and
  the loader refuses a major mismatch.  No offline source for an older build,
  so the melonDS side of the comparison was answered by the ROM's own bytes
  (disassembly + the measured branch) instead of by replay.
* **The title-screen skip script was not run**: the cold-boot path still parks
  in the `0x020078F4` halfword spin long before frame 4,300 (my 700 M-step run
  ends at `0x020078F8/0x020078FC`), the same pre-existing wakeup blocker
  Phase-5AT recorded.
* **The movie band audit was not re-run** (needs a ~2.9 e9-step intro capture
  plus per-frame shots against `work/p5t/op_u.yuv`).  No movie, codec or YUV
  code moved; the two fixes are a state note at exception returns and one
  sign-extension in the live branch decoder.
* The new parking point (`0x0200AD78`, a pointer-list spin) is the *known*
  Phase-5AT wait frontier, not part of this card's defect.  Whoever takes the
  save path next starts there.
