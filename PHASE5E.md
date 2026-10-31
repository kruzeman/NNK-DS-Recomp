# Phase-5E — the 0x0204C784 pop is not a stack write: Thumb BLX(1) PC-base alignment (class 8)

Follows Phase-5D (merged `e89642e`).  Scope: root-cause the `0x0204C784`
`pop {r3,r4,r5,r6,pc}` returning `0x00000002`, sweep the `0x020D2xxx`
dispatch-helper link/interworking family in one pass, and reach the picture if
the family allowed it.

**Result: the pop's frame was never written — the "pop content" is a
*downstream symptom*, not a stack-write class.  The first differing value in
the run is the BRANCH TARGET at `0x020D2426`: native `0x0204C3F6`, melonDS
`0x0204C3F4`.  Class: ARMv5T **Thumb `BLX(1)` (immediate) bases its offset on
`Align(PC,4)`, not `PC`** — the live C re-decoder used `pc + 4` for both
`BL(1)` and `BLX(1)`, so every BLX at an address `≡ 2 (mod 4)` landed 2 bytes
past its target.  Fixed in `tools/nnk_smc.inc` and `nnktool/src/decode.rs` +
3 cargo tests (suite 18 → 21).  The run now advances **20.8M steps past the old
halt** (39,238,304 → 60,000,001, budget) into the real post-dispatch drain and
parks in the ARM9 WFI idle loop, the new measured barrier.  Frames are still
uniform white (the content switch is behind the idle-loop barrier).

## 0. The pop: measured, not assumed

`NNK_ST_WATCH="0x027E3B40:0x027E3B68"` (the Phase-5B store watcher lineage)
over the whole run recorded **607** stores in the pop's stack window.  The last
word written to the frame the pop reads (`r13 = 0x027E3B48` at the halt,
pc slot at `0x027E3B58`) is:

```
st4 addr=0x027E3B48 val=0x02143158 pc=0x020D22D8 step=39148898
st4 addr=0x027E3B4C val=0x027E0080 pc=0x020D22D8 step=39148898
st4 addr=0x027E3B50 val=0x00000000 pc=0x020D22D8 step=39148898
st4 addr=0x027E3B54 val=0x020D1995 pc=0x020D22D8 step=39148898
                                              (p5e_stw.txt, 607 entries)
```

— i.e. **89,406 steps before** the pre-fix pop at step 39,238,304, from a
*different* function's prologue.  And `NNK_DEBUG_AT_PC=0x0204C730` logged
**0 hits** in the whole run: the prologue `push {r3,r4,r5,r6,lr}` at
`0x0204C730` never executed, so nothing ever pushed a link for the pop to
restore.  The legacy runtime nonetheless pushed `r3=0x020D24A3`,
`r4=0x0213A3D0`, `r5=0x020D23E3`, `r6=1` before the pop: those are the
registers of the *stale* frame's owner, not this function's.

The halt trace names the real entry (`p5e_tr2.out`, 706 PCs from step
39,237,600):

```
39238259 0x0204c42a
39238260 0x0204c42e          <- inside the ARM `ldr r1,[r1,#4]` at 0x0204C42C
39238261 0x0204c754          <- ARM body of 0x0204C730 entered at +0x24
```

`0x0204C754` is the *middle* of the ARM function at `0x0204C730`
(`push {r3,r4,r5,r6,lr}; sub sp,#12; ...; pop {r3,r4,r5,r6,pc}`).  The guest
ran it in **Thumb state**, so the epilogue word `E8BD8078` also decodes as
the Thumb `pop {r3,r4,r5,r6,pc}` (`BD78`) — a valid decode, hence no halt
inside the function; it popped the stale stack word `0x00000002` and the
dispatch loop halted on `pc=0x00000002`.

**There is no first differing stack write at the pop.**  The first differing
write in the run is the PC write at `0x020D2426` (next section).

## 1. The first differing value: a Thumb BLX(1) target

Native, pre-fix (`p5e_tr2_num.txt`):

```
39238245 0x020d2424
39238246 0x020d2426       <- `f779 efe6` : blx 0x0204C3F4
39238247 0x0204c3f6       <- 2 bytes INTO the ARM function body
39238248 0x0204c3fa
39238249 0x0204c3fe
39238250 0x0204c402       ... Thumb walk through ARM code
```

melonDS, same site (`MELONDS_ARM9TRACE` window 0x0204C3C0..0x0204C440,
`p5e_mel_c3.txt`):

```
0x0204c3f4
0x0204c3f8
0x0204c3fc
0x0204c400
0x0204c404
0x0204c408
0x0204c40c
```

4-byte ARM strides from the real prologue: melonDS executes `0x0204C3F4`.
binutils agrees: `f779 efe6` at `0x020D2426` disassembles as
`blx 0x204c3f4` (and the assembler round-trip reproduces the exact bytes —
see §2).

## 2. The class: ARMv5T BL(1) vs BLX(1) PC base

| form | suffix | target | verified by |
|---|---|---|---|
| `BL(1)`  | `11111` (bit12=1) | `PC + offset`, PC = addr+4 | assembler round-trip `f7ff ff8d` at 0xE6 → target 4 |
| `BLX(1)` | `11101` (bit12=0) | `Align(PC,4) + offset`     | assembler round-trip `f779 efe6` at 0x86036 → target 4 |

`BLX(1)` calls ARM code and its H bit is forced 0, hence the aligned base.
The live C re-decoder used `(pc + 4u + o) & ~1u` for both forms.  Scan of the
`0x020C0000` overlay (`p5e_count.py`, binutils as authority):

* **2,053** `BLX(imm)` sites were mis-targeted by 2 bytes; **95** of them live
  in the `0x020D2000..0x020D2FFF` helper band.
* **0** `BL(imm)` sites are mis-targeted — the BL arm of the fix is a no-op
  (1,286 BLs sit at addr ≡ 2 mod 4; 28 in the helper band).

## 3. The fix

`tools/nnk_smc.inc` (`smc_exec_thumb`, Thumb BL/BLX arm):

```c
uint32_t base = (hw2 & 0x1000u) ? (pc + 4u) : ((pc + 4u) & ~3u);
*npc = (base + (uint32_t)o) & ~1u;
```

`nnktool/src/decode.rs` (`decode_thumb`): same base selection before adding
the J-form offset (the old `& !3` on the BLX result was only accidentally
right when the offset happened to be a multiple of 4).

Three tests added (`nnktool/src/decoder_regression_tests.rs`, class 8):
`thumb_blx_immediate_aligns_the_pc_base` (F779 EFE6 @ 0x020D2426 →
`0x0204C3F4`), `thumb_bl_immediate_keeps_the_unaligned_pc_base`
(F7FF FF8D @ 0x020D136E → `0x020D128C`, the negative guard) and
`runtime_smc_redecoder_aligns_the_thumb_blx_base` (C-mirror source lock).

## 4. The dispatch-helper family sweep (`0x020D2xxx`, measured)

Forms executed in the band in this run, with their status:

| site | form | status |
|---|---|---|
| `0x020D22D8` | `push {r3,r4,r5,r6,lr}` (Thumb fmt 14) | pushes 5 words, `lr=0x020D1995` (store watcher, step 39,148,898) |
| `0x020D23E0` | `blx r2` (vtable dispatch, Thumb fmt 3) | Phase-4Y/5D semantics; returns to `0x020D23E3` |
| `0x020D2414` | `push {r3,r4,r5,lr}` | fine |
| `0x020D2418` | `blx 0x020E7968` (addr ≡ 0 mod 4) | unchanged and correct before **and** after the fix |
| `0x020D2426` | `blx 0x0204C3F4` (addr ≡ 2 mod 4) | **fixed** (was 0x0204C3F6) |
| `0x020D2466` | `pop {r3,r4,r5,pc}` | Phase-5A class; returns correctly |
| `0x020D24xx` | 28 BL(imm) / 224 BLX(imm) in band | all now resolve like binutils |

No further link/interworking divergence exists in this family: with the one
fix the run walks `0x020D2424 → 0x020D2426 → 0x0204C3F4 → … → 0x020D242A`
(the ARM helper's return site) exactly like melonDS and continues through the
whole `0x0204CExx/0x0204D6xx/0x020D25xx` drain to `0x020D3250`.

## 5. Verification (branch code)

| run | steps | distinct_pcs | frames | halt |
|---|---|---|---|---|
| before (Phase-5D) | 39,238,304 | 16,441 | 76 | `0x0204C784` pop → `pc=2` |
| after (this branch) | **60,000,001** (budget) | **16,447** | **113** | WFI idle loop `0x02009110` (§7) |
| after, 46M-step trace run | 46,000,001 | 16,447 | 88 | same idle loop |

Post-fix trace at the fixed site (`p5e_trfix.out`):

```
39238245 0x020d2424
39238246 0x020d2426
39238247 0x0204c3f4      <- ARM prologue, like melonDS
39238248 0x0204c3f8
39238253 0x0204c40c
39238254 0x0204c440
...
39238276 0x020d242a      <- back to the helper, like melonDS
```

`NNK_DEBUG_AT_PC=0x0204C784` on the fixed run: **0 hits** — the ARM epilogue
is never executed as Thumb any more.

`cargo test` in `nnktool`: **21 passed** (18 previous + 3 class-8).
`SKIP_MELONDS=1 bash tools/verify_phase2a.sh` on this branch: **PASS**
(strict common prefix 1000/1000 melonDS PCs, 100.0%).

## 6. The picture (honest)

Still uniform white.  `p5e_fix_sh.00080.top/bot.ppm` are single-colour
(`0xFFFFFF`, 1 distinct colour, 256x192); PNGs (590 bytes) at
`/home/hermes/NiNoKuni/shots/p5e_f80_top.png` and `..._bot.png`.  The
content switch (melonDS frame ~102) is behind the idle-loop barrier below.

## 7. The next barrier (named, measured)

Step 39,238,902 (654 steps after the fixed BLX) the guest enters the ARM9 WFI
idle loop:

```
0x02009108  push {r3, lr}
0x0200910C  bl   0x02008D20
0x02009110  bl   0x02009118
0x02009114  b    0x02009110
0x02009118  mov  r0, #0
0x0200911C  mcr  p15, 0, r0, c7, c0, {4}   ; Wait For Interrupt
0x02009120  bx   lr
```

Native spins there for the rest of the run (799,820 iterations inside a
4M-line trace).  **Ground truth** (`MELONDS_LOOPWATCH` window
`0x02009110..0x02009114`, `p5e_mel_wfi.txt`): melonDS wakes the loop every
~21,700 master cycles:

```
irqwake ts=56548252 pc9=0x0200911C ie=0x000C0009 if=0x00280000 ime=1 enabled=0x00080000 iters=0
irqwake ts=56569952 ... enabled=0x00080000
irqwake ts=56591652 ... enabled=0x00080000   (cadence 21,700)
```

— the wake source is **IF/IE bit 19 (`0x00080000`, game-card IRQ)**, delivered
32,309,950 master cycles after the last entry in the runtime's scripted IRQ
table (`irq_raise_log` ends at step 9,097,800, 3 `irq_trig_log` entries).  The
runtime therefore never raises the interrupt and the guest never leaves the
idle loop, which is why frames stay blank.

**Follow-up barrier: IRQ delivery beyond the scripted boot window** — extend
the ARM9 IRQ model (or regenerate `irq_raise_log` from a longer melonDS IRQ
capture, `tools/add_irqtrace.py` + `tools/gen_irq_script.py`) so IF bit 19 is
raised at the ~21.7k-cycle cadence the loop-watch measured, then verify the
handler at the user vector runs and the frame-102 content appears.

## 8. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/link-sweep     # work -> ../work symlink
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  bash tools/build_split.sh out/split/p5e_fix
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=60000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/p5e_fix work/phase3a/arm9_stored_padded.bin
(cd nnktool && cargo test)                        # 21 tests
SKIP_MELONDS=1 bash tools/verify_phase2a.sh       # PASS
# store-watch image (pop frame measurement):
python3 tools/p5b_store_watch.py work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  work/phase3a/arm9_p5e_watch.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_p5e_watch.c bash tools/build_split.sh out/split/p5e_watch
NNK_ST_WATCH="0x027E3B40:0x027E3B68" NNK_ST_WATCH_PATH=$S/p5e_stw.txt \
  NNK_ST_WATCH_MAX=3000000 NNK_HALT_AT_PC=0x0204C784 ...
# melonDS ground truth:
MELONDS_ARM9TRACE_PATH=$S/p5e_mel_c3.txt MELONDS_ARM9TRACE_LO=0x0204C3C0 \
  MELONDS_ARM9TRACE_HI=0x0204C440 MELONDS_ARM9TRACE_N=400 MELONDS_ARM9TRACE_EXIT=1 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"
MELONDS_LOOPWATCH_PATH=$S/p5e_mel_wfi.txt MELONDS_LOOPWATCH_LO=0x02009110 \
  MELONDS_LOOPWATCH_HI=0x02009114 MELONDS_LOOPWATCH_AFTER=40 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "/home/hermes/NiNoKuni/Ni No Kuni.nds"
```

Evidence (`$S` = scratch): `p5e_stw.txt` (607 frame stores), `p5e_tr2_num.txt`
(pre-fix misaligned path), `p5e_trfix.out` (post-fix path through 0x0204C3F4),
`p5e_mel_c3.txt` (melonDS ARM strides at 0x0204C3F4), `p5e_mel_wfi.txt`
(irqwake cadence), `p5e_fix.err` / `p5e_fix.out` (60M run), `p5e_fix_sh.*.ppm`,
`p5e_verify.log`, assembler round-trip `p5e_rt1.s`/`p5e_rt2.s`.

Changed in this phase: `tools/nnk_smc.inc` (BLX(1) PC base),
`nnktool/src/decode.rs` (same class in the Rust decoder),
`nnktool/src/decoder_regression_tests.rs` (+3 tests), `PHASE5E.md`.
