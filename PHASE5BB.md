# Phase-5BB — the cutscene-skip press: the guest resumed ARM at a Thumb address

Follows Phase-5BA (master `67cefc6`).  Branch `wt/t_3fa854c8`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5bb/nnk_runner`
(+ prelude splice `5bb_irq_state`, manifest 56/56).

**Result: the divergent transfer is named, its cause is fixed and verified, and
the guest now runs the in-game cutscene movie for ~900 display frames with no
halt.**  The "Skip the cutscene?" box itself was NOT captured inside the
remaining budget; §6 names exactly what is still missing and where to look.

## 1. Reproduction (the card's own script shape, corrected timing)

* State: the Phase-5BA frame-4201 savestate (`st2350.nnks`, build `0x0768D9EB`)
  + `skip_prompt2.nnk` (B at 120, A at 4300, A `New Game` at 4550, then A held
  10 frames at 5400/5600/5800/6000) reproduces the 5BA crash **exactly**:
  halt in the heap walk, SWI `#0x65` (`0xEF656C00`) at `0x02270CDC`.
* Script, state and evidence are in `work/repro_5bb/` (not committed):
  `cutscene_press.nnk` (cold-boot shape), `skip_prompt2.nnk`, `st2350.nnks`,
  `st3400.nnks` (frame 6076, inside the movie, healthy).
* **The card's press window was wrong**: a press at frame 5200/5300 does
  nothing (the stream-2 movie has not started); the divergent transfer is
  reproduced by the press at **5400** and happens ~42M steps later, at step
  3,064,069,170, frame 5477.

## 2. The transfer, named

Trace + `NNK_DEBUG_AT_PC` (registers at every hit) + a new `NNK_EXC_TRACE=1`
exception-return log:

```
step 3064069170  pc=0x0200729C  r14=0x02137A96  r13=0x0228EEDC  (r0=0x0003AA04)
  exc_return target=0x02137A92 spsr=0x4000001F thumb=0 mode=31 (SYS)
  then 0x02137A92, 0x02137A96, 0x02137A9A, 0x02137A9E, 0x02137AA2  (stride 4 = ARM)
  then 0x02267068 ... 0x02270CDC  HALFWORD SWI #0x65 -> halt
```

* `0x0200729C` is the SDK's **RestoreContext** tail (`0x02007284 ldr r1,[r0],#4
  / 0x02007288 msr SPSR_fsxc,r1 / ldr sp / ldr lr / ldm ..^ / 0x0200729C
  subs pc,lr,#4`).  Its operands: `r14` = the thread's saved `lr` =
  `0x02137A96`, so the resumed pc is `lr-4 = 0x02137A92`.
* `0x02137A92` is **Thumb** code in overlay 17 (offset `0x67092`): `adds r0,#16`
  (`0x3010`), and the words the ARM walk executed are the same halfwords read
  2 bytes off (`0x50293010`, …) until the ARM reading of `pop {r4,r5,r6,pc}`
  at `0x02137AA2` (`0xaa04bd70`) popped the heap pointer `0x02267068`.
* `0x02137A9E` — the pc the card named — is a **mid-instruction address**, not
  the transfer: it is `str r1,[r5,r4]` inside that Thumb block.  The transfer
  is the exception return; what was wrong is the SPSR's T bit.
* **Provenance**: the resumed context is a *saved thread frame*: its CPSR word
  (`0x4000001F`, mode SYS, Z=1) and its `lr` (`0x02137A96` = interrupted pc + 4)
  are exactly what the IRQ path writes.  It is not a function pointer, not a
  vtable slot and not a computed jump: the guest restored SPSR **0x4000001F**
  and asked the CPU to resume `lr-4` in the state that SPSR names.

## 3. What is wrong: the runtime never recorded the interrupted T bit

`irq_enter` synthesised SPSR_irq from NZCV + `cpsr_ctl`:

```c
spsr_bank[MODE_IRQ] = (n<<31)|(z<<30)|(c<<29)|(v<<28)|0x10u|cpsr_ctl;
```

`cpsr_ctl` is a **state-free** control byte by design (the runtime resolves the
instruction state per dispatch; a `mrs rd,CPSR` inside ARM code must read
T=0), so **every IRQ taken over Thumb code told the guest its interrupted
thread was in ARM state**.  The guest stored that SPSR into the thread context,
RestoreContext restored it 460 frames later, and the live-decoded Thumb bytes
at `0x02137A92` ran as ARM.  Hardware records CPSR→SPSR on exception entry, so
the value `0x4000001F` cannot occur on hardware for an interrupt over
`0x02137A92` (it must be `0x4000003F`).

A second, hidden instance of the same class appeared once the first was fixed:
`bios_irq_return` resumed the interrupted pc without telling the dispatcher what
state the SPSR named, so 5AZ's "predecessor state" rule (the ARM BIOS stub) ran
ov17's Thumb `blx 0x02043BD0` at `0x020D827A` as ARM and jumped to
`0x08BD8000` (`untranslated pc`, step 3,124,271,861).

## 4. The fix (runtime model, no special-casing, no pointer patching)

1. `nnktool/src/recompile.rs` `irq_enter`: SPSR_irq's T bit now comes from
   `nnk_state_of_next(resume, last_pc)` — the same witness the dispatch uses
   (2-byte stream stride = Thumb, 4-byte = ARM, else the compiled pc-form map).
   `bios_irq_return` masks the bit out of `cpsr_ctl` again (`& 0xDFu`) and
   stamps the resume carrier `nnk_smc_note_resume(*pc, spsr)`.
2. `tools/nnk_smc.inc`: `nnk_state_of_next()` (new witness) and
   `nnk_smc_note_resume()` (the Phase-5AW exception-return carrier for the BIOS
   return, which lands in the same loop iteration, so it stamps `steps-1`).
3. `tools/p5bb_irq_state_splice.py` + a `build_split.sh` chain entry applies the
   identical transform to the frozen image (marker `Phase-5BB IRQ-state splice
   applied`); the prelude audit gained `5bb_irq_state` / `5bb_irq_resume` rows
   (56/56 present).  `NNK_PRELUDE_DISABLE_SPLICE=5bb_irq_state` is the negative
   control.
4. `NNK_EXC_TRACE=1` logs every exception return (`target`, `spsr`, T bit,
   mode) — the measurement that produced this analysis.

**Verified**: the same state + script now logs
`exc_return step=3064069170 target=0x02137A92 spsr=0x4000003F thumb=1`, runs the
Thumb block instead of the ARM walk, and proceeds through the second barrier to
the step budget (3.62e9, frame 6470, `halt=step budget exhausted`).

## 5. Where the guest gets to now

`st3100.nnks` -> 3.5e9: no halt; the stream-2 movie (`op_u.mods`,
`codec=0x022C9F38`) starts at step 3,064,079,661 and presents its title cards
(Ghibli, Level-5, "音楽/石盤") on the bottom screen at frames 6200-6600.  A
healthy state inside the movie is saved as `work/repro_5bb/st3400.nnks`
(step 3.4e9, frame 6076) and `st3450.nnks` (frame 6165).

## 6. What is still missing (for the next card)

The prompt box: A presses held 10 frames at 6100/6200/6300 (inside the movie)
produce no "Skip the cutscene?" frame — the presses are applied and read by the
guest (`input_trace8.log`), the movie keeps playing.  Two candidates, both
reachable now:
1. the trigger window is tied to the *scene* the movie is in (the operator's
   prompt was reached from their own session, `work/repro_5au/melonds_state.mln`,
   at a different movie position), so sweep A/START presses across the movie
   (5000..6600) and watch for the dialog's AOBJ write — the box is drawn into
   AOBJ (bank B), which is the Phase-5BA upload defect, so a prompt that draws
   may still be missing its text/"No" tiles;
2. the dialog handler at `0x02137A70` (live-decoded, no compiled cases) calls
   `0x020DE224`/`0x020DE26C`; `NNK_DEBUG_AT_PC=0x02137A70` plus
   `NNK_VRAM_TRACE` will show whether the skip handler runs at all when A is
   pressed inside the movie.

No renderer/VRAM change was made; the Phase-5BA AOBJ and stream-2 residual
defects are untouched (they now sit behind a reachable scene).
