# Phase-5BE — SWI 0x2C is a decode of DATA; the IRQ-entry SPSR T bit must come from the carrier

Follows Phase-5BD (branch `wt/t_e40b23ae`, commit `cadd93c`). Branch `wt/t_7aa2d437`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=/home/hermes/NiNoKuni/work/phase3a/arm9_thumb_ovl_itcm_p5u.c SPLIT_DIR=/home/hermes/NiNoKuni/work/split/c
bash tools/build_split.sh out/p5be2/nnk_runner` → `BUILD OK`.

## 1. SWI 0x2C is not a service call (measured, not inferred)

The halt state's word was taken from UNALIGNED bytes, so the "SWI" is a data
word whose top nibble happens to be 0xF:

```
smc: on-demand step=4467284178 pc=0x02143186 word=0x7F2C0001 state=arm -> 0x0214318A
hle swi imm24=0x2C0001 num=0x2C lo=0x01 st=arm ...
```
Bytes at 0x02143186 (dump of `work/repro_5bb/st3400.nnks`, and the halt-time
savestate is byte-identical there): `01 00 2C 7F 27 02` → `0x7F2C0001`;
`(w & 0x0F000000) == 0x0F000000` makes it `svcle #0x2C0001`.  0x02143158 is
**overlay 17 BSS** (base 0x020D0A00, code size 0x72700 → code ends 0x02143100),
i.e. runtime DATA: `{0x020D1C70, 0x02143158, 0x0213A340, ...}`, and the walk at
0x02143158..0x02143186 is a 4-byte-stride ARM walk through that data
(`0x0213A340`, `0x02278264`, `0xFFFF0053`, `0x10F00052`, ...).  It is a wild
branch, not a service request — there is no SWI 0x2C to implement.

## 2. Root cause: the IRQ-entry SPSR lost the T bit of an interworking target

Measured trace (savestate `work/repro_5bb/st3400.nnks`, ROM-only, GX headless):

```
pc 0x0207DD78  ARM `blx 0x02137C28`   (live-decoded: movie player, per frame)
               smc_branch_mode(0x02137C28|1)  -> carrier says THUMB
irq enter step=4467281928 resume=0x02137C28 ... spsr=0x2000001F   (T=0, WRONG)
irq return -> guest thread switcher -> 0x0200729C `subs pc, lr, #4`
smc: on-demand step=4467283486 pc=0x02137C28 word=0xB084B570 state=arm  <- WRONG
```
`nnk_state_of_next(0x02137C28, 0x0207DD78)` saw a **non-sequential** successor
(the `blx` target), so the stride rules did not apply and it fell through to
the compiled pc-form map, which has no case at 0x02137C28 (live-decoded) and
answers ARM.  It never asked the one-step CARRIER, which already said Thumb.

Consequence chain (all measured):
* SPSR_irq 0x2000001F (T=0) → the guest's thread context saved that CPSR;
* the exception return restored T=0, so 0x02137C28's Thumb prologue
  `push {r4,r5,r6,lr}` / `sub sp,#16` (0xB084B570) executed as ARM (`ldr`) —
  storewatch proves neither the push nor the sp adjustment happened:
  the next nested push still used `sp=0x027E3B18` and wrote 0x027E3B08..14
  (the caller's *own* frame slots), and every nested call re-used that frame;
* the epilogue `pop {r4,r5,r6,pc}` at 0x02137CDE popped 0x02143158 (a data
  pointer), and the ARM walk of that data halted on the fake `svc`.

## 3. The fix (one witness, in the dispatch's own order)

`tools/nnk_smc.inc: nnk_state_of_next()` now consults `smc_carried_thumb()`
FIRST — the same one-step carrier the dispatch consults (5AS/5C/5J/5K/5O/5AW),
stamped by `nnk_smc_note_branch` / `smc_branch_mode` / `nnk_smc_note_resume`.
A taken interworking branch is exactly the case neither stride rule can
classify and the compiled map cannot see.  Nothing else changed; the carrier
is already one-step scoped and pc-keyed, so a stale stamp cannot answer.

## 4. Measured after the fix

* `nnk_state_of_next(0x02137C28, 0x0207DD78)` → Thumb (carrier), SPSR_irq now
  carries T=1, the thread context saves/restores it, and the exception return
  resumes 0x02137C28 in Thumb state.
* The same repro (limit 5.4e9) **no longer halts at step 4,467,284,179**:
  `done steps=5400000001 … halt_pc=0x0200986C halt=step budget exhausted`,
  `distinct_pcs=31706` (was 25,822) — it runs like normal code to the budget.
* The film now presents **`data/movie/s01.mods frame=461/581`** (was 382/581)
  and is still advancing when the budget ends; 462 presents, **0**
  `op_u`/`op_d` lines: every present is the bound container.
* Class test: `cargo test --release` → **62 passed, 0 failed** (61 + new
  `phase_5be_irq_spsr_uses_the_interworking_carrier`, which pins the witness
  order carrier → stride → map, the `blx imm`/SPSR-bit-5 carriers, and the
  single `nnk_state_of_next(resume, last_pc)` consumer).

## 5. Not done here (honest)

The card's last acceptance item — `frame=580/581` plus the guest's own
end-of-movie handoff back to the cutscene loop 0x0207C678 — is **not yet
shown**: the 5.4e9 budget is now consumed by *real* execution (the fix also
restores the frame code that was being skipped), and the film needs roughly
0.6–1.0e9 more steps past 461/581.  The halt is gone; what remains is a longer
run, not a defect.  Follow-up: same command with `NNK_STEP_LIMIT=7000000000`
(and `NNK_GX_SHOT*` for the last frames), expecting `frame=580/581` and the
return to 0x0207C678.
