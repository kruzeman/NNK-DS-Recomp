# Phase-4K — decrypt-schedule divergence: root cause, fix, and the ten passes

Follows Phase-4J (merged `e87f774`).  Scope: find the loader decision that made
native stop after the first five decrypt taps while melonDS asked for ten, fix
it with measured semantics, and verify the arena at every pass boundary.

**Outcome: root-caused and fixed.  All TEN decrypt taps now run in native, in
melonDS's order, and the arena at EVERY pass boundary is byte-identical to the
melonDS per-pass dumps — 0 of 2168 words differ at all ten boundaries (§5).
The run then continues 86,059 instructions past the fifth tap (was 1,237) and
stops at a different, unrelated barrier: the card-data FIFO drain loop (§6).**

## 1. The exact first differing state (measurement, not inference)

Phase-4J left native at `0x0215B77C = 0x56BA5EE9` with all three images equal.
The task premise ("the descriptor loop ends after 5 taps") turned out to be one
instruction *before* the real divergence: native and melonDS agree on all five
taps, and differ at the **first instruction after the tap batch returns**.

Method: both runs emit a full PC trace; native (`NNK_TRACE_FROM=12600000`) and
melonDS (`MELONDS_ARM9TRACE_PATH`, 30 M lines) were aligned on the fifth
decryptor entry (`0x0215B468` #5; native step 13,141,375 = melonDS step
12,671,417) and compared instruction by instruction.  Before Phase-4K the very
same comparison was already exact up to that point (same five taps, same words,
same `r0` descriptors).

```
first diff at +1208 after tap 5 (native step 13,142,583 / melonDS 12,672,623)
   ... 0x0215B6FC 0x0215B700                     (identical)
nat  0x0215B704      <-- fall-through
mel  0x0215B528      <-- the decrypt helper (BLX r12 target)
```

The instruction at `0x0215B700` is `E12FFF3C` = **`blx r12`**:

```
0x0215B6E0  E92D000F  push {r0-r3}
0x0215B6E4  E58CA010  str  r10,[r12,#16]
0x0215B6E8  E1A0A00C  mov  r10,r12
0x0215B6EC  E59FC054  ldr  r12,[pc,#84]      ; literal at 0x0215B748
0x0215B6F0  E58AE000  str  lr,[r10]
0x0215B6F4  E59CC000  ldr  r12,[r12]
0x0215B6F8  E99A0007  ldmib r10,{r0,r1,r2}
0x0215B6FC  E24CCC21  sub  r12,r12,#0x2100
0x0215B700  E12FFF3C  blx  r12               ; -> 0x0215B528
0x0215B704  E1A0C000  mov  r12,r0            ; the fall-through native took
```

Two independent facts pinned the cause:

* the **compiled** case for `0x0215B700` was emitted from the ciphertext word
  `0x9FEB1278` (`work/phase3a/ovl_p4f4.bin` at that address; the union image
  used by the recompiler holds a *different pass's* word there), and decoded as
  `svcls 0x00eb1278` + `hle_swi` — the SDK/cache path was never emitted for
  this address;
* **SMC did** re-decode it from live RAM (`NNK_SMC_TRACE`: `pc=0x0215B700
  img=0x9FEB1278 word=0xE12FFF3C state=arm -> 0x0215B704`), i.e. the runtime
  half saw the right word and then executed it as a no-op.

Class: **decoder/dataflow bug on the loop path** (the `LDRSB` pattern of
Phase-4F), in the runtime re-decoder rather than in the image.

## 2. Fix 1 — `BX`/`BLX (register)` in the SMC ARM re-decoder

`tools/nnk_smc.inc:smc_exec_arm` had no case for `0x012FFF1m` / `0x012FFF3m`;
the encoding fell through to the data-processing branch and matched
`op=9 (teq)`, `rd=15`: `teq pc, r12, lsr pc` with `r[15]` stale.  It returned
"executed" with a garbage `npc`, so nothing halted — a silent mis-execution.

Added, mirroring the emitter (`nnktool/src/recompile.rs:Kind::Bx`): the
dispatch key is the even target address (the state is implied by which case
exists there) and `BLX` links `pc+4 | 1`.

## 3. Fix 2 — PC-relative base register in the SMC memory ops

With `BLX` in place the next halt was `untranslated pc=0xe59fa048`: the
`ldr r12,[pc,#84]` at `0x0215B6EC` had loaded from `r[15]+84` (the runtime
never maintains `r[15]`).  The ARM single-data-transfer, halfword/signed and
LDM/STM handlers used `base = r[rn]`; they now use `smc_reg(rn, pc, 0)`
(`pc+8` for `rn==15`), which is what the emitter does (`reg_mode`).

## 4. Fix 3 — long multiplies were never decoded (mask bug)

Frontier after fixes 1–2: `+63,438` after tap 5, at `0x0215B110`:

```
0x0215B0F8  ldr  r0,[pc,#112]   ; 0x10FEF011 (division magic)
0x0215B0FC  mov  r2,#0xF1
0x0215B100  umull r0,r1,r0,r4   ; E0810094
0x0215B104  lsr  r1,r1,#4
0x0215B108  umull r0,r1,r2,r1
0x0215B10C  subs r1,r4,r0       ; r4 % 241
0x0215B110  bne  0x0215B12C
```

Measured at `0x0215B110`, `r4 = 0x00324415` in both runs:
melonDS `r0=0x00324415 r1=0`, `CPSR.Z=1` (not taken, `r4 % 241 == 0`);
native `r0=0 r1=0x00324415`, `Z=0` (taken).  The multiply guard
`(w & 0x0FC000F0) == 0x90` masks bits 23..22, so it only matched `MUL`/`MLA`
and every **long** multiply (`UMULL`/`SMULL`/`UMLAL`/`SMLAL`, bit 23) fell into
the data-processing branch.  Mask corrected to `0x0F0000F0`; the existing
inner logic already distinguishes signed (bit 22) and accumulate (bit 21).

## 5. Fix 4 — the console WIFI MAC was never seeded (new class)

Frontier after fix 3: `+27,445`, at `0x0215B7AC` — a 6-byte compare:

```
0x0215B790  bl   0x02008EB0        ; memcpy(buf, 0x02FFFCF4, 6)
0x0215B794  ldr  r2,[pc,#156]      ; 0x0215C17C (the game's stored MAC, inverted)
0x0215B79C  ldrb r0,[r5,r6] / ldrb r1,[r2,r6] / eor r0,r0,#0xFF / cmp / bne
```

`0x02008EB0` is `mov r1,r0; ldr r0,=0x02FFFCF4; ldr ip,=0x02009E08; mov r2,#6;
bx ip` — a 6-byte copy of the **console WIFI MAC** from the mirrored
firmware-header copy at `0x02FFFCF4`.

Measured (melonDS `MELONDS_REGPROBE_PCS=0x0215B79C`,
`MELONDS_REGPROBE_MEM=0x02FFFCF4:8,0x027E3A30:8`):

```
melonDS  0x02FFFCF4 = 00 09 BF 11 22 33 41 10   buffer = 00 09 BF 11 22 33
native   0x02FFFCF4 = 00 00 00 00 00 00 00 00   buffer = 00 00 00 00 00 00
```

`00:09:BF` is Nintendo's OUI and `11:22:33` is melonDS's documented default
console MAC (`src/SPI_Firmware.h DEFAULT_MAC`; its boot log prints
`MAC: 00:09:BF:11:22:33`).  The game's stored MAC (`~` of `0x0215C17C`) is
`00:09:BF:00:00:31`, so the reference run matches the first three bytes and
stops at index 3; native (all zero) stopped at index 1 and took the
"different console" path.

The MAC is a **console/firmware property, not game or ROM data** — the same
class as the RTC and the firmware user-settings block already seeded by
`bios_bootparam` (Phase-4C2).  `tools/nnk_boot.inc` now writes it (plus the
enabled-channel bytes at `0x02FFFCFA`) from `guest_init`; `NNK_MAC=<12 hex>`
overrides it and the default is melonDS's `DEFAULT_MAC` so native/melonDS
evidence stays comparable.  `tools/split_dispatch.py` splices the call and the
include; `tools/build_split.sh` adds the fragment to the object-cache key.

## 6. Measured result

Run (stored guest image, live gx bridge, no per-pass images):

```sh
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

| | Phase-4J | Phase-4K |
|---|---|---|
| decryptor entries (`0x0215B468`) | 5 | **10** (same order, same `r0` descriptors) |
| PC trace identical to melonDS after tap 5 | 1,237 instr | **86,059 instr** |
| SMC re-decodes | 44 | 90,061 (halts 0, state_fallbacks 0) |
| steps | 13,142,612 | 13,676,011 |

Arena at every pass boundary — native `NNK_DEBUG_AT_PC=0x0215B468
NNK_DEBUG_MEM=0x02159FE0:0x21E0` vs the Phase-4J melonDS dumps
(`p4j_arena_h*.bin`, 0x02159FE0..0x0215C1C0 = 2168 words):

```
pass  1 vs h1 : 0/2168 words differ      pass  6 vs h7 : 0/2168
pass  2 vs h3 : 0/2168                   pass  7 vs h9 : 0/2168
pass  3 vs h4 : 0/2168                   pass  8 vs h10: 0/2168
pass  4 vs h5 : 0/2168                   pass  9 vs h11: 0/2168
pass  5 vs h6 : 0/2168                   pass 10 vs h12: 0/2168
```

No manual per-pass image is supplied: the arena is produced by the guest's own
decrypt calls, executed through the (now correct) runtime re-decoder.

## 7. New frontier (honest stop point)

First divergence after the ten passes — native step 13,227,434 / melonDS step
12,757,476 — is `0x02016B38`, the **card-data FIFO drain**:

```
0x02016B30  ldr  r2,[r4]        ; r4 = 0x040001A4 (ROMCNT)
0x02016B34  tst  r2,#0x800000   ; bit23 = data word available
0x02016B38  beq  0x02016B4C
0x02016B3C  ldr  r1,[r11]       ; r11 = 0x04100010 (CARD_DATA)
0x02016B40  cmp r3,#0x80        ; copy up to 128 words to 0x0206F120
0x02016B44  strcc r1,[r5,r3,lsl #2]
0x02016B48  addcc r3,r3,#1
0x02016B4C  tst  r2,#0x80000000 ; bit31 = transfer active
0x02016B50  bne  0x02016B30
```

Native takes the `beq` at `0x02016B38` where melonDS falls through: the native
card-bus model clears ROMCNT bit 23 one word earlier than melonDS's FIFO, so
the drain loop ends short (the registers at the loop head are identical for
the whole transfer — `r4=0x040001A4`, `r11=0x04100010`, `r5/r6/r7` equal — so
this is the FIFO model, not the guest code).  Class: card-bus / IO model, the
Phase-4D family, not the decrypt schedule.

Frames: still none with content (DISPCNT never leaves 0 — display init is not
reached), so no screenshot claim here.

## 8. Files

* `tools/nnk_smc.inc` — BX/BLX(register); PC-relative base in LDR/STR/LDRH/LDM;
  long-multiply mask; `__has_include` of the Thumb map with an ARM-only
  fallback so the mono (recompile-all → cc) image links without the generated
  map.
* `tools/nnk_boot.inc` (new) — console MAC + enabled channels at `0x02FFFCF4`
  (`NNK_MAC` override, default = melonDS `DEFAULT_MAC`).
* `tools/split_dispatch.py`, `tools/build_split.sh` — splice the boot fragment
  into `guest_init` and include it in the object-cache key.
* `tools/verify_phase2a.sh` — step D now copies the runtime fragments (and the
  Thumb map when the split build produced one) next to the mono image.

Guest images, arena dumps and melonDS captures stay under gitignored `work/`
and scratch.  No ROM bytes are committed.

## 9. Verification status (honest)

* `tools/verify_phase2a.sh`: sections A–C **PASS** (`RESULT: PASS`, trace at
  `p4k_verify.log`).  Step D stops at the mono link, not at a semantic check:
  the script `cd`s into the sibling worktree `worktrees/picture-fix` (it was
  written for that checkout), whose `tools/` predate the Phase-4J emitter
  change that makes every generated image `#include "nnk_smc.inc"`; that
  tree's mono image therefore cannot link.  The copy/fallback added here makes
  the step work in a checkout whose tools are current — it is a harness gap,
  not a semantic regression (the split build + the runs in §6 use the same
  generated image and link fine).
* The acceptance-relevant runs in §6 were made with this branch's build
  (`out/split/arm9_gx`, `NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c`).

