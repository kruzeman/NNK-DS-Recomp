# Phase-5M — barrier I: the Mobiclip player's unrooted PC chain, first link rooted (ITCM live decode)

Follows Phase-5L (merged `31bcd65`).  Phase-5L's branch run halted at
`dispatch to untranslated pc` with `halt_pc=0x0207B218`, and named that as
barrier I without a mechanism.  Phase-5M measures the mechanism, fixes the
decoder gap behind it, and moves the run **past** `0x0207B218` into the movie
player's card stream.  The movie does not play yet; the next barrier is named.

**Headline**

1. **The halt PC was not the failing PC.**  The run's own diagnostic
   (`untranslated pc=0x%08x`) says `0x01FF8540`, not `0x0207B218`:
   `0x0207B218` is only the *last executed* PC (`halt_pc=last_pc`).  The
   instruction there is a legitimate ARM `blx r1` (word `0xE12FFF31`), and its
   target is an **ITCM** address.
2. **Why it halted:** the band dispatch routed the ITCM PC to the compiled
   `nnk_part_itcm`, whose rooted ITCM range ends at `0x01FF8520`
   (`pop {r3-r9,pc}`); `0x01FF8540` has no compiled case, so `rc==0` and the
   run fell into `nnk_smc_exec_any`, which refused **every PC outside main
   RAM** and returned 0 → honest halt.
3. **melonDS really executes `0x01FF8540`** (probe:
   `MELONDS_ARM9TRACE_LO=0x01FF8540 HI=0x01FF8560 N=64` → 64 hits; the same
   window shows ITCM code at `0x01FF84D8` writing display IO).  The ITCM model
   is exact: native's `NNK_ITCM_DUMP` is **byte-identical** (0 differing bytes
   out of 32 KiB) to melonDS's `MELONDS_ITCM_DUMP` at first ITCM execution.
4. **Fix:** `nnk_smc_exec_any` now decodes live code from the ITCM window
   (`ITCM_LO..ITCM_HI`, fetched from the guest-owned 32 KiB `itcm` array)
   exactly as it does from main RAM.  Class 18 in `decoder_regression_tests.rs`
   locks it (suite 29 → **30**).
5. **Measured effect:** the run passes `0x0207B218`/`0x01FF8540` and continues
   **542,287 steps** further into the movie player (2,256,702,439 vs
   2,256,160,152), with distinct PCs 21,685 → **22,704** and card starts
   9,726 → **9,747**.
6. **Next barrier (I2, named): the empty IRQ vector slot at `0x027E0048`.**
   The new halt is `bx r0` at `0x01FF804C` (the SDK IRQ dispatcher in ITCM)
   with `r0 = [0x027E0000 + 18*4] = 0` — the IPC (bit 18) handler slot is
   zero while `IE&IF = 0x00040000`.  `bx 0` sends PC to `0x00000000`
   (`untranslated pc=0x00000000`), i.e. ARM9 BIOS vector space, which the
   runtime does not model.  See §4 for the two candidate root causes.

## 1. What actually halts — measured

```
# Phase-5L branch head (p5l1), rebuilt and re-run here with NNK_SMC_FAILLOG=1
done steps=2256160152 traced=2256160151 distinct_pcs=21685 hle_hits=11202
     halt_pc=0x0207B218 halt=dispatch to untranslated pc
untranslated pc=0x01ff8540                     <-- the real failing PC
```

`halt_pc` is `last_pc` (the last PC that went through `trace_pc`), so it names
the instruction that *branched* to the failing address, not the address that
failed.  Phase-5L's "unrooted player PCs at `0x0207B218`" therefore described
the wrong end of the jump.

The player function around it (melonDS RAM dump, `p5l/mel_ram.bin`, ARM
disassembly at `0x0207B1D0`):

```
207b1ec: ldr  r0, [r5, #52]
207b1f0: ldr  r1, [r5, #56]      ; r1 = callback from the player object
207b1f4: ldr  r0, [r0]
207b1f8: ldrh r0, [r0]
207b1fc: tst  r0, #0x8000
207b200: movne r0, #1
207b204: strne r0, [r5, #164]
207b210: ldr  r0, [r5, #52]
207b214: streq r4, [r5, #164]
207b218: blx  r1                 ; word 0xE12FFF31 — target 0x01FF8540 (ITCM)
```

melonDS's ARM9 trace window `0x0207A000..0x0207C000` (200,000 PCs,
`p5m/mel/player_trace.txt`) shows this region executed in ARM state at a
4-byte stride, the routine above repeated in a loop, and `0x0207b218` visited
(1056 distinct PCs in the window).

## 2. The ITCM half

* The auto-load ITCM block is 0x540 bytes (`0x01FF8000..0x01FF853F`, extracted
  from `arm9.bin+0x6C5C0`) — it ends **exactly** at the failing PC.  The code
  at `0x01FF8540` is written into ITCM by the guest at run time (the movie
  player's own copy), so the only honest way to execute it is from live ITCM.
* `nnk_part_itcm` carries cases for `0x01FF8500..0x01FF8520` (ARM, ending in
  `pop {r3,r4,r5,r6,r7,r8,r9,pc}`) and nothing above.
* melonDS proof the PC is real code:

```
$ MELONDS_ARM9TRACE_LO=0x01FF8540 MELONDS_ARM9TRACE_HI=0x01FF8560
  MELONDS_ARM9TRACE_N=64 ... melonDS "Ni No Kuni.nds"
# ARM9 trace window 0x01FF8540..0x01FF8560 limit 64
0x01ff8540  (x64)
```
  and around it, `unknown ARM9 IO write32 04001060 00000000 01FF84D8` — ITCM
  code at `0x01FF84D8` writing display registers, i.e. this is the display
  half of the movie path.
* ITCM model cross-check (`NNK_ITCM_DUMP` vs `MELONDS_ITCM_DUMP`):
  `cmp -l itcm5.bin mel2/itcm_mel.bin` → **0 differing bytes** of 32 KiB.
  Native's ITCM is exactly melonDS's ITCM, so live-decoding it is legitimate.

## 3. The fix (measured scope)

`tools/nnk_smc.inc`, `nnk_smc_exec_any`: the word fetch now has two windows —
ITCM (`ITCM_LO <= pc < ITCM_HI`, bytes from `itcm + (pc & (ITCM_PHYS-1))`) and
main RAM (unchanged).  Everything after the fetch (state resolution, ARM/Thumb
decode, `ld8/ld16/ld32`/`st*`, `card_tick`, trace) is shared, and those
accessors already route DTCM/ITCM/IO/windowed RAM, so an ITCM instruction can
touch any bus exactly like a main-RAM one.

Class 18 (`runtime_live_decoder_covers_the_itcm_window`) pins: the two windows
are disjoint, `0x01FF8540` lies at offset `0x540` inside the 32 KiB array, the
runtime prelude still defines `ITCM_LO/HI/PHYS`, and the C hook still has the
ITCM branch reading the guest-owned array.  Suite 29 → **30** (class 17 below
takes it to 31 in §6).

## 4. Result and the next barrier

```
done steps=2256702439 traced=2256702438 distinct_pcs=22704 hle_hits=11202
     halt_pc=0x01FF804C halt=dispatch to untranslated pc
untranslated pc=0x00000000
p4t: starts=9747 last_op=0xB8
gx: 4035 frames presented
```

| check | Phase-5L | now |
|---|---|---|
| steps | 2,256,160,152 (halt) | **2,256,702,439 (halt, +542,287)** |
| distinct guest PCs | 21,685 | **22,704** |
| card starts | 9,726 | **9,747** |
| frames presented | 4,034 | 4,035 |
| first `(op,addr,size)` card divergence vs melonDS | none | **none** |

Card stream: `python3 tools/card_cmdseq.py p5m/card.m5.log p5h/mel_card.log` →
*first addr/size divergence at none* (native B7-only 0, melonDS B7 2461 — the
run simply stops earlier: 9,747 of melonDS's 9,840 starts).  The movie data is
now streaming (`B7 0x15BE8C00..0x15BF0000`, 512-byte blocks, `crc` logged) from
the movie's own ROM region.

**Barrier I2 (named): `bx r0` with an empty IRQ vector slot — `0x01FF804C`.**

```
irq enter step=2256702419 resume=0x020255D0 vector=0x01FF8000
          sp_irq=0x027E3F64 spsr=0x2000001F ime=1 ie=0x00040019 if=0x000C0000
untranslated pc=0x00000000
```

`0x01FF804C` is the SDK IRQ dispatcher in ITCM:

```
1ff8000: stmfd sp!, {lr}
1ff8004: mov  ip, #0x04000000 ; +0x210 (IE)
1ff800c: ldr  r1, [ip, #-8]
1ff8010: cmp  r1, #0        ; no enabled IRQ -> return
1ff8018: ldm  ip, {r1, r2}   ; r1 = IE, r2 = IF
1ff801c: ands r1, r1, r2
1ff8024: mov  r3, #0x80000000
1ff8028: clz  r0, r1         ; highest pending+enabled bit
1ff8038: str  r1, [ip, #4]   ; acknowledge IF
1ff803c: rsbs r0, r0, #31    ; bit index
1ff8040: ldr  r1, [pc, #8]   ; = 0x027E0000 (DTCM vector table)
1ff8044: ldr  r0, [r1, r0, lsl #2]
1ff804c: bx   r0             ; <-- r0 == 0
```

`IE&IF = 0x00040000` (IPC recv, bit 18), so the dispatcher takes slot 18
(`0x027E0048`), which reads **0**, and `bx 0` sends the PC to `0x00000000`
(ARM9 BIOS vector space — not modelled).  Two candidate roots, not yet
distinguished by measurement:

* **A — our IPC model raises an IRQ the game has already unmasked away from:**
  the ARM7 FIFO responder (Phase-4Q) replies to the last FIFO send of the
  handshake while the game is inside the movie player; melonDS may leave IF
  bit 18 clear (or IME/IE unchanged) at that instant.
* **B — the slot really is 0 in melonDS too, and `bx 0` is guest behaviour:**
  then the missing piece is the ARM9 BIOS at `0x00000000` (its exception
  vector / reset stub), which this runtime does not model at all.

Distinguishing them needs one measurement: melonDS's DTCM word at `0x027E0048`
plus its IF at the same resume PC (`0x020255D0`), or an IRQ trace
(`MELONDS_IRQTRACE_PATH`) around the IPC acknowledgement.

## 5. Frames

Frame 4000 is still the Mobiclip splash (top: white `0xfbfbfb` + red `0xfb0000`,
34 distinct sampled colours); the run halts 35 frames later, so no movie frame
is presented yet.  `shots/p5m_f04000_top.png`, `shots/p5m_f04000_bot.png`.
No successive non-uniform movie frames exist in this phase — the honest
statement is that the movie *data* now streams while the picture is still the
splash.

## 6. Also in this phase (smaller, independent)

* **Thumb format 8 implemented.**  Phase-5L left `0101 H S 1 Ro Rb Rd`
  (STRH/LDRSB/LDRH/LDRSH register-offset) as `return 0`.  It is now decoded
  exactly as `nnktool/src/decode.rs:1077-1085` (H = bit11, S = bit10, no
  scaling) with sign extension on the signed loads and the same load/store
  costs.  Class 17 locks it (`thumb_format8_register_offset_halfword_and_signed`,
  four fixtures `0x5241/0x5641/0x5A41/0x5E41` + source assertions).  Measured:
  **no live format-8 execution occurs anywhere in the 2.25G-step run**
  (`NNK_F8LOG=200` → 0 lines), so it is coverage, not the barrier — but it
  removes the last silent "halt instead of decode" hole in the Thumb load/store
  block.
* `HwKind` gained `PartialEq, Eq` so the class-17 fixtures can be compared
  against the decoder's answer.
* Suite: 29 → **31** tests, all green (`cd nnktool && cargo test`).

## 7. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5m
# build (NOTE: the image is the p5k one — the p4j image named in earlier task
# notes is stale and produces a DIFFERENT run that halts at 0x020F5DF4, 40.7M)
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5k.c \
  bash tools/build_split.sh out/split/p5m5

env NNK_ROM_PATH='Ni No Kuni.nds' NNK_GX=headless NNK_STEP_LIMIT=2300000000 \
    NNK_SMC_FAILLOG=1 NNK_DUMP_MEM=$S/halt5_ram.bin NNK_ITCM_DUMP=$S/itcm5.bin \
    NNK_GX_SHOT=$S/f5/sh NNK_GX_SHOT_EVERY=50 NNK_CARDLOG=$S/card.m5.log \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    out/split/p5m5 work/phase3a/arm9_stored_padded.bin > /dev/null 2> $S/m5.err

# melonDS cross-checks
MELONDS_ARM9TRACE_LO=0x01FF8540 MELONDS_ARM9TRACE_HI=0x01FF8560 \
MELONDS_ARM9TRACE_N=64 MELONDS_ITCM_DUMP=$S/mel2/itcm_mel.bin \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS 'Ni No Kuni.nds'
cmp -l $S/itcm5.bin $S/mel2/itcm_mel.bin   # 0 differences
```
