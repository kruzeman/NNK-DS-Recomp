# Phase-5AS — the 0x02062C48 crash: root cause, the state carrier (OPT-IN), and the honest gap

Follows Phase-5AP (merged as `83f38f0`).  Build: `NNK_GX=1
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh
work/split/arm9_5as` (prelude audit green: 51 required / 3 divergent / 3
obsolete; `cargo test`: **56 passed**).

## 1. Both reproducers reproduce, byte-exactly

Cold boot (the operator's own session, `work/repro_5as/operator_session_freshboot.nnk`):

```
$ NNK_INPUT_FILE=work/repro_5as/operator_session_freshboot.nnk \
  NNK_STEP_LIMIT=700000000 NNK_GX=headless work/split/arm9_5as 'Ni No Kuni.nds'
done steps=508793671 traced=508793671 distinct_pcs=24874 hle_hits=7968 \
     halt_pc=0x02062C48 halt=undefined instruction 0xF94AF000
halt state: r0=00000000 r1=00000000 r2=020774F0 ... r14=0204F2D9 nzcv=0010
```

Savestate (operator session state + `press_a_frame1100.nnk`), **on a different
build than the one that wrote the state** (`NNK_SAVESTATE_IGNORE_BUILD=1`):

```
state: WARNING: NNK_SAVESTATE_IGNORE_BUILD=1 accepts
       work/repro_5as/operator_title_notice.nnks written by build 0x0768D9EB
       on build 0xEB1B328D -- reproduce-only, never for a round trip
state: loaded ... (identity OK: rom 536870912 B hdr_crc=0x72EEE403
       arm9_crc=0x8ECE2C54 build=0x0768D9EB; restored step=611840034 frame=1097)
done steps=626972668 ... halt_pc=0x02062C48 halt=undefined instruction 0xF94AF000
halt state: r0=02278388 r1=0213BB3C r2=00000000 r3=027E3B10 r4=02278264
            r5=02143174 r6=00000D34 r7=00000000 r13=027E3B38 r14=020FEBB7 nzcv=0000
```

Both match the operator's numbers exactly.  `NNK_SAVESTATE_IGNORE_BUILD=1`
relaxes ONLY the build fingerprint: magic, version, header/payload/region
CRC32s, region names+lengths and the ROM identity stay strict (measured: the
refusal still fires on a region-length or CRC mismatch).

## 2. GOAL 1 — what 0x02062C48 is, and who branches there

Measured with `NNK_TRACE_FROM=626970000` (savestate reproducer):

```
... 0x020FEBAC 0x020FEBAE 0x020FEBB0 0x020FEBB2   <- ov17, Thumb, 2-byte stride
0x02062C44                                        <- arrival
0x02062C48                                        <- the halt
```

`0x020FEBB2` holds `f764 f847` = Thumb `bl 0x02062C44`:
LR = 0x020FEBB6|1 = **0x020FEBB7** (the r14 the halt state shows — the caller
is Thumb and a Thumb BL does not change state).  The target is a Thumb function:

```
0x02062C44  b510        push {r4,lr}
0x02062C46  1c04        adds r4,r0,#0
0x02062C48  f000 f94a   bl   0x02062EE0
0x02062C4C  1c20/307c   adds r0,r4,#0 ; adds r0,#0x7c
```

The **compiled image** carries `0x02062C44` as the ARM reading of those bytes —
`cdpne 5,0,cr4,cr11`, i.e. a coprocessor no-op — and then `0x02062C48` as an
ARM undefined-instruction trap.  Provenance of that wrong form: the phase3a
function seed file `work/phase3a/arm9_functions.tsv` lists the entry as
`0x02062C44  arm  2  8` (mode **arm**, 8-byte body), so the recompiler walked
it in ARM state and emitted `cdpne` + a trap.  It is *not* a pointer: no
literal `0x02062C44`/`0x02062C45` exists in any of the 22 ROM images, and
`tools/pcform_query.py work/phase5ap/nnk_pcform_map.h 0x02062C44` → **HOLE**
(no ROM-declared entry, ctor or pointer reaches the function).  So neither the
ROM map nor the seed pass can arbitrate here: the FORM has to come from the
instruction that branched.

## 3. GOAL 1 — the state carrier (implemented, measured, OPT-IN)

`tools/nnk_smc.inc` + `tools/split_dispatch.py`:

* `nnk_smc_carry_override(&pc)` — new dispatch hook, called after
  `nnk_smc_try()` and **before** the compiled band switch.  When a fresh
  carried state (`smc_t_pc`/`smc_t_val`, set by `smc_branch_mode`) disagrees
  with the compiled case's form (`nnk_pc_is_thumb`), the pc is decoded from
  live memory in the CARRIED state (`smc_force_state` -> `nnk_smc_exec_any`).
* `smc_carry_forward()` — the interpreter contract: the sequential successor
  (pc+2 / pc+4) of an instruction executed under a carrier inherits that
  state, so a stream keeps the state the branch promised (without it the
  carrier covered one instruction and 0x02062C48 ran as the ARM trap).
* `smc_t_live` — ONLY a carrier produced by a LIVE-decoded instruction may
  overrule a compiled case.  Measured counter-example for trusting compiled
  carriers: an ARM `ldmfd sp!,{pc}` at 0x0200F2D8 popped a stray odd word,
  "proved" Thumb for the ARM routine at 0x0200DC38, and re-decoded a whole ARM
  block as Thumb (`asrs`/`b` garbage, death at 0x02009112).
* `carry_overrides=N` in the run summary; `stateconf CARRIED pc=... carried=
  thumb compiled=arm step=...` lines; `NNK_STATE_CARRIER=1` enables it.

Class 52 test: `cargo test` →
`phase_5as_state_carrier_wins_over_a_wrong_form_compiled_case` (**56 passed**).

### Why it is opt-in: the measured remaining gap

| configuration | savestate reproducer | cold-boot reproducer |
|---|---|---|
| baseline (pre-5AS) | 626,972,668 · 0x02062C48 | 508,793,671 · 0x02062C48 |
| carrier ON | 612,149,847 · 0x02055B7C | **18,395,574 · 0x027FF800** |

So the carrier alone moves the savestate path 310k steps forward but DIVERGES
the cold-boot path after 18.4M steps: some live-decoded producer still hands
over a state that is wrong for the arrival.  The candidates measured while
chasing it (both reverted, see §5):

* the emitted ARM `blx rM` link tags bit0 (`r[14] = addr+4|1`) while the live
  ARM decoder links `pc+4` with no tag (`tools/nnk_smc.inc`, Phase-5D, melonDS
  `A_BLX_REG`).  With the 542-image-site transform applied the cold-boot run
  still diverges (318,836,046 · 0x02079AC8) — and that halt reproduces with the
  carrier OFF, i.e. the link change alone changes behaviour.  The tree's own
  test `arm_blx_rm_still_links_addr_plus_4` pins the existing (tagged) form, so
  changing it needs its own measured phase.
* `nnk_pc_is_thumb` is built from the fall-through stride, and this image has
  NO `nnk_state=` markers; `0x02008DC8` (ARM `mov r0,r4`) is classified Thumb
  because its emitted fall-through is addr+2.

## 4. `NNK_SAVESTATE_IGNORE_BUILD=1` (GOAL 2 — done, proven)

`tools/nnk_state.inc`: when the build fingerprint differs, the load is refused
as before UNLESS `NNK_SAVESTATE_IGNORE_BUILD=1`, which prints

```
state: WARNING: NNK_SAVESTATE_IGNORE_BUILD=1 accepts <path> written by build
       0x%08X on build 0x%08X -- reproduce-only, never for a round trip
```

and continues.  Everything else stays strict (§1).  `smc_force_state`,
`smc_ncarry`, `smc_t_live`, `smc_carrier_ab` are deliberately kept OUT of the
generated state field table (`tools/nnks_gen_state_fields.py`, CONFIG), so the
5AQ state layout is byte-identical to pre-5AS builds — that is what makes the
operator's state loadable at all (a first attempt grew the `io` region by 12
bytes and the load was refused for a length mismatch, correctly).

**TODO for the next run:** the same paragraph is to be mirrored into
`SAVESTATES.md` (not done here — budget).

## 5. Reverted / not shipped

* `tools/p5as_blx_link_splice.py` and the `5as_blx_link` manifest row (built,
  measured, then removed: it changes guest behaviour and needs its own phase).
* `nnktool/src/recompile.rs` ARM BLX link changes (same reason).

## 6. Evidence

```
work/phase5as/ev/repro1.err      savestate + IGNORE_BUILD reproduces the halt
work/phase5as/ev/tr1.out/.err    pc trace + registers at 0x02062C44
work/phase5as/ev/itcm8130.txt    ITCM bytes at 0x01FF8130 (ARM handler + blx r1)
work/phase5as/ev/cold1..cold9.*  cold-boot runs across the A/B configurations
work/phase5as/ev/final_cold.*    final binary: 508,793,671 · 0x02062C48
work/phase5as/ev/final_state.*   final binary: 626,972,668 · 0x02062C48
```

No game bytes were committed; the tests quote single instruction encodings and
the measured addresses above.
