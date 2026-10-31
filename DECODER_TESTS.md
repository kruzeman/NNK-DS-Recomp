# Decoder regression tests — the five bug classes that stalled the boot chain

nnktool's decoder is now covered by `cargo test` for the five decode/emit bugs
that were found the hard way (each one cost a boot-chain debugging phase).  The
fixes were already in master; these tests exist so they cannot regress
silently.

```
cd nnktool
cargo test              # 14 tests, one command, no external crates
cargo test ldrsb        # one class (test-name filter)
```

Test file: `nnktool/src/decoder_regression_tests.rs` (a `#[cfg(test)]` module
of `recompile`, declared at the bottom of `nnktool/src/recompile.rs`; it is not
part of the release build and touches no decoder/runtime semantics).

Every fixture word below was verified with binutils before being committed:

```
printf '\xD7\x80\x9A\xE1...' > w.bin
arm-none-eabi-objdump -D -b binary -marm w.bin
```

| class | fixture word | objdump | old behaviour |
|---|---|---|---|
| 1 | `E19A80D7` | `ldrsb r8, [sl, r7]` | decoded as `ldrd` (8-byte load) |
| 2 | `E0801298` | `umull r1, r0, r8, r2` | emitted `r[0] = _lo; r[1] = _hi;` (swap) |
| 3 | `4798` (Thumb) | `blx r3` | linked `addr+4\|1` instead of `addr+2\|1` |
| 4 | `E12FFF3C` | `blx ip` | SMC re-decoder ran it as `teq pc, ip, lsr pc` |
| 5 | `B570` / `BD70` (Thumb) | `push {r4,r5,r6,lr}` / `pop {r4,r5,r6,pc}` | SMC re-decoder's `0xF600` mask made every POP execute as a PUSH (POP arm unreachable) |

## Class 1 — LDRSB vs LDRD (Phase-4F)

`nnktool/src/decode.rs`, extra load/store path (bits 7-4 = `1SH1`).  With
`SH=10` the instruction is **LDRSB when L=1** and **LDRD when L=0**; the old
decoder keyed on `SH` alone for the doubleword forms and matched
`(SH=10, L=1)` as `ldrd`.

* Fixture `E19A80D7` = `ldrsb r8, [sl, r7]`, measured live at `0x0200C7C8`.
* Old decode: `Kind::Hw { kind: Ldrd }`, text `ldrd r8, [sl, r7]` — an 8-byte
  load that clobbered r9 with path-string bytes.  The SDK path parse at
  `0x0200C638` then returned 0 and the guest took the `0x0210E388` abort arm.
  Across the image 271 instructions decoded as `ldrd` were really `ldrsb` and
  29 decoded as `ldrsb` were really `strd`.
* Reference: `PHASE4F_LDRSB_DECODE.md`.

Tests: `ldrsb_register_offset_decodes_as_signed_byte` (positive:
text + `HwKind::Ldrsb` + register-offset EA `[sl, r7]`) and
`ldrsb_is_not_ldrd_but_real_ldrd_still_decodes` (negative guard against the
LDRD match, plus a contrast that a genuine LDRD `E1CA80D0` = `ldrd r8, [sl]`,
L=0, still decodes as LDRD).

## Class 2 — UMULL/SMULL/UMLAL/SMLAL RdLo/RdHi (Phase-4V2)

`decode_arm` passed `Kind::Mul { rd: rdhi, rn: rdlo }` while the codegen
(`recompile.rs`, `Kind::Mul`) assigns `r[rd] = _lo; r[rn] = _hi;`.  Every
static-image long multiply therefore wrote its two destinations swapped, and
the UMLAL/SMLAL accumulate pair (`((uint64_t)r[rn] << 32) | r[rd]`) with them.
The runtime SMC re-decoder always had the correct order, which is why the
Phase-4K live-redecode fix looked healthy.

* Fixtures, all `… r1, r0, r8, r2` (RdLo=r1, RdHi=r0): `E0801298` umull,
  `E0A01298` umlal, `E0C01298` smull, `E0E01298` smlal.  Also the
  measured Phase-4K word `E0810094` = `umull r0, r1, r4, r0`.
* Old decode: `rd=0, rn=1` for the first four — generated `r[0] = _lo` /
  `r[1] = _hi`.  Measured consequence: the SDK divide-by-10 loop at
  `0x0201E814` computed `low32(x·0xCCCCCCCD)>>3` instead of
  `high32(...)>>3`, never converged, and the digit-store loop ran up through
  DTCM top, overwriting the IRQ user vector at `0x027E3FFC` at step
  25,214,182 (the 1.94M rejected IRQ entries that the "SDL quit at step
  27,371,209" was blamed on).
* Reference: `PHASE4V.md`.

Tests: `long_multiply_maps_rdlo_to_low_destination` (all four kinds; `rd`=lo,
`rn`=hi, plus the negative `assert_ne!((rd,rn),(0,1))` swap guard),
`measured_divide_loop_word_keeps_its_pair`, and
`long_multiply_codegen_writes_low_then_high_to_the_right_registers` (end-to-end
decode→emit: `r[1] = _lo;` must precede `r[0] = _hi;`).

## Class 3 — Thumb `blx Rm` link address (Phase-4Y)

`recompile.rs`, `Kind::Bx`.  ARM ARM B1.3.3 T1: Thumb `BLX Rm` links
`addr+2 | 1` (BLX always returns to Thumb state); ARM-state `BLX Rm` (A1) is a
4-byte instruction and links `addr+4 | 1`.  The old code emitted `addr+4|1`
unconditionally.

* Fixtures: Thumb `4798` = `blx r3`; ARM `E12FFF3C` = `blx ip`.
* Old emission at the measured site: the overlay dispatcher's `blx r3` at
  `0x020D1A1C` linked `0x020D1A21` instead of `0x020D1A1F`, so the return
  skipped the caller's epilogue `pop {r4,r5,r6,pc}` at `0x020D1A1E` and called
  the not-yet-downloaded module entry `0x02988AA0`.
* Reference: `PHASE4Y.md`.

Tests: `thumb_blx_rm_links_the_next_thumb_instruction` (positive `0x020D1A1F`,
negative `assert!(!contains("0x020D1A21"))`),
`arm_blx_rm_still_links_addr_plus_4` (the ARM form must not be "fixed" to +2),
`thumb_bx_rm_without_link_does_not_touch_lr` (no `r[14]` write for plain
`bx r3`).

## Class 4 — BX / BLX(register) dispatch (Phase-4K)

`tools/nnk_smc.inc:smc_exec_arm` had no case for `0x012FFF1m` / `0x012FFF3m`;
the encoding fell through to the data-processing branch and matched
`op=9 (teq)`, `rd=15`: `teq pc, r12, lsr pc` with a stale `r[15]`.  It
reported "executed" with a garbage `npc`, so nothing halted — a silent
mis-execution of `E12FFF3C blx r12` at `0x0215B700`, which should have called
the decrypt helper at `0x0215B528` (first divergence vs melonDS, step
13,142,583).

nnktool's own decoder (`decode.rs`) has always had the BX case, so the
regression lived in the **C runtime half**, which has no C test harness (the
include depends on live guest state).  It is locked at the source level
instead — the honest option, and it does go red if the case is deleted.

* Fixtures: `E12FFF1E` = `bx lr` (every ARM return), `E12FFF3C` = `blx ip`.
* Reference: `PHASE4K_DECRYPT_SCHEDULE.md` §2.
* Tests: `bx_and_blx_register_are_dispatched_before_data_processing` (positive
  `Kind::Bx { rm, link }` for both words), `bx_register_never_decodes_as_data_processing`
  (negative guard: not `Kind::Dp`, text never contains `teq`),
  `runtime_smc_redecoder_keeps_its_bx_case_before_the_dp_catchall` (reads
  `../tools/nnk_smc.inc` and asserts the `0x012FFF10u` / `0x012FFF30u` case
  exists, sits **before** the `0x0C000000u` data-processing catch-all, and
  still links `pc+4|1`).

## Class 5 — Thumb PUSH/POP format-13 mask (Phase-5A)

`tools/nnk_smc.inc:smc_exec_thumb` format 13 (`1011 L 10 R <low list>`) tested
PUSH first with `(hw & 0xF600) == 0xB400` and POP with `(hw & 0xF600) ==
0xBC00`.  0xF600 keeps bits 15,14,13,12,10,9 — **bit 11 (`L`) is not in the
mask**, so push and pop compare equal (`0xBD70 & 0xF600 == 0xB400`).  Every
POP therefore executed the PUSH arm and the POP arm was unreachable dead code.

* Fixtures (objdump-verified): `B570` = `push {r4,r5,r6,lr}` (the prologue at
  `0x020D19B8`), `BD70` = `pop {r4,r5,r6,pc}` (the epilogue at `0x020D1A1E`).
* Old behaviour, measured: the `bl` at `0x020D1642` linked `0x020D1647` and the
  `push` stored it correctly (`0x027E3AA8: 00000000 02143158 00000000
  020D1647`), but at the epilogue the pop pushed a second frame and the guest
  fell through to `0x020D1A20` (the literal pool) instead of returning to
  `0x020D1646`, walking `0x020D1A20..0x020D1A8E` as code and halting at step
  39,001,760.  (That fall-through is also why Phase-4Z read the symptom as
  "the pop returns 0x020D1A21" — `0x020D1A20|1`.)
* Fix: mask `0xFE00` on both arms (`0xB4xx` push, `0xBCxx` pop).  The Rust
  half re-tested the L bit and so never mis-executed, but its mask is now
  `0xFE00` with both arms matched explicitly — and the new positive test
  caught that a `0xB400`-only match turns every pop into `Kind::Undef`.
* Reference: `PHASE5A.md`.

Tests: `thumb_push_and_pop_split_on_the_l_bit` (both fixtures: text, `LdmStm`
load flag, list `0x4070`/`0x8070`, `pu` 2/1), `thumb_pop_never_decodes_as_push`
(negative guard: `0xBD70` never `push`, always `LdmStm { load: true }`, plus
the `0xF600`-cannot-separate / `0xFE00`-does sanity assertions),
`runtime_smc_redecoder_masks_the_form13_rbit` (source-level lock for the C
half: no `0xF600` format-13 mask, both `0xFE00` arms present with PUSH before
POP, and the POP arm still pops list bit 8 into `npc`).

Red-on-old, measured: putting the two `0xF600` masks back makes
`runtime_smc_redecoder_masks_the_form13_rbit` fail with
"regression: smc_exec_thumb is masking format 13 with 0xF600 again (bit 11
hidden)"; restoring `0xFE00` is green again.

## Red-on-old / green-on-current proof

Reverting the four fix lines (in a throw-away copy of the crate, so the tree
stays clean):

```
# 1  decode.rs : (2,true) => ("ldrsb", Ldrsb)   ->  ("ldrd", Ldrd)
# 2  decode.rs : Kind::Mul { rd: rdlo, rn: rdhi } -> { rd: rdhi, rn: rdlo }
# 3  recompile.rs : linkv if thumb {addr+2|1} else {addr+4|1} -> addr+4|1
# 4  tools/nnk_smc.inc : 0x012FFF10u -> 0x012FFF11u  (case no longer matches)
$ cargo test
test ...::ldrsb_is_not_ldrd_but_real_ldrd_still_decodes ... FAILED
test ...::ldrsb_register_offset_decodes_as_signed_byte ... FAILED
test ...::long_multiply_codegen_writes_low_then_high_to_the_right_registers ... FAILED
test ...::long_multiply_maps_rdlo_to_low_destination ... FAILED
test ...::measured_divide_loop_word_keeps_its_pair ... FAILED
test ...::thumb_blx_rm_links_the_next_thumb_instruction ... FAILED
test ...::runtime_smc_redecoder_keeps_its_bx_case_before_the_dp_catchall ... FAILED
test result: FAILED. 4 passed; 7 failed; 0 ignored; 0 measured; 0 filtered out
```

Representative messages:

```
regression: E19A80D7 decoded as LDRD again (8-byte load)
assertion `left == right` failed: umull destination order (rd=lo, rn=hi)
E0810094 must be umull r0(lo), r1(hi), got Mul { kind: Umull, s: false, rd: 1, rn: 0, rm: 4, ra: 0 }
Thumb blx r3 must link r[14] = 0x020D1A1Fu; (addr+2|1)
regression: smc_exec_arm lost the BX/BLX(register) case (0x012FFF1m)
```

The 4 tests that stay green in that run are the deliberate contrasts and the
nnktool-side BX decode (that path was never broken — class 4's regression was
in the C re-decoder, covered by the source-level test).

On current master:

```
$ cd nnktool && cargo test
running 14 tests
test result: ok. 14 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.00s
```

## Coverage / limits

* Positive assertions: 14 tests, ≥1 per class (class 2 and 3 have three each,
  class 5 has three).
* Negative guards that pin the old buggy shape: 6 (LDRD match, swapped
  `(rd,rn)==(0,1)`, swapped `r[0] = _lo` emission, `addr+4|1` Thumb link,
  `Kind::Dp`/`teq` fall-through, missing SMC BX case).
* Not covered by automated tests, stated honestly:
  * the C runtime half of class 4 and class 5 beyond their source invariants
    (no C harness for `nnk_smc.inc`; a real test would need the generated
    dispatch image and live guest state);
  * the Phase-4K long-multiply **mask** fix in `tools/nnk_smc.inc`
    (`0x0FC000F0` → `0x0F0000F0`) — different bug from class 2's field swap,
    and likewise C-side only.
* No decoder or runtime semantics were changed by this task; the only edits
  are the test module, its one-line declaration in `recompile.rs`, and this
  document.
