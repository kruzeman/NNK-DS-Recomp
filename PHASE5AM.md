# Phase-5AM — the 0x449F "count" is an instruction word: Thumb R15 reads must not be word-aligned

Follows Phase-5AL (`9e135bc`, merged as `0268b1f`).  5AL proved melonDS reaches
the stream-state guard `0x02044868` and always returns normally, and located the
first heap difference at allocation **#412**: our runtime ran an ov17 loop that
allocates 0x1A8-byte payloads (call site `0x020E1B6B`), melonDS allocated a
0x54-byte file record instead.

**5AM answers the open question: `[0x06006250] = 0x449F` is not data at all — it
is the encoding of the `add pc, r3` instruction at `0x020DDEA2`, read back as a
jump-table entry because the live Thumb re-decoder word-aligned the PC.**
The guest therefore jumped from a switch into an unrelated ov17 constructor,
which stored its bogus 4th argument (`0x449F`) into `[0x06006000+0x250]` —
i.e. into VRAM — and then looped `0x449F` times asking the guest heap for 0x1A8
bytes.  With the PC read fixed the switch takes its real case, the constructor
is never reached, the 0x1A8 loop is gone and allocations **#412 onward match
melonDS**.  The guest now runs to step **559,830,681** (was 334,058,027) and
stops on a *different*, named barrier.  The "Skip the cutscene?" dialog is NOT
restored yet; §5 says exactly where the movie path still differs.

## 1. Who writes `[0x06006250]`

The only writer in the whole run is one guest store, and it is inside the
constructor pair the accidental jump landed in:

```
ov17 (base 0x020D0A00), Thumb            instruction
0x020E2344  derived ctor: push {r3,r4,r5,lr}; bl 0x020E1AC4; *this = vtable
0x020E1AC4  base ctor:   push {r3,r4,r5,r6,r7,lr}; sub sp,#32
0x020E1ACE  str  r0,[r5,#0]              ; vtable
0x020E1AEC..0x020E1AF8   zeroes [this+0x11C..0x12C] (incl. +0x124)
0x020E1AFE  str  r0,[r5,#0x248]
0x020E1B20  str  r6,[r5,#0x250]          ; <-- the writer: [0x06006250] = r6
0x020E1B24  str  r1,[r5,#0x260]
0x020E1B26  ldr  r4,[r5,#4]; ldr r4,[r4,#0x24]; blx r4   ; class init
0x020E1B38  ldr  r0,[r5,#0x250]          ; re-read
0x020E1B3E  ble  0x020E1BBE              ; <= 0 -> skip the loop
0x020E1B64  movs r0,#0x25; lsls r0,#4; subs r0,#0xA8    ; 0x1A8
0x020E1B6A  blx  0x020462DC              ; guest heap
```

Measured write table (native debug probe `NNK_DEBUG_AT_PC=0x020E1AC4`,
`NNK_DEBUG_MEM=0x027E3A00:0x80`; the store itself in the 5AL gate log):

| emulator | writer PC | size | value | step / ts |
|---|---|---|---|---|
| native (before this fix) | `0x020E1B20` (`str r6,[r5,#0x250]`, r5=0x06006000) | 4 | **0x0000449F** | step 333,970,241 |
| native (after this fix)  | — the constructor is never entered — | — | — | — |
| melonDS 1.1 | — `0x020E1B20` never executes (StreamProbe, 0 hits / 2600 frames) — | — | — | — |

The value comes straight from the caller: at `0x020E2344` the runtime had
`r0=0x06006000 r1=0 r2=0x20 r3=0x449F`, and `0x449F` is the halfword at
`0x020DDEA2` — the guest's own `add pc, r3`.  The store is an ordinary guest
`str` through the runtime's `st32` path (the value in memory equals the
constructing instruction word, and it is the constructor's own 4th argument,
so nothing else can have produced it); `0x06006000` is the `this` pointer the
movie code loaded from `[player+0x40]`, not a VRAM shadow or VRAMCNT effect.
A dedicated store-watch over `0x06006000..0x06008000` (guest `st8/16/32` **and**
the Phase-5X HLE plane copy, as in `tools/p5w_store_probe_splice.py`) was not
run in this phase — the writer is established from the trace + disassembly and
from melonDS executing neither the store nor the constructor.

## 2. The root cause: a Thumb switch whose table is 2 mod 4

GCC's Thumb switch idiom at `0x020DDE9A` (inside ov17's chunk dispatcher
`0x020DDE70`, called from ov6 `0x0207A238` = `blx 0x20dde70`):

```
0x020DDE90  ldr  r3,[r4,#0x48]          ; sub-state, 0..3 (cmp/bhi guard at DE94/DE96)
0x020DDE98  adds r3, r3
0x020DDE9A  add  r3, pc                 ; base = (this instr) + 4 = 0x020DDE9E
0x020DDE9C  ldrh r3, [r3, #6]           ; entry = [0x020DDEA4 + state*2]
0x020DDEA0  asrs r3, r3, #16            ; sign-extend
0x020DDEA2  add  pc, r3                 ; pc = (this instr) + 4 + entry
0x020DDEA4  .hword 0x0006, 0x0008, 0x000A, 0x000C     ; the table
0x020DDEAC  b    0x020DDEB4             ; case 0 body
```

The table is `0x020DDEA4`, i.e. the base `0x020DDE9E` is **2 mod 4**.  The live
re-decoder (`tools/nnk_smc.inc`, `smc_reg`) returned `(pc + 4) & ~3` for a Thumb
R15 read, so:

* `add r3, pc` gave `0x020DDE9C` → `ldrh` read `[0x020DDEA2]` = **0x449F** (the
  `add pc, r3` instruction word) instead of entry 0 (`0x0006`);
* `add pc, r3` added to `0x020DDEA4` instead of `0x020DDEA6`, so the same
  value produced `0x020E2342` — the byte before the unrelated derived
  constructor at `0x020E2344`.

Measured PC trace at the site (native, `NNK_TRACE_FROM`, 333,957,200):

```
before:  ... 0x020dde9a 0x020dde9c 0x020dde9e 0x020ddea0 0x020ddea2 -> 0x020e2342 0x020e2344
after:   ... 0x020dde9a 0x020dde9c 0x020dde9e 0x020ddea0 0x020ddea2 -> 0x020ddeac 0x020ddeb4
```

melonDS is the reference and reads the unaligned value: in `CPUExecute`
(`src/ARM.cpp`) `R[15]` *is* `instruction address + 4` while a Thumb instruction
runs ("Thumb state: R[15] reads as instruction address + 4 here"), and
`T_ADD_HIREG` adds `R[15]` with no mask.  Its `pop {r4,r5,pc}` at `0x020DDE48`
also matches: with the unaligned base, case 0 lands on `0x020DDEAC`
(`b 0x020DDEB4`), not on the `movs r4,r1` word at `0x020DDEAA` (which would
destroy the struct pointer that `0x020DDEBA` dereferences).

## 3. The fix

* `tools/nnk_smc.inc` — `smc_reg()`: a Thumb R15 read is `(pc + 4)`, **not**
  `(pc + 4) & ~3`.  Formats 6 (LDR literal) and 12 (ADR) keep their explicit
  `(pc + 4u) & ~3u`, which is where the architecture really does force the
  word-aligned form.
* `nnktool/src/recompile.rs` — the *static* emitter had the same bug on the
  destination-side operand of `Kind::Dp` (`reg_mode` → `pc_lit`).  It now uses
  `reg_c_t` (Thumb: `(pc + 4u)`) like the source operand.  This site is not a
  static case (ov17 executes through the live re-decoder), so the C fix is the
  one that moved the guest; the Rust fix closes the class for the next image
  generation.
* Regression class 48:
  `phase_5am_thumb_format5_reads_the_unaligned_pc_in_a_switch_table` — decodes
  `0x449F`, asserts the emitted body adds to `(pc + 4u)` and not `0x020DDEA4u`,
  asserts `smc_reg` carries no `& ~3u`, and pins the arithmetic both ways
  (`0x020DDE9E + 6 = 0x020DDEA4`, case 0 = `0x020DDEAC`, and the old mask
  reproduces the traced `0x020E2342`).  `cargo test` = **52 passed**.

## 4. Result: the 0x1A8 loop is gone and the heap matches melonDS

Same scripted session (`tools/nnk_operator_session.nnk`), allocator probe
`NNK_DEBUG_AT_PC=0x0202A13C`:

```
index  native (before)                native (after)                 melonDS (5AL)
#411   0x3D8  lr=0x020DDA20          0x3D8  (step 333,870,729)      0x3D8  ts=339,990,062
#412   0x1A8  lr=0x020E1B6B  <--     0x54   (step 333,994,875)      0x54   ts=340,115,333
#413   0x1A8  lr=0x020E1B6B  <--     0x228  (step 333,999,419)      0x228  ts=340,121,063
#414   0x1A8  lr=0x020E1B6B  <--     0x81AE (step 334,009,681)      0x81AE ts=340,137,301
#415   0x1A8  lr=0x020E1B6B  <--     0xC030 (step 334,482,131)      0xC030 ts=340,923,159
#416   ...                           0x624                          0x624
```

65 iterations of 0x1A8 → **zero**; the four allocations after #411 are
byte-for-byte the melonDS sizes, and the run presents 1006 frames (was 602).

## 5. The new barrier (honest) and the skip dialog

* **Step number past the old barrier: 559,830,681** vs the old halt at
  334,058,027 (the `0x020090D8` shutdown from `[obj+0x124]==0x80808080`).  The
  stream guard is no longer fed a poisoned object because the object is no
  longer created at all.
* New halt: `halt=dispatch to untranslated pc pc=0x020DEE48` (ov17 offset
  0xE448, the `pop {r4,r5,pc}` word of the function at `0x020DEE24`).  The
  dispatch has no case for it and the live re-decoder declines; the caller chain
  must be captured with `NNK_TRACE_FROM=559829000` (same method as §2).
* **"Skip the cutscene? Yes/No" is still NOT reached.**  With the same input the
  runtime now loads the movie's chunk records (the melonDS #412/#413/#414/#415
  sequence) and keeps running, but the last frames before the new halt show the
  fan-translation notice screen, not the dialog.  Measured reason for the
  remaining gap: melonDS **never executes ov17's chunk dispatcher at all** in the
  session — StreamProbe over 2600 frames logs **0 hits** at `0x020DDE70`,
  `0x020DDE90`, `0x020DDEA2`, `0x020DDEAC` and `0x020E2344`, while our runtime
  reaches it from ov6 `0x0207A238` (`blx 0x20dde70`, part of the
  `0x0207A1A0..0x0207A250` movie-start block that also calls `0x020FC380`,
  `0x020DD9D8`, `0x020DEE78`, `0x020DE26C`).  So the next target is that ov6
  block's entry condition, not the ov17 loop: probe `0x0207A1A0/0x0207A238` in
  melonDS and compare with `NNK_DEBUG_AT_PC=0x0207A238` (the same instrument run
  that produced this phase's call chain).

## 6. Evidence

```
work/phase5am/native_trace3.txt   the accidental jump (dde98..ddea2 -> 0x020E2342)
work/phase5am/native_trace4.txt   the fixed path (ddea2 -> 0x020DDEAC)
work/phase5am/ctor_entry.txt      [0x06006250]-writer chain: r3=0x449F, lr=0x020E2351
work/phase5am/ctor2_entry.txt     derived ctor entry, lr=0x0207A23C (ov6 0x0207A238)
work/phase5am/run_fix1.err        allocator #400..#470 (the matching table) + halt
work/phase5am/shots/              1006 frames, NNK_GX_SHOT_EVERY=10
work/phase5am/probe1.probe.txt    melonDS: 0 hits at the ctor/gate/store PCs
work/phase5am/probe2.probe.txt    melonDS: 0 hits at the ov17 dispatcher PCs
```

Note: no game bytes were committed; the only guest constants in the test are a
single Thumb instruction encoding (`0x449F`) and its measured address.
