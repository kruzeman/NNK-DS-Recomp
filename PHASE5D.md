# Phase-5D — 0x020078F4 is a VCOUNT==0 spin (measured); the real barrier was ARM interworking on the returning `pop {pc}`

Follows Phase-5C (merged `7959fe9`).  Scope: identify what wakes the boot wait
loop at `0x020078F4`, deliver it natively, or stop with the exact class named.

**Result: the loop is not an IRQ wait and native never got stuck in it (the
Phase-5C claim was a `TRACE_LIMIT` truncation artefact).  It is the SDK's
"wait for start of frame" helper: it spins on `VCOUNT == 0`.  The run's real
halt at step 39,237,994 was one instruction *after* the ARM helper at
`0x020E7968` returns: an ARM `pop {r3,r4,r5,pc}` whose stack word is
`0x020D241D`.  melonDS interworks there (bit0 -> Thumb); the runtime kept ARM
state and decoded the Thumb `blx 0x0204D3D0` at `0x020D2420` as an ARM
`svc 0xD6F77A`.  Fixed (with melonDS source + trace ground truth) — the run now
follows melonDS's Thumb path and advances 310 steps further to a new, named
barrier.  Distinct PCs 16,331 -> **16,412**.**

## 0. The loop at 0x020078F4..0x020078FC (disassembly, ARM)

```
0x020078E8  mov  ip, #0x04000000
0x020078EC  ldr  r1, [ip, #0x208]      ; save REG_IME
0x020078F0  str  ip, [ip, #0x208]      ; IME = 0x04000000 (bit0 = 0: IRQ off)
0x020078F4  ldrh r0, [ip, #6]          ; REG_VCOUNT  (0x04000006)
0x020078F8  cmp  r0, #0
0x020078FC  bne  0x020078F4            ; spin while VCOUNT != 0
0x02007900  str  r1, [ip, #0x208]      ; restore IME
0x02007904  bx   lr
```

The polled state is **REG_VCOUNT** (`0x04000006`), not DISPSTAT
(`0x04000004`) and not a flag in main RAM: the guest waits for the **start of
line 0**, i.e. the end of the vertical blank.  There is no IRQ involved (it
even masks IME while polling), so the Phase-4N "wake event + cadence" pattern
does not apply — the "event" is arithmetic on the LCD clock.

### melonDS ground truth (`MELONDS_LOOPWATCH_PATH`, window LO=0x020078E8 HI=0x02007904)

```
enter ts=12287887 pc=0x020078E8 cpsr=0x2000001F r13=0x027E3B68 r14=0x02007958 ie=0x00040000 if=0x00080000 ime=0
leave ts=12324209 pc=0x02007904 cpsr=0x6000001F r13=0x027E3B68 r14=0x02007958 ie=0x00040000 if=0x00080000 ime=0 iters=7263
post  ts=12324213 pc=0x02007958
post  ts=12324219 pc=0x0202986C
post  ts=12324224 pc=0x02046244 ... 0x02046264
post  ts=12324245 pc=0x02007AC0
```

* **cadence**: one call in the whole boot; 7,263 loop iterations; the loop
  leaves at `ts=12,324,209`, **36,322 cycles** after entry (~5 cycles per
  3-instruction iteration), with `VCOUNT == 0`.
* **post-wake path**: `0x02007904` (epilogue) -> `0x02007958` (the `bl`
  return site) -> `0x0202986C` -> `0x02046244..0x02046264` -> `0x02007AC0`.
  Native takes *the same* path (`p5d_tail.out`: `0x02007954 -> 0x020078E8`
  ... `0x020078FC` ... -> `0x02007958`), just with its documented coarse
  cadence (1 cycle/instruction, 3 instructions per iteration => up to 186,730
  iterations to cross one 2130-cycle scanline instead of 7,263).

**Consequence: nothing had to be "delivered" for this loop.**  It already
terminates natively; the step budget was consumed *after* it.  Phase-5C's
"halt inside the loop" came from `TRACE_LIMIT` (4,000,000 lines) cutting the
trace while the PC stream was still spinning at the loop, 231,753 steps before
the run's real end — the trace tail is not the run's tail.

## 1. The actual barrier: ARM `pop {pc}` interworking

Halt (before the fix, 60M-step run): `halt_pc=0x020D2420`, steps 39,237,994,
`hle swi imm24=0xD6F77A num=0xD6 ... st=arm at=0x020D2420`.

`NNK_SMC_TRACE=200000` shows the live decoder's own view (`p5d_smc2.err`):

```
step=39237991 pc=0x020E79A8 word=0xE8BD8038 state=arm -> 0x020D241C   ; pop {r3,r4,r5,pc}
step=39237992 pc=0x020D241C word=0x6B204C13 state=arm -> 0x020D2420   ; Thumb `ldr r4,[pc,#76]` read as ARM
step=39237993 pc=0x020D2420 word=0xEFD6F77A state=arm -> 0x020D2424   ; Thumb `blx 0x0204D3D0` read as `svc 0xD6F77A`
```

The stack word is `0x020D241D` (odd: the Thumb return address of the
`blx 0x020E7968` at `0x020D2418`).  The decoder masked it
(`*npc = lv & ~1u`) and, with no register still holding the tagged value,
fell back to the sticky ARM state.

**Ground truth, melonDS source** (the project's reference implementation):

| form | melonDS | interworks? |
|---|---|---|
| `LDM`/`pop` with PC | `src/ARMInterpreter_LoadStore.cpp:464` `JumpTo(pc, ...)`; `src/ARM.cpp:729` `if (addr & 0x1) CPSR |= 0x20` | **yes** |
| ARM `LDR` into PC | `src/ARMInterpreter_LoadStore.cpp:95` `if (Num==1) val &= ~0x1; JumpTo(val)` | no (ARM9) |
| ARM data-processing into PC | `src/ARMInterpreter_ALU.cpp:321` `JumpTo(res & ~1)` | no |
| Thumb `pop {pc}` | `src/ARMInterpreter_LoadStore.cpp:745` `if (Num==1) pc |= 0x1` | always Thumb |
| ARM `BLX <Rm>` | `src/ARMInterpreter_Branch.cpp:57` `lr = R[15]-4; JumpTo(Rm); R[14] = lr` | yes, LR **even** |

**Ground truth, melonDS ARM9 trace** at the same point
(`MELONDS_ARM9TRACE_LO=0x020D2400 HI=0x020D2440`, `p5d_mel_d24.txt`):

```
0x020d2414  0x020d2416  0x020d2418  0x020d241a  0x020d241c  0x020d241e  0x020d2420
```

Two-byte strides through `0x020D241C`/`0x020D241E` — melonDS executes Thumb
there and then takes the 32-bit `blx 0x0204D3D0` at `0x020D2420`.

## 2. The fix (`tools/nnk_smc.inc`, 4 sites)

1. ARM `LDM` with PC in the list: `*npc = lv & ~1u; smc_branch_mode(lv);`
2. Thumb `pop {pc}`: `*npc = tv & ~1u; smc_branch_mode(tv | 1u);`
3. ARM `bx/blx rM`: carries the target's state (already recovered from the
   register indirectly; now explicit).
4. **ARM `blx rM` links `pc + 4u`, bit0 CLEAR** — this was a pre-existing bug
   (`r[14] = (pc + 4u) | 1u`), not a Phase-5D choice.

Site 4 is why Phase-5C's rejected variant "regressed": with the odd LR, the
decryptor stub `0x0215AC1C` (`E12FFF3C blx r12`) pushed `0x0215AC21`, so the
helper's `pop {pc}` re-entered the **ARM** code at `0x0215AC20`
(`E1A02005 mov r2,r5`, dumped from the run's RAM) in Thumb state and walked off
at step 13,146,753.  With the LR fixed there is no 13.1M regression: 13.1M is
now clean and the frontier moves *past* 39,237,994.

Kept masked on purpose (melonDS does not interwork there): ARM `LDR` into PC,
ARM data-processing into PC.  A cargo test locks all four decisions.

## 3. Verification (branch code, `out/split/p5d_fix2`)

| run | steps | distinct_pcs | frames | halt |
|---|---|---|---|---|
| before (Phase-5C image) | 39,237,994 | 16,331 | 76 | `0x020D2420` ARM `svc 0xD6F77A` (garbage decode) |
| after (this branch) | **39,238,304** | **16,412** | 76 | `0x0204C784` `pop {r3,r4,r5,r6,pc}` returns `0x00000002` → `untranslated pc=0x00000002` |

`smc: probes=35,851,318 stale=130,972 redecoded=132,442 halts=0
state_fallbacks=1 on_demand=1,470`; the 13.1M decryptor path is unchanged
(`0x0215AC1C blx r12` -> `0x0215A7C0` ARM memcpy -> returns via `pop {pc}`
to `0x0215AC20` in **ARM** state now).

`cargo test` in `nnktool`: **18 passed** (17 previous + 1 new class-7 test
`runtime_smc_redecoder_interworks_on_ldm_pc_and_links_arm_blx_even`; the old
class-4 assertion that locked `pc+4|1` was corrected against melonDS).

## 4. The next barrier (named, measured)

`0x0204C784  pop {r3,r4,r5,r6,pc}` of the ARM function at `0x0204C730`
(`push {r3,r4,r5,r6,lr}; sub sp,#12; ... bl 0x020027EC; add sp,#12; pop ...` —
a balanced frame) returns **`0x00000002`** in the PC slot at step
39,238,304, i.e. the LR saved at entry was `2`.  At the pop the other saved
words are `r3=0x020D24A3`, `r4=0x0213A3D0`, `r5=0x020D23E3`, `r6=1` (two
Thumb-tagged `0x020D2xxx` code pointers), `r1=0x2ED4D20A`, `r7=0x02069688`,
`r13=0x027E3B48`.  Class: **caller-side link / frame content** in the
`0x020D2xxx` dispatch helpers — the call that should have linked
`0x020D24A3`-adjacent code entered with `lr = 2`.  Follow-up: trace the
`0x020D2xxx` helper chain at step 39,238,2xx and compare its frame with
melonDS at the matching ts.

## 5. The picture (honest)

Frames are still **uniform white** (76 presented; melonDS's content switch is
frame ~102, still ahead).  PNGs saved:
`/home/hermes/NiNoKuni/shots/p5d_f60_top.png`, `..._bot.png` (256x192, 443-byte
PNG = single colour), from `p5d_sh.00060.{top,bot}.ppm` of the run above.

Operator telemetry folded in: `gx: 114,451 unhandled io writes` (was 114,448;
unchanged class), `vrc: VRAM window split ... LCDC=491520` unchanged — no
per-register histogram is available without touching `gxrender/` (the counter
is `gxd_unhandled_io()`), and no VRAM/palette consumer exists at frame 76 in
either implementation, so neither is this barrier's cause.

## 6. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/wait-loop     # work -> ../work symlink
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_p5b_kp.c bash tools/build_split.sh out/split/p5d_fix2
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_STEP_LIMIT=60000000 NNK_GX=headless \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/p5d_fix2 work/phase3a/arm9_stored_padded.bin
(cd nnktool && cargo test)                        # 18 tests
SKIP_MELONDS=1 bash tools/verify_phase2a.sh       # branch code
# melonDS ground truth (build-ptrace, loopwatch window is now env-driven):
MELONDS_LOOPWATCH_PATH=$S/p5d_mel_loop.txt MELONDS_LOOPWATCH_LO=0x020078E8 \
  MELONDS_LOOPWATCH_HI=0x02007904 MELONDS_LOOPWATCH_AFTER=60 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"
MELONDS_ARM9TRACE_PATH=$S/p5d_mel_d24.txt MELONDS_ARM9TRACE_LO=0x020D2400 \
  MELONDS_ARM9TRACE_HI=0x020D2440 MELONDS_ARM9TRACE_N=400 MELONDS_ARM9TRACE_EXIT=1 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"
```

Evidence (`$S` = scratch): `p5d_mel_loop.txt` (loop enter/leave/post),
`p5d_mel_d24.txt` (Thumb strides at 0x020D241C), `p5d_smc2.err` (the three
ARM-state decodes that name the barrier), `p5d_mem96.txt` (RAM at
0x0215AC18/0x0215AF70 proving the ARM code the odd LR re-entered),
`p5d_fix13.err` (13.1M path with the fix), `p5d_fix2_*.err` (halt chain),
`p5d_tail.out` (native post-wake loop path), `p5d_sh.*.ppm`,
`p5d_verify.log`.

Changed in this phase: `tools/nnk_smc.inc` (interworking carry on LDM/POP,
even BLX(reg) LR), `nnktool/src/decoder_regression_tests.rs` (class-7 test +
class-4 assertion corrected against melonDS), `tools/melonds-loopwatch.patch`
artefact + `src/ARM.cpp` in the melonDS checkout: `MELONDS_LOOPWATCH_LO/HI`
env window, `PHASE5D.md`.
