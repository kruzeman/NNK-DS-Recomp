# Phase-5K — barrier F fixed: the compiled one-register `pop {pc}` forced the Thumb tag on ARM code; sweep G (compiled `bx`/`blx` state) and H (exact per-case state marker)

Follows Phase-5J (merged `a73405c`).  The named barrier was **F**: `sp = 0` in
the `0x0207EA20` frame (42,038,854 steps, halt `dispatch to untranslated pc`).

**Headline:** barrier F is the *Phase-5J class 13/14 bug on the emitter side*,
not in the frame at all — the frozen image's single-register
`nnk_smc_note_branch(_t | 1u)` applies the Thumb `T_POP` rule
(`if (Num==1) pc |= 0x1`) to the **ARM `A_LDM`** form, whose rule is the
opposite (`if (Num==1) pc &= ~0x1`).  One ARM epilogue therefore returned into an
ARM function *tagged Thumb*; the live re-decoder ran that function as Thumb,
walked off into the guest's own string table, and an ASCII word decoded as an
ARM `ldmia` with `r13` in its register list loaded 0 into `sp`.

With F fixed and G/H swept the run **no longer halts on a bad PC**:

| check | Phase-5J | now (5K) |
|---|---|---|
| run end | 42,038,854 steps, halt `0x0207EA20` | **80,000,001 = NNK_STEP_LIMIT**, no halt |
| distinct guest PCs | 18,252 | **19,470** |
| halt_pc | `0x0207EA20` (untranslated) | `0x02009118` (SDK idle/WFI helper) |
| frames presented | 81 (uniform) | **148** (fade, then black — see §4) |

## 1. Barrier F — the first differing sp write (measured)

Instrumented sp-write watch (`NNK_SPWATCH`, `NNK_SPWATCH_LO`) on the branch
code, run to the old halt:

```
spwatch step=42038800 bypc=0x0207E94C old=0x027E3A78 new=0x00000000 lr=0x00000000 r0=0x00000000
```

That is the **first** `r13` change to 0 in the frame.  Walking backwards with
`NNK_TRACE_FROM` + `NNK_SMC_TRACE` (both live decodes and their states):

```
step=42038693 pc=0x02043C04  (compiled)                     -> 0x0207D2D8
step=42038694 pc=0x0207D2D8 word=0xE59A01AC state=thumb     -> 0x0207D2DA
step=42038695 pc=0x0207D2DA word=0x1006E59A state=thumb     -> 0x0207D612
...
step=42038725 pc=0x0207E502 word=0xFF31E591 state=thumb     -> 0x0207E828
step=42038726 pc=0x0207E828 word=0x7265566C state=arm       -> 0x0207E82C
...
step=42038800 pc=0x0207E94C word=0x49552F61 state=arm       -> ...
```

`0x02043C04` is the compiled ARM epilogue whose body was (frozen image):

```c
/* 0x02043C04: pop {pc} */
{ uint32_t _base = r[13];
  uint32_t _a = _base;
  uint32_t _t = ld32(_a + 0u);
  pc = _t & ~1u;
  nnk_smc_note_branch(_t | 1u);      /* <-- Thumb tag on an ARM pop */
  r[13] = _base + 4u;
}
```

The popped word is the even (ARM) return address `0x0207D2D8`; `| 1u` makes it
`0x0207D2D9`, so the next live decode resolves the target as Thumb.
`0x0207D2D8` is ARM code (`E59A01AC` = `ldr r0,[r10,#0x1AC]`), so the guest ran
ARM as Thumb down `0x0207E828..` — the region is a **string table**
(`/data/BattleGameOver/Re...`, `/data/UI/Common/Cursor`, dumped from
`NNK_DUMP_MEM`).  The ASCII word at `0x0207E94C` is `0x49552F61` ("a/UI");
decoded as ARM it is an `ldmia` (bits 27..25 = 100, L=1, list = 0x2F61 includes
bit 13), so it loads `r13` from the string bytes → `sp = 0`; the frame then ran
on to `0x0207EA20`, whose word is `0x2E4F474F` ("OGO.") and decodes as neither
state cleanly → halt.

**Class:** melonDS `A_LDM` (src/ARMInterpreter_LoadStore.cpp:436)
`if (cpu->Num == 1) pc &= ~0x1;` vs `T_POP` (:745) `if (cpu->Num==1) pc |= 0x1;`
— one-register lists take the FORM's forced tag, not a Thumb tag.

**Fix (measured scope):** `tools/p5k_note_branch.py` rewrites every frozen-image
single-register LDM-pc note from the **compiled generation word** at that
address (ARM `cond 100 P U S W L Rn list` with pc vs Thumb `0xBCxx/0xBDxx` —
they cannot collide, and an address whose word matches both or neither is left
untouched, never guessed): **25 sites corrected** (22 `0xE8BD8000`, 3
conditional `0x08BD8000`), 3 ITCM sites undecidable (their bytes are not in the
`NNK_SMC_IMAGE` segments).  The emitter (`nnktool/src/recompile.rs`,
`Kind::LdmStm`) already had this right since Phase-5J; the *patcher* had copied
the old Thumb rule.

## 2. Sweep G — compiled `bx`/`blx Rm` carried no state

`Kind::Bx` emitted `pc = _t & ~1u;` and dropped bit0, for **both** the
register-indirect form and the `bx lr` return form — the same class as F, one
arm of the match.  melonDS `A_BX`/`A_BLX_REG`/`T_BX`/`T_BLX` all `JumpTo(Rm)`,
i.e. bit0 of the register decides the state.

* Emitter: both arms now push `nnk_smc_note_branch(_t);` (raw target) next to
  the `pc = _t & ~1u;` assignment.
* Frozen image: `tools/p5k_note_branch.py` splices the same text into the
  **1,402** `{ uint32_t _t = r[..]; pc = _t & ~1u; }` sites (LDR-pc sites are
  deliberately left alone: melonDS `A_LDR0` masks bit0 and does not interwork,
  matching the live decoder's Phase-5D behaviour).

## 3. Sweep H — exact per-case state marker (implemented)

`nnk_pc_is_thumb()` was built by `tools/split_dispatch.py` from the
"emitter-written fall-through is addr+2" heuristic, which is **wrong for every
case that ends in a branch** (every `pop {..,pc}`/`bx`/`b` case) — precisely the
cases whose targets the live re-decoder had to guess (barrier class E in
Phase-5J, F here).

Implemented (minimal, sound):

* `nnktool/src/recompile.rs`: `EmitOutcome` carries `thumb` (the state the body
  was emitted FROM) and `case_string()` stamps every case label
  `case 0x0207D2D8u: /* nnk_state=arm */`.
* `tools/split_dispatch.py`: the marker is authoritative for that case;
  the addr+2 heuristic is kept only for markerless (frozen) images and the
  count of marker-classified cases is reported in the build log.

The full design (for the next regeneration): the marker makes `nnk_pc_is_thumb`
exact for every compiled PC, so the runtime resolution chain in
`nnk_smc_try`/`nnk_smc_exec_any` (marker → carried branch state → register tag →
sticky) only has to guess for PCs the recompiler never rooted, and those are the
ones whose state the caller's transfer really does not carry.  The frozen image
under test stays on the heuristic because it has no markers (and 3 of its 73k
cases cannot be classified from the available image segments) — that limitation
is why §1's fix has to be a per-site encoding decision rather than a map fix.

## 4. Result on the branch code

```
$ cd nnktool && cargo test
test result: ok. 28 passed; 0 failed        # 25 -> 28 (classes F/G/H)

$ SKIP_MELONDS=1 sh tools/verify_phase2a.sh
1) strict common prefix        : 1000 / 1000 melonDS PCs (100.0%)
   melonDS trace fully reproduced
2) window-filtered prefix      : 1000 / 1000 (window 0x02000000..0x02400000)
3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
rc=0

run: 42,038,854 (halt)  ->  80,000,001 (NNK_STEP_LIMIT), 19,470 distinct PCs
     halt_pc=0x02009118 halt="step budget exhausted"
     frames: 148 presented
```

Sweep H was also verified end-to-end on a real regeneration: the mono image
`work/phase2a/arm9_all.c` now carries 72,959 case markers (72,908 arm / 51
thumb) and 3,693 `nnk_smc_note_branch(_t);` sites, and a marker stamped on one
case that the addr+2 heuristic called Thumb (`0x020D128C`) removes it from
`nnk_smc_map.h`, i.e. the marker is authoritative and the heuristic is only the
fallback for markerless images.

Reproduce:

```sh
ln -sfn /home/hermes/NiNoKuni/work work
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  python3 tools/p5k_note_branch.py work/phase3a/arm9_thumb_ovl_itcm_p5j.c \
          work/phase3a/arm9_thumb_ovl_itcm_p5k.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5k.c SPLIT_DIR=work/split/p5k \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5k1
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=80000000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  /home/hermes/NiNoKuni/out/split/p5k1 work/phase3a/arm9_stored_padded.bin
```

Frames (black → white fade → grey fade → black) are captured with
`NNK_GX_SHOT`/`NNK_GX_SHOT_EVERY=1`; the two PNGs saved to
`/home/hermes/NiNoKuni/shots/p5k_f00090_top.png` (mid-fade) and
`p5k_f00147_top.png` (end state) are **uniform**, i.e. the picture is still not
there.

## 5. Named follow-up barrier (G-barrier: the guest parks in the SDK idle loop)

The guest no longer faults; it **parks**.  A trace window at the end of the run
is a five-PC cycle:

```
0x02007134 -> 0x02009118 -> 0x0200911c -> 0x02009120 -> 0x02007138 -> 0x02007134
```

which is ARM `0x0200712C: push {r3,lr}` / `0x02007134: bl 0x02009118` (the WFI
helper) / `0x02007138: b 0x02007134` — an unbounded "wait for the wake that
matters" loop.  State at that point (`NNK_WFI_PROBE`, step 79,000,003):

```
ime=1 ie=0x00040019 if=0x00080000 cpsr=1F
cardnext=(none) romcnt=0x27416000 spicnt=0xC000 dma2=0x05000001 dispstat=0x0008
```

i.e. IRQs are enabled and delivered (`irq enter step=79369452 resume=0x0200911C
vector=0x01FF8000`), VBlank is enabled, but IF bit 19 (card transfer done) is
**permanently latched** while the card model has **no transfer scheduled**
(`cardnext = ~0`), so the WFI returns immediately forever and the flag the guest
actually waits on never changes.  Frames keep being presented (VBlank compositor)
but nothing is drawn after the boot fade.

**Follow-up barrier G: the parked idle loop at `0x02007134`/`0x02007138` — the
card/ARM7 event that ends the boot is never produced; start from the latched
IF bit 19 with `cardnext = ~0` and the ARM7 request the guest issued last.**
