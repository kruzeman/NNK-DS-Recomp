// Decoder regression tests — one test per bug class that actually halted the
// boot chain, with the exact machine words that were measured at the site.
//
// Run with:  cargo test            (in nnktool/)
//            cargo test ldrsb     (single class)
//
// Every fixture word below was verified against binutils:
//   printf '\xD7\x80\x9A\xE1...' > w.bin
//   arm-none-eabi-objdump -D -b binary -marm w.bin
// The disassembly is quoted next to each constant.  No game bytes, no ROM:
// these are single 4-byte ARM / 2-byte Thumb encodings plus the measured
// addresses documented in PHASE4F_LDRSB_DECODE.md, PHASE4V.md, PHASE4Y.md and
// PHASE4K_DECRYPT_SCHEDULE.md.
//
// Convention: each class has a POSITIVE test (the correct decode / emission)
// and a NEGATIVE guard that pins the shape of the OLD, buggy behaviour so the
// fix cannot be silently reverted without a test going red.

use crate::decode::*;

/// The measured address of the overlay dispatcher's `blx r3` in Phase-4Y: the
/// fix moved its link from 0x020D1A21 (addr+4|1) to 0x020D1A1F (addr+2|1).
const BLX_SITE: u32 = 0x020D_1A1C;

fn body_of(o: &super::EmitOutcome) -> String {
    o.body.join("\n")
}

// ---------------------------------------------------------------- class 1 ---
// LDRSB vs LDRD (Phase-4F).  ARM ARM extra load/store, bits 7-4 = 1SH1:
//   SH=01 halfword, SH=10 -> LDRSB when L=1, LDRD when L=0, SH=11 -> LDRSH /
//   with L=0 STRD.  The old decoder keyed on SH alone for the doubleword
//   forms and matched (SH=10, L=1) as LDRD, loading eight bytes and clobbering
//   the register pair's second half (Phase-4F: the SDK path parse at
//   0x0200C638 returned 0 and the guest took the 0x0210E388 abort arm).

const W_LDRSB_R8_SL_R7: u32 = 0xE19A_80D7; // ldrsb r8, [sl, r7]  (measured 0x0200C7C8)
const W_LDRD_R8_SL: u32 = 0xE1CA_80D0; //     ldrd  r8, [sl]      (L=0, the real doubleword)

#[test]
fn ldrsb_register_offset_decodes_as_signed_byte() {
    let insn = decode_arm(W_LDRSB_R8_SL_R7, 0x0200_C7C8);
    assert_eq!(insn.text, "ldrsb r8, [sl, r7]");
    match insn.kind {
        Kind::Hw { kind: HwKind::Ldrsb, rd: 8, rn: 10, ea } => match ea {
            Ea::Reg { rm: 7, pre: true, wb: false, up: true, .. } => {}
            other => panic!("expected register-offset ea [sl, r7], got {:?}", other),
        },
        other => panic!("E19A80D7 must be Kind::Hw Ldrsb, got {:?}", other),
    }
}

#[test]
fn ldrsb_is_not_ldrd_but_real_ldrd_still_decodes() {
    // NEGATIVE guard: the old decoder's regression signature was
    // Kind::Hw { kind: Ldrd } with text "ldrd r8, [sl, r7]" for this word,
    // i.e. an 8-byte load into (r8, r9).  Pin that it can never come back.
    let sb = decode_arm(W_LDRSB_R8_SL_R7, 0x0200_C7C8);
    assert!(
        !matches!(sb.kind, Kind::Hw { kind: HwKind::Ldrd, .. }),
        "regression: E19A80D7 decoded as LDRD again (8-byte load)"
    );
    assert!(!sb.text.starts_with("ldrd"), "regression: text became {}", sb.text);

    // POSITIVE contrast: the genuine doubleword (L=0) must still be LDRD —
    // a "fix" that simply dropped the doubleword form would be just as wrong.
    let dw = decode_arm(W_LDRD_R8_SL, 0x0200_C7C8);
    assert_eq!(dw.text, "ldrd r8, [sl]");
    match dw.kind {
        Kind::Hw { kind: HwKind::Ldrd, rd: 8, rn: 10, .. } => {}
        other => panic!("E1CA80D0 must stay LDRD (L=0), got {:?}", other),
    }
}

// ---------------------------------------------------------------- class 2 ---
// UMULL/SMULL/UMLAL/SMLAL RdLo/RdHi destination (Phase-4V2).  The decoder fed
// `Kind::Mul { rd: rdhi, rn: rdlo }` while the codegen (recompile.rs,
// Kind::Mul) assigns `r[rd] = _lo; r[rn] = _hi;` — so every static-image long
// multiply wrote its two destinations swapped (and the UMLAL/SMLAL accumulate
// pair with them).  Signature: for `umull r1, r0, r8, r2` the generated case
// emitted `r[0] = _lo; r[1] = _hi;`.
//
// All four words below are `... r1, r0, r8, r2` (RdLo=r1, RdHi=r0):
//   bit 23 = long, bit 22 = signed, bit 21 = accumulate.

const W_UMULL_R1_R0: u32 = 0xE080_1298; // umull r1, r0, r8, r2  (measured 0x0201E814 shape)
const W_UMLAL_R1_R0: u32 = 0xE0A0_1298; // umlal r1, r0, r8, r2
const W_SMULL_R1_R0: u32 = 0xE0C0_1298; // smull r1, r0, r8, r2
const W_SMLAL_R1_R0: u32 = 0xE0E0_1298; // smlal r1, r0, r8, r2
const W_UMULL_R0_R1: u32 = 0xE081_0094; // umull r0, r1, r4, r0  (measured at 0x0215B100)

#[test]
fn long_multiply_maps_rdlo_to_low_destination() {
    let cases = [
        (W_UMULL_R1_R0, MulKind::Umull, "umull"),
        (W_UMLAL_R1_R0, MulKind::Umlal, "umlal"),
        (W_SMULL_R1_R0, MulKind::Smull, "smull"),
        (W_SMLAL_R1_R0, MulKind::Smlal, "smlal"),
    ];
    for (w, want, name) in cases {
        let insn = decode_arm(w, 0x0201_E814);
        assert_eq!(insn.text, format!("{} r1, r0, r8, r2", name));
        match &insn.kind {
            // rd = RdLo (low half), rn = RdHi (high half) — the codegen's
            // convention, see the comment block in decode.rs.
            Kind::Mul { kind, rd, rn, rm, ra, .. } => {
                assert_eq!(*kind as u8, want as u8, "{} kind", name);
                // NEGATIVE guard: the old swap signature for this exact word.
                assert_eq!((*rd, *rn), (1, 0), "{} destination order (rd=lo, rn=hi)", name);
                assert_ne!(
                    (*rd, *rn),
                    (0, 1),
                    "regression: {:08X} ({}) swapped RdLo/RdHi again",
                    w,
                    name
                );
                assert_eq!((*rm, *ra), (8, 2), "{} operands r8, r2", name);
            }
            other => panic!("{:08X} must be Kind::Mul, got {:?}", w, other),
        }
    }
}

#[test]
fn measured_divide_loop_word_keeps_its_pair() {
    // 0x0215B100 `umull r0, r1, r4, r0`: RdLo=r0, RdHi=r1.  Phase-4K measured
    // r0=0 (native) vs 0x00324415 (melonDS) when this pair was mishandled.
    match decode_arm(W_UMULL_R0_R1, 0x0215_B100).kind {
        Kind::Mul { kind: MulKind::Umull, rd: 0, rn: 1, .. } => {}
        other => panic!("E0810094 must be umull r0(lo), r1(hi), got {:?}", other),
    }
}

#[test]
fn long_multiply_codegen_writes_low_then_high_to_the_right_registers() {
    // End-to-end guard: decode -> emit.  The bug lived in the decode field
    // mapping but only *manifested* in these emitted lines.
    let mut out: Vec<String> = Vec::new();
    let o = super::emit_arm(W_UMULL_R1_R0, 0x0201_E814, &mut out, true);
    let body = body_of(&o);
    assert!(
        body.contains("r[1] = _lo;"),
        "RdLo (r1) must receive the low word; body was:\n{}",
        body
    );
    assert!(
        body.contains("r[0] = _hi;"),
        "RdHi (r0) must receive the high word; body was:\n{}",
        body
    );
    // The old, swapped emission was exactly `r[0] = _lo;` / `r[1] = _hi;`.
    assert!(
        !body.contains("r[0] = _lo;"),
        "regression: swapped emission came back:\n{}",
        body
    );
}

// ---------------------------------------------------------------- class 3 ---
// Thumb `blx Rm` link address (Phase-4Y).  ARM ARM B1.3.3 T1: LR = the address
// of the instruction plus 2, with bit0 set (BLX returns to Thumb).  ARM-state
// `BLX Rm` (A1) is a 4-byte instruction: LR = addr+4 | 1.  The old code
// emitted addr+4|1 unconditionally, so every Thumb vtable call returned two
// bytes past its caller's epilogue (measured: the overlay dispatcher's
// `blx r3` at 0x020D1A1C returned to 0x020D1A20 instead of the epilogue's
// `pop {r4,r5,r6,pc}` at 0x020D1A1E, calling the not-yet-downloaded module
// entry 0x02988AA0).

const T_BLX_R3: u16 = 0x4798; // blx r3   (objdump: 4798 blx r3)
const T_BX_R3: u16 = 0x4718; //  bx r3    (objdump: 4718 bx  r3)
const A_BLX_R12: u32 = 0xE12F_FF3C; // blx ip (r12) — the Phase-4K site 0x0215B700

#[test]
fn thumb_blx_rm_links_the_next_thumb_instruction() {
    let mut out: Vec<String> = Vec::new();
    let o = super::emit_thumb(T_BLX_R3, 0, BLX_SITE, 2, &mut out, true);
    let body = body_of(&o);
    let want = format!("r[14] = 0x{:08X}u;", BLX_SITE + 2 | 1); // 0x020D1A1F
    assert!(
        body.contains(&want),
        "Thumb blx r3 must link {} (addr+2|1); body was:\n{}",
        want,
        body
    );
    // NEGATIVE guard: the old unconditional addr+4|1.
    let old = format!("r[14] = 0x{:08X}u;", BLX_SITE + 4 | 1); // 0x020D1A21
    assert!(
        !body.contains(&old),
        "regression: Thumb blx r3 links {} (addr+4|1) again:\n{}",
        old,
        body
    );
}

#[test]
fn arm_blx_rm_still_links_addr_plus_4() {
    // POSITIVE contrast — the ARM form must NOT be "fixed" to +2.
    let mut out: Vec<String> = Vec::new();
    let o = super::emit_arm(A_BLX_R12, BLX_SITE, &mut out, true);
    let body = body_of(&o);
    let want = format!("r[14] = 0x{:08X}u;", BLX_SITE + 4 | 1); // 0x020D1A21
    assert!(
        body.contains(&want),
        "ARM blx ip must link {} (addr+4|1); body was:\n{}",
        want,
        body
    );
}

#[test]
fn thumb_bx_rm_without_link_does_not_touch_lr() {
    // Guard against a "+2 for every Bx" over-fix: plain BX must not write LR.
    let mut out: Vec<String> = Vec::new();
    let o = super::emit_thumb(T_BX_R3, 0, BLX_SITE, 2, &mut out, true);
    let body = body_of(&o);
    assert!(
        !body.contains("r[14]"),
        "Thumb bx r3 must not write the link register; body was:\n{}",
        body
    );
}

// ---------------------------------------------------------------- class 4 ---
// BX/BLX(register) dispatch (Phase-4K).  0x012FFF1m / 0x012FFF3m must be
// recognised as BX / BLX(reg) BEFORE the data-processing catch-all, in both
// backends.  In the runtime SMC re-decoder (tools/nnk_smc.inc) the encoding
// had no case and fell into data processing as `teq pc, r12, lsr pc`
// (op=9, rd=15) with a stale r[15]: it "executed" with a garbage npc, so the
// run fell through 0x0215B700 (E12FFF3C `blx r12`) instead of calling the
// decrypt helper at 0x0215B528 — first divergence vs melonDS at step
// 13,142,583.

#[test]
fn bx_and_blx_register_are_dispatched_before_data_processing() {
    // 0xE12FFF1E `bx lr` — every ARM return.
    let bx = decode_arm(0xE12F_FF1E, 0x0200_0000);
    assert_eq!(bx.text, "bx lr");
    match bx.kind {
        Kind::Bx { rm: 14, link: false } => {}
        other => panic!("E12FFF1E must be Kind::Bx rm=14 link=false, got {:?}", other),
    }
    // 0xE12FFF3C `blx ip` — the measured Phase-4K site.
    let blx = decode_arm(A_BLX_R12, 0x0215_B700);
    assert_eq!(blx.text, "blx ip");
    match blx.kind {
        Kind::Bx { rm: 12, link: true } => {}
        other => panic!("E12FFF3C must be Kind::Bx rm=12 link=true, got {:?}", other),
    }
}

#[test]
fn bx_register_never_decodes_as_data_processing() {
    // NEGATIVE guard: the old SMC fall-through produced a data-processing
    // decode (`teq pc, r12, lsr pc`).  Neither word may be Kind::Dp, and the
    // text must never read as a teq.
    for w in [0xE12F_FF1E, A_BLX_R12] {
        let insn = decode_arm(w, 0x0215_B700);
        assert!(
            !matches!(insn.kind, Kind::Dp { .. }),
            "regression: {:08X} fell through to data processing ({:?})",
            w,
            insn.kind
        );
        assert!(
            !insn.text.contains("teq"),
            "regression: {:08X} decoded as a garbage teq: {}",
            w,
            insn.text
        );
    }
}

#[test]
fn runtime_smc_redecoder_keeps_its_bx_case_before_the_dp_catchall() {
    // tools/nnk_smc.inc has no C test harness (it is a generated-runtime
    // include with live guest state), so the runtime half is locked at the
    // source level: the BX/BLX(reg) case must exist and must sit BEFORE the
    // data-processing catch-all in smc_exec_arm.  Deleting it — the Phase-4K
    // pre-fix state — makes this test red.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));

    let bx = src
        .find("0x0FFFFF30u) == 0x012FFF10u")
        .expect("regression: smc_exec_arm lost the BX/BLX(register) case (0x012FFF1m)");
    assert!(
        src.contains("0x0FFFFF30u) == 0x012FFF30u"),
        "regression: smc_exec_arm lost the BLX(register) case (0x012FFF3m)"
    );
    let dp = src
        .find("0x0C000000u) == 0x00000000u")
        .expect("smc_exec_arm data-processing catch-all not found (anchor moved?)");
    assert!(
        bx < dp,
        "regression: BX/BLX(reg) case now sits AFTER the data-processing catch-all"
    );
    // BLX (register) must link an EVEN pc+4.  melonDS A_BLX_REG
    // (src/ARMInterpreter_Branch.cpp:57) does `u32 lr = R[15]-4; JumpTo(Rm);
    // R[14] = lr;` - R[15] is pc+8 in ARM state, so lr == pc+4 with no
    // interworking bit.  Phase-5D measured the old `| 1u` form: the decryptor
    // stub at 0x0215AC1C (`E12FFF3C blx r12`) pushed an LR of 0x0215AC21, and
    // the helper's `pop {pc}` therefore re-entered the ARM code at
    // 0x0215AC20 (`E1A02005 mov r2,r5`) in Thumb state.
    let block = &src[bx..dp];
    assert!(
        block.contains("if (link) r[14] = pc + 4u;"),
        "regression: smc BLX(register) no longer links an even pc+4"
    );
    assert!(
        !block.contains("| 1u;"),
        "regression: smc BLX(register) tags the ARM caller's LR with bit0 again"
    );
    assert!(
        block.contains("smc_branch_mode(tgt);"),
        "regression: ARM bx/blx rM no longer carries the target's state"
    );
}

// ---------------------------------------------------------------- class 5 ---
// Thumb-1 PUSH/POP, format 13 (Phase-5A).  Encoding `1011 L 10 R <low list>`:
// bit 11 is L (0 = PUSH, 1 = POP) and bit 8 is R (LR for PUSH, PC for POP).
// The runtime's C re-decoder (tools/nnk_smc.inc) matched both arms with
// `hw & 0xF600`, a mask that leaves bit 11 out, and tested PUSH first — so
// every POP took the PUSH arm and the POP arm was unreachable dead code.
// Measured at the overlay dispatcher's epilogue 0x020D1A1E (`pop
// {r4,r5,r6,pc}` = 0xBD70): the frame the `push {r4,r5,r6,lr}` at 0x020D19B8
// had written (r4, r5, r6, 0x020D1647 at 0x027E3AA8..0x027E3AB4) was never
// popped; the guest fell through to 0x020D1A20 (the literal pool) instead of
// returning to 0x020D1646, walked 0x020D1A20..0x020D1A8E as code and halted
// at step 39,001,760.  The Rust half re-tested the L bit and so stayed
// correct, but its mask is tightened to 0xFE00 as well so both halves key on
// the same field.

const T_PUSH_R4_R5_R6_LR: u16 = 0xB570; // push {r4, r5, r6, lr}  (0x020D19B8)
const T_POP_R4_R5_R6_PC: u16 = 0xBD70; //  pop  {r4, r5, r6, pc}  (0x020D1A1E)

#[test]
fn thumb_push_and_pop_split_on_the_l_bit() {
    let push = decode_thumb(T_PUSH_R4_R5_R6_LR, 0, 0x020D_19B8, 2);
    assert_eq!(push.text, "push {r4, r5, r6, lr}");
    match push.kind {
        Kind::LdmStm { load: false, rn: 13, wb: true, list: 0x4070, pu: 2, .. } => {}
        other => panic!("B570 must be push {{r4,r5,r6,lr}} via sp (list 0x4070), got {:?}", other),
    }

    let pop = decode_thumb(T_POP_R4_R5_R6_PC, 0, 0x020D_1A1E, 2);
    assert_eq!(pop.text, "pop {r4, r5, r6, pc}");
    match pop.kind {
        Kind::LdmStm { load: true, rn: 13, wb: true, list: 0x8070, pu: 1, .. } => {}
        other => panic!("BD70 must be pop {{r4,r5,r6,pc}} from sp (list 0x8070), got {:?}", other),
    }
}

#[test]
fn thumb_pop_never_decodes_as_push() {
    // NEGATIVE guard on the exact field that the old 0xF600 mask hid: with
    // 0xF600 the two encodings are indistinguishable, which is why the C
    // re-decoder's PUSH arm swallowed every POP.
    assert_eq!(
        T_POP_R4_R5_R6_PC & 0xF600,
        T_PUSH_R4_R5_R6_LR & 0xF600,
        "fixture sanity: 0xF600 cannot separate push from pop"
    );
    assert_eq!(T_PUSH_R4_R5_R6_LR & 0xFE00, 0xB400, "PUSH (L=0) under 0xFE00");
    assert_eq!(T_POP_R4_R5_R6_PC & 0xFE00, 0xBC00, "POP (L=1) under 0xFE00");

    let pop = decode_thumb(T_POP_R4_R5_R6_PC, 0, 0x020D_1A1E, 2);
    assert!(!pop.text.starts_with("push"), "regression: BD70 decoded as {}", pop.text);
    match pop.kind {
        Kind::LdmStm { load, .. } => assert!(load, "regression: BD70 decoded as a store"),
        other => panic!("BD70 must stay Kind::LdmStm, got {:?}", other),
    }
}

#[test]
fn runtime_smc_redecoder_masks_the_form13_rbit() {
    // Source-level lock for the C half (same honest option as class 4 — the
    // include has no C test harness): format 13 must be matched with 0xFE00 so
    // bit 11 is part of the comparison, and the POP arm must exist after the
    // PUSH arm.  The Phase-5A pre-fix state (0xF600 for both) makes this red.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));

    assert!(
        !src.contains("0xF600u) == 0xB400u") && !src.contains("0xF600u) == 0xBC00u"),
        "regression: smc_exec_thumb is masking format 13 with 0xF600 again (bit 11 hidden)"
    );
    let push = src
        .find("0xFE00u) == 0xB400u")
        .expect("regression: smc_exec_thumb lost its PUSH arm (0xB4xx under 0xFE00)");
    let pop = src
        .find("0xFE00u) == 0xBC00u")
        .expect("regression: smc_exec_thumb lost its POP arm (0xBCxx under 0xFE00) — a pop falls through or is pushed");
    assert!(push < pop, "regression: POP arm now sits before the PUSH arm");
    // The POP arm must take the pc from the top of the list (0x8000 for the
    // R bit) and write it to npc, not to r[15]'s slot in the register file.
    let block = &src[pop..(pop + 1600).min(src.len())];
    assert!(
        block.contains("0x8000u") && block.contains("*npc = raw & ~1u;"),
        "regression: POP no longer pops bit-8 R as the return address into npc"
    );
    // Phase-5J: the forced Thumb tag is the SINGLE-register form only
    // (melonDS T_POP `if (cpu->Num==1) pc |= 0x1`); a multi-register
    // pop{..,pc} interworks on the loaded word's bit0.
    assert!(
        block.contains("uint32_t raw = (cnt == 1u) ? (tv | 1u) : tv;"),
        "regression: the Thumb POP arm forces Thumb for every register list again"
    );
}

// ---------------------------------------------------------------- class 6 ---
// Thumb BLX (immediate) forces ARM state for its target (Phase-5C).
//
// The T bit is part of the processor state, not of the PC: `bl` stays in
// Thumb, `blx <imm>` always switches to ARM (ARM ARM A7.1.4).  The live C
// re-decoder derived the state from the compiled map plus a sticky last-state
// guess, so an ARM target reached by `blx <imm>` was re-decoded as Thumb.
//
// Measured site: 0x020D2418 `blx 0x020E7968` (halfwords 0xF015 0xEAA6) in the
// reloaded overlay; the target holds the ARM `push {r3,r4,r5,lr}` (0xE92D4038).
// The shipped Phase-5B build re-decoded that word as Thumb
// (`stmdb sp!,{ip,lr}`), walked the pointer table at 0x02160394 for 25,284 PCs
// through the 0x0214xxxx-0x0217xxxx band and halted at 0x02178E9E on the data
// word 0x1C36F000 — a "coprocessor" encoding, i.e. data executed as code.
const T_BLX_IMM_H1: u16 = 0xF015; // blx prefix, S=0
const T_BLX_IMM_H2: u16 = 0xEAA6; // blx suffix: 11101 (bit12 = 0)
const T_BL_IMM_H2: u16 = 0xFAA6; //  bl  suffix: 11111 (bit12 = 1)
const BLXI_SITE: u32 = 0x020D_2418;
const BLXI_TARGET: u32 = 0x020E_7968;

#[test]
fn thumb_blx_immediate_target_is_arm_state() {
    let insn = decode_thumb(T_BLX_IMM_H1, T_BLX_IMM_H2, BLXI_SITE, 4);
    match insn.kind {
        Kind::BlxImm { target } => assert_eq!(
            target, BLXI_TARGET,
            "0xF015 0xEAA6 at 0x020D2418 is `blx 0x020E7968`"
        ),
        other => panic!("F015 EAA6 must decode as Kind::BlxImm, got {:?}", other),
    }
    match thumb_flow(BLXI_SITE, &insn) {
        Flow::Call(target, callee_is_thumb) => {
            assert_eq!(target, BLXI_TARGET);
            assert!(
                !callee_is_thumb,
                "regression: Thumb `blx <imm>` marked its target as Thumb state"
            );
        }
        other => panic!("F015 EAA6 must be Flow::Call, got {:?}", other),
    }
}

#[test]
fn thumb_bl_immediate_stays_in_thumb_state() {
    // NEGATIVE neighbour of the same encoding pair: only bit 12 of the suffix
    // separates BL from BLX, so a fix that switches state for both would be as
    // wrong as the old behaviour (it would run every Thumb call in ARM).
    let insn = decode_thumb(T_BLX_IMM_H1, T_BL_IMM_H2, BLXI_SITE, 4);
    match thumb_flow(BLXI_SITE, &insn) {
        Flow::Call(target, callee_is_thumb) => {
            assert_eq!(target, BLXI_TARGET);
            assert!(callee_is_thumb, "regression: Thumb `bl` marked its target ARM");
        }
        other => panic!("F015 FAA6 must be Flow::Call, got {:?}", other),
    }
    assert_eq!(T_BLX_IMM_H2 & 0x1000, 0x0000, "blx suffix: bit12 = 0");
    assert_eq!(T_BL_IMM_H2 & 0x1000, 0x1000, "bl suffix: bit12 = 1");
}

#[test]
fn runtime_smc_redecoder_carries_the_blx_state() {
    // Source-level lock for the C half (same honest option as classes 4/5 —
    // tools/nnk_smc.inc has no C test harness).  The live decoder must record
    // the interworking state at the BL/BLX site and honour it at dispatch; the
    // Phase-5B state (state inferred from the compiled map + sticky only) is
    // what produced the 0x0214..0x0217 data walk.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        src.contains("smc_branch_mode(*npc | ((hw2 & 0x1000u) ? 1u : 0u));"),
        "regression: the Thumb BL/BLX arm no longer carries the target's state"
    );
    assert!(
        src.contains("static int smc_carried_thumb(uint32_t pc, int *state)"),
        "regression: the live re-decoder lost its carried-state lookup"
    );
    let uses = src.matches("if (smc_carried_thumb(pc, &st))").count();
    assert!(
        uses >= 2,
        "regression: carried state is not consumed by both dispatch hooks (found {})",
        uses
    );
    assert!(
        src.contains("steps != smc_t_step + 1ull"),
        "regression: the carried state is no longer one-step-scoped — a loop \
         revisiting the same target would inherit a stale state"
    );
}

#[test]
fn runtime_smc_redecoder_interworks_on_ldm_pc_and_links_arm_blx_even() {
    // Phase-5D, class 7: the ARM9's *data-dependent* interworking.  Ground
    // truth is melonDS itself (the project's reference implementation):
    //   src/ARM.cpp:729 JumpTo(): `if (addr & 0x1) { ... CPSR |= 0x20; }`
    //   src/ARMInterpreter_LoadStore.cpp:464 A_LDM: `JumpTo(pc, ...)` for a
    //     PC in the register list -> the loaded word's bit0 selects the state
    //   src/ARMInterpreter_LoadStore.cpp:95..121 A_LDR0/A_LDR_POST:
    //     `if (Num==1) val &= ~0x1;` -> ARM9 LDR-into-PC does NOT interwork
    //   src/ARMInterpreter_ALU.cpp:321/336 data-processing into PC:
    //     `JumpTo(res & ~1)` -> no interworking
    //   src/ARMInterpreter_LoadStore.cpp:745 A_POP: `if (Num==1) pc |= 0x1`
    //     -> Thumb pop{pc} always continues in Thumb
    //   src/ARMInterpreter_Branch.cpp:57 A_BLX_REG:
    //     `u32 lr = R[15]-4; ... R[14] = lr;` -> LR = pc+4, bit0 CLEAR
    // Both halves are measured on this ROM: melonDS executes 0x020D241C /
    // 0x020D241E as Thumb (2-byte strides after the ARM helper's `pop {pc}`
    // at 0x020E79A8 returns 0x020D241D), while the pre-Phase-5D runtime kept
    // ARM state there and decoded the Thumb `blx 0x0204D3D0` at 0x020D2420 as
    // an ARM `svc 0xD6F77A` (halt, step 39,237,994).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    // (1) the ARM LDM with PC in the list carries the loaded word's state,
    //     except for the one-register list where melonDS masks bit0
    //     (A_LDM: `if (cpu->Num == 1) pc &= ~0x1;`) — Phase-5J.
    assert!(
        src.contains("uint32_t raw = (cnt == 1u) ? (lv & ~1u) : lv;")
            && src.contains("*npc = raw & ~1u; smc_branch_mode(raw);"),
        "regression: an ARM `pop/LDM {{..,pc}}` no longer interworks on the loaded word"
    );
    // (2) ARM BLX (register) must link an EVEN pc+4 — the `| 1u` form tagged
    //     an ARM caller's return address Thumb (measured: 0x0215AC21).
    assert!(
        src.contains("if (link) r[14] = pc + 4u;"),
        "regression: ARM BLX (register) links a Thumb-tagged LR again"
    );
    // (the block-scoped negative lock lives in
    // runtime_smc_redecoder_keeps_its_bx_case_before_the_dp_catchall)
    // (3) the forms melonDS does NOT interwork on must stay masked
    assert!(
        src.contains("if (rd == 15u) *npc = tv & ~1u;"),
        "regression: an ARM LDR into PC started interworking (melonDS masks bit0)"
    );
    // (4) a Thumb pop{..,pc} takes the state from the popped word's bit0;
    //     only the SINGLE-register form forces Thumb (melonDS T_POP:
    //     `if (cpu->Num==1) pc |= 0x1; cpu->JumpTo(pc);`).  Phase-5D made
    //     the |1u unconditional and re-entered the ARM caller 0x0207CAC4 in
    //     Thumb state (Phase-5J).
    assert!(
        src.contains("uint32_t raw = (cnt == 1u) ? (tv | 1u) : tv;")
            && src.contains("smc_branch_mode(raw);"),
        "regression: Thumb pop{{..,pc}} lost the Num==1-only forced Thumb tag"
    );
}

// ---------------------------------------------------------------- class 8 ---
// Thumb BL(1)/BLX(1) immediate PC base (Phase-5E).  ARMv5T ARM ARM:
//   BL(1)  (suffix 11111): target = PC + offset, PC = addr + 4
//   BLX(1) (suffix 11101): target = Align(PC,4) + offset  (H forced 0,
//                          because the target is ARM code, word-aligned)
// The decoder used addr+4 for BOTH forms, so a BLX whose instruction lives at
// a 2 (mod 4) address landed 2 bytes past the real target.  Measured word:
// the overlay dispatch helper at 0x020D2426 = F779 EFE6, which binutils
// decodes as `blx 0x0204C3F4` and which melonDS *executes* at 0x0204C3F4
// (MELONDS_ARM9TRACE window 0x0204C3C0..0x0204C440: 3f4 3f8 3fc 400 ...).
// Native reached 0x0204C3F6 instead — two bytes into the ARM function body,
// so `push {r4,r5,r6,r7,lr}` never ran; the ARM epilogue word E8BD8078 later
// decoded as a Thumb `pop {r3,r4,r5,r6,pc}` off a frame that was never
// pushed and returned the stale stack word 0x00000002 (step 39,238,304).
#[test]
fn thumb_blx_immediate_aligns_the_pc_base() {
    let insn = decode_thumb(0xF779, 0xEFE6, 0x020D_2426, 4);
    match insn.kind {
        Kind::BlxImm { target } => assert_eq!(
            target, 0x0204_C3F4,
            "F779 EFE6 at 0x020D2426 is `blx 0x0204C3F4` (binutils + melonDS)"
        ),
        other => panic!("expected Thumb BLX immediate, got {:?}", other),
    }
}

#[test]
fn thumb_bl_immediate_keeps_the_unaligned_pc_base() {
    // F7FF FF8D at 0x020D136E = `bl 0x020D128C` (binutils).  The aligned base
    // (0x020D1370) would land 2 bytes low at 0x020D128A, so this guard pins
    // that BL(1) is NOT changed by the BLX(1) alignment rule.
    let insn = decode_thumb(0xF7FF, 0xFF8D, 0x020D_136E, 4);
    match insn.kind {
        Kind::B { link: true, target, .. } => assert_eq!(
            target, 0x020D_128C,
            "F7FF FF8D at 0x020D136E is `bl 0x020D128C` (binutils)"
        ),
        other => panic!("expected Thumb BL immediate, got {:?}", other),
    }
}

#[test]
fn runtime_smc_redecoder_aligns_the_thumb_blx_base() {
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        src.contains("uint32_t base = (hw2 & 0x1000u) ? (pc + 4u) : ((pc + 4u) & ~3u);"),
        "regression: the C re-decoder no longer aligns the BLX(1) PC base"
    );
    assert!(
        !src.contains("*npc = (pc + 4u + (uint32_t)o) & ~1u;"),
        "regression: the unaligned Thumb BL/BLX base came back"
    );
}

// ---------------------------------------------------------------- class 9 ---
// Thumb format-5 `ADD rd, pc` must read the PC, not the r[15] register slot
// (Phase-5I).
//
// The Rust emitter already reads R15 as (addr+4) (`reg_c_t`), so the compiled
// cases were right; only the live C re-decoder was wrong.  `add r0, pc`
// (0x4478 at 0x020D24E0) took its second operand straight out of `r[15]`,
// which the runtime never maintains (it stays 0), so the result was the stale
// r0 instead of the literal base 0x020D24EA.  The halfword load through
// `ldrh r0,[r0,#6]` then returned 0, and `add pc, r0` (0x4487 at 0x020D24E8)
// fell through into the jump-table data at 0x020D24EA and walked it as code
// until it reached the unconditional `bl 0x020090D8` halt stub at 0x020D3250.
// melonDS, with r0 = 0xE8, branches to case 3 (0x020D25D4) and the card state
// machine re-arms the block read (#9093, B7 0x0DA44600).
#[test]
fn runtime_smc_redecoder_reads_pc_in_thumb_format5_add() {
    // Rust side: the fixture must stay `add r0, pc` with rm = 15 (the operand
    // that has to be PC-sourced), so the C guard below is about a real site.
    let insn = decode_thumb(0x4478, 0x0000, 0x020D_24E0, 4);
    match insn.kind {
        Kind::Dp { rd: 0, rn: 0, ref src, .. } => match *src {
            Operand::Reg { rm, .. } => assert_eq!(
                rm, 15,
                "4478 at 0x020D24E0 is `add r0, pc` (binutils); rm must be 15"
            ),
            ref other => panic!("4478 source must be Reg(15), got {:?}", other),
        },
        other => panic!("4478 must stay a data-processing ADD, got {:?}", other),
    }

    // C side: format-5 case 0 must source BOTH operands through smc_reg.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let fmt5 = src
        .find("format 5: hi-reg / BX")
        .expect("regression: smc_exec_thumb lost its format-5 (hi-reg/BX) arm");
    let block = &src[fmt5..(fmt5 + 1400).min(src.len())];
    assert!(
        block.contains("smc_reg(rd, pc, 1) + smc_reg(rs, pc, 1)"),
        "regression: Thumb format-5 ADD no longer reads its source through smc_reg (R15 must be PC)"
    );
    assert!(
        !block.contains("+ r[rs]"),
        "regression: Thumb format-5 ADD reads r[15]'s slot again (ADD rd, pc gives the stale value)"
    );
}

// --------------------------------------------------------------- class 10 ---
// Thumb format 8 (LDRH/STRH, immediate) must not be matched as format 10
// (LDR/STR, SP-relative) (Phase-5I).
//
// Both live in 0x8xxx and the old format-10 test was `(hw & 0xF000) == 0x8000`,
// so `ldrh r0,[r0,#6]` (0x88C0, measured at 0x020D24E2) executed as
// `ldr r0,[sp,#imm]` and returned whatever sat at sp+0x300.  The guest's
// jump-table index then died and the following `add pc,r0` walked the
// halfword data at 0x020D24EA as code into the 0x020D3250 halt stub.
#[test]
fn runtime_smc_redecoder_splits_the_thumb_imm_halfword_form() {
    // Rust fixture: 0x88C0 = `ldrh r0, [r0, #6]` (binutils), not SP-relative.
    let insn = decode_thumb(0x88C0, 0x0000, 0x020D_24E2, 4);
    match insn.kind {
        Kind::Hw { kind: HwKind::Ldrh, rd: 0, rn: 0, ref ea } => match *ea {
            Ea::Imm { off: 6, .. } => {}
            ref other => panic!("88C0 must keep its #6 halfword offset, got {:?}", other),
        },
        other => panic!("88C0 must stay a halfword load from r0, got {:?}", other),
    }

    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        src.contains("(hw & 0xF000u) == 0x9000u) {                 /* format 10: SP-relative */")
            || src.contains("(hw & 0xF000u) == 0x9000u) { /* format 10: SP-relative */"),
        "regression: the C re-decoder's SP-relative arm no longer masks 0x9000 (0x8xxx halfword forms are swallowed again)"
    );
    assert!(
        src.contains("/* format 8: LDRH/STRH imm5 */"),
        "regression: the C re-decoder lost its Thumb immediate halfword load/store arm"
    );
    // The halfword offset is imm5*2, and stores must use st16.
    let fmt8 = src
        .find("/* format 8: LDRH/STRH imm5 */")
        .expect("format 8 arm missing");
    let block = &src[fmt8..(fmt8 + 800).min(src.len())];
    assert!(
        block.contains("(((hw >> 6) & 0x1Fu) << 1)") && block.contains("ld16(ea)"),
        "regression: the Thumb immediate halfword arm no longer reads a 16-bit element (imm5 scaled by 2)"
    );
}

#[test]
fn runtime_smc_redecoder_splits_add_sp_from_sub_sp() {
    // Phase-5J, class 12: Thumb `1011 0000 0 imm7` is ADD SP,#imm*4 and
    // `1011 0000 1 imm7` is SUB SP,#imm*4 (ARM ARM A6.6.4).  The C
    // re-decoder matched the whole 0xB000..0xB0FF block as ADD, so every
    // `sub sp,#imm` RAISED sp.  Measured at 0x020DDE72 (`sub sp,#16`, word
    // 0xB084) inside the frame that ends at 0x020DDE6E: sp sat 0x20 too high,
    // the [sp,#8]/[sp,#12] locals read the wrong words and the epilogue's
    // `pop {r4,r5,r6,r7,pc}` popped 0 into pc (halt step 41,720,639).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        src.contains("/* format 12: ADD/SUB SP,#imm */"),
        "regression: the C re-decoder no longer separates ADD SP from SUB SP"
    );
    let at = src.find("/* format 12: ADD/SUB SP,#imm */").expect("format 12 arm missing");
    let block = &src[at..(at + 900).min(src.len())];
    assert!(
        block.contains("if (hw & 0x80u) r[13] -= (hw & 0x7Fu) << 2;")
            && block.contains("else            r[13] += (hw & 0x7Fu) << 2;"),
        "regression: SUB SP (0xB080..0xB0FF) is added instead of subtracted again"
    );
    assert_eq!(0xB084 & 0x80, 0x80, "fixture sanity: 0xB084 has bit7 set (SUB)");
    assert_eq!(0xB019 & 0x80, 0x00, "fixture sanity: 0xB019 has bit7 clear (ADD)");
}

#[test]
fn interworking_targets_carry_their_state_across_the_dispatch() {
    // Phase-5J, classes 13/14: the target of an interworking transfer carries
    // its instruction state in bit0 of the transferred word — melonDS
    // JumpTo() (`if (addr & 0x1) CPSR |= 0x20`), T_POP `if (Num==1) pc |= 0x1`
    // and A_LDM `if (Num==1) pc &= ~0x1` for one-register lists
    // (src/ARMInterpreter_LoadStore.cpp:436/745).
    // (a) the EMITTER records that word for the compiled cases, which used to
    //     drop it (`pc = _t & ~1u;` alone) — measured at 0x020F5C88
    //     `pop {r3,r4,r5,r6,r7,pc}` returning to the unrooted 0x020F5CC4.
    let em = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/recompile.rs");
    let em = std::fs::read_to_string(&em).unwrap_or_else(|e| panic!("cannot read {}: {}", em.display(), e));
    assert!(
        em.contains("nnk_smc_note_branch(_t);"),
        "regression: the emitter drops an LDM/pop-with-pc target's state bit again"
    );
    assert!(
        em.contains("nnk_smc_note_branch(_t | 1u);") && em.contains("nnk_smc_note_branch(_t & ~1u);"),
        "regression: the single-register LDM form no longer forces the tag (Num==1 rule)"
    );
    // (b) the RUNTIME exposes the one-step carry the emitter calls, and BOTH
    //     live hooks resolve an unclassified PC the same way (the stale-band
    //     hook used to have no state resolution at all).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        src.contains("void nnk_smc_note_branch(uint32_t raw) { smc_branch_mode(raw); }"),
        "regression: the runtime lost the interworking-state hook the band TUs call"
    );
    let hdr = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/split_dispatch.py");
    let hdr = std::fs::read_to_string(&hdr).unwrap_or_else(|e| panic!("cannot read {}: {}", hdr.display(), e));
    assert!(
        hdr.contains("void nnk_smc_note_branch(uint32_t raw);"),
        "regression: nnk_rt.h no longer declares the state hook for the band TUs"
    );
    assert!(
        src.matches("smc_state_from_regs(pc)").count() >= 2,
        "regression: only one of the two live dispatch hooks resolves the target state"
    );
    // (c) a multi-register Thumb pop{..,pc} must NOT force Thumb: returning to
    //     an ARM caller (0x0207CAC4, the ARM BLX caller of 0x020F5C98) has to
    //     resume in ARM state.
    assert!(
        src.contains("uint32_t raw = (cnt == 1u) ? (tv | 1u) : tv;"),
        "regression: the Thumb POP arm forces bit0 for every register list again"
    );
    assert!(
        src.contains("uint32_t raw = (cnt == 1u) ? (lv & ~1u) : lv;"),
        "regression: the ARM LDM arm no longer masks bit0 for a one-register list"
    );
}

#[test]
fn compiled_bx_and_blx_rm_carry_the_target_state() {
    // Phase-5K, follow-up G: the Phase-5J class (13) fixed the LDM/`pop {..,pc}`
    // form only.  A compiled `bx Rm` / `blx Rm` still emitted
    //     pc = _t & ~1u;
    // with NO state note, so the live re-decoder had to guess the state of an
    // uncompiled target.  melonDS A_BX / A_BLX_REG both `JumpTo(Rm)`, and
    // JumpTo takes bit0 (ARMInterpreter_Branch.cpp) - the register value IS the
    // carrier, in both states.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/recompile.rs");
    let em = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let at = em.find("Kind::Bx { rm, link } =>").expect("Kind::Bx arm missing");
    let end = em.find("Kind::Clz").expect("Kind::Clz arm missing");
    let block = &em[at..end];
    assert!(
        block.matches("nnk_smc_note_branch(_t);").count() >= 2,
        "regression: compiled bx/blx Rm (or the bx lr return form) drops the target's state bit again"
    );
    assert!(
        block.contains("body.push(\"nnk_smc_note_branch(_t); }\".into());"),
        "regression: the register-indirect branch no longer notes the state it transfers to"
    );
}

#[test]
fn frozen_image_patch_decides_the_ldm_form_from_the_compiled_word() {
    // Phase-5K, barrier F: tools/p5j_note_branch.py spliced
    // `nnk_smc_note_branch(_t | 1u)` for EVERY one-register LDM-pc site, i.e.
    // the Thumb T_POP rule on ARM code.  melonDS A_LDM does the opposite
    // (`if (cpu->Num == 1) pc &= ~0x1;`), so the ARM epilogue at 0x02043C04
    // (`ldmfd sp!,{pc}`, word 0xE8BD8000) returned to ARM 0x0207D2D8 tagged
    // Thumb; the live re-decoder ran that ARM function as Thumb, walked the
    // string table at 0x0207E828.., the ASCII word 0x49552F61 at 0x0207E94C
    // decoded as an ARM `ldmia` with r13 in its list and sp became 0 (halt
    // 0x0207EA20, 42,038,854 steps).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/p5k_note_branch.py");
    let tool = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        tool.contains("(w & 0x0E000000) == 0x08000000") && tool.contains("(w & 0x00008000) != 0"),
        "regression: the LDM-form test no longer recognises an ARM LDM with pc in the list"
    );
    assert!(
        tool.contains("want = '& ~1u' if form == 'arm' else '| 1u'"),
        "regression: the single-register note is no longer chosen by the encoding's FORM"
    );
    // the Phase-5J patcher must not re-insert a contradicting Thumb tag on a
    // site the Phase-5K pass already corrected.
    let p5j = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/p5j_note_branch.py");
    let p5j = std::fs::read_to_string(&p5j).unwrap_or_else(|e| panic!("cannot read {}: {}", p5j.display(), e));
    assert!(
        p5j.contains("NOTE_ARM_SINGLE") && p5j.contains("(NOTE, NOTE_SINGLE, NOTE_ARM_SINGLE)"),
        "regression: the Phase-5J splice would duplicate the note (and re-tag an ARM return as Thumb)"
    );
    assert_eq!(0xE8BD8000u32 & 0x0E000000, 0x08000000, "fixture sanity: ARM ldmfd sp!,{{pc}}");
    assert_eq!(0xE8BD8000u32 & 0x00100000, 0x00100000, "fixture sanity: A_LDM load bit");
    assert_eq!(0xE8BD8000u32 & 0x00008000, 0x00008000, "fixture sanity: pc in the list");
}

#[test]
fn case_labels_carry_the_exact_state_marker() {
    // Phase-5K, follow-up H: nnk_pc_is_thumb() was built from the addr+2
    // fall-through heuristic, which is wrong for every case that ends in a
    // branch, so the live re-decoder had to guess.  The emitter now stamps the
    // state it decoded FROM on the case label and split_dispatch.py prefers
    // that marker, keeping the heuristic only for markerless (frozen) images.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/recompile.rs");
    let em = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    assert!(
        em.contains("case 0x{:08X}u: /* nnk_state={} */"),
        "regression: the emitted case label no longer records the instruction state"
    );
    assert!(
        em.contains("halts, thumb }"),
        "regression: the emit outcome no longer carries the state the body came from"
    );
    let p2 = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/split_dispatch.py");
    let sd = std::fs::read_to_string(&p2).unwrap_or_else(|e| panic!("cannot read {}: {}", p2.display(), e));
    assert!(
        sd.contains("nnk_state=(thumb|arm)"),
        "regression: split_dispatch.py no longer reads the per-case state marker"
    );
    assert!(
        sd.contains("if state is not None:") && sd.contains("needle = \"pc = 0x%08Xu;\" % (addr + 2)"),
        "regression: the marker is no longer authoritative over the addr+2 heuristic"
    );
}

// --------------------------------------------------------------- class 16 ---
// Thumb format 7 (`0101 L B 0 Ro Rb Rd`) — LDR/STR/LDRB/STRB with REGISTER
// offset.  Phase-5L, measured at 0x020F6560 inside the Mobiclip player: the
// live C re-decoder refused EVERY 0x5xxx word because it read B and L out of
// bit12/bit11 and then bailed with `if (hw & 0x1000u) return 0;` — bit12 is
// part of the 0x5 prefix and is set for the whole block, so a plain
// `ldrb r1,[r0,r1]` (0x5C41) could not be decoded and the run halted at step
// 2,255,768,127 with `dispatch to untranslated pc` (smcfail hw0=0x5C41
// hw1=0x1C02).  The Rust decoder has always been right (bit9 splits format 7
// from format 8, B = bit10); this test pins both halves.
#[test]
fn thumb_format7_register_offset_byte_load() {
    // 0x5C41 = `ldrb r1, [r0, r1]` (binutils: 0x5C41).
    let o = decode_thumb(0x5C41, 0, 0x020F_6560, 2);
    assert_eq!(o.text, "ldrb r1, [r0, r1]");
    match o.kind {
        Kind::Mem { load: true, byte: true, rd: 1, rn: 0, .. } => {}
        other => panic!("5C41 must be a byte load r1 <- [r0 + r1], got {:?}", other),
    }
    // 0x5801 = `ldr r1, [r0, r0]` — the WORD form of the same class (bit10 = 0).
    let w = decode_thumb(0x5801, 0, 0x020F_6560, 2);
    assert_eq!(w.text, "ldr r1, [r0, r0]");
    match w.kind {
        Kind::Mem { load: true, byte: false, rd: 1, rn: 0, .. } => {}
        other => panic!("5801 must be a word load r1 <- [r0 + r0], got {:?}", other),
    }
    // Format 8 (bit9 set) is a different class and must not be confused with it.
    assert_eq!(0x5C41 & 0x0200, 0, "fixture sanity: 5C41 is format 7");
    assert_eq!(0x5E41 & 0x0200, 0x0200, "fixture sanity: 5E41 is format 8");

    // Source-level lock for the C half (the include has no C harness, same
    // honest option as classes 13/16 above): format 7 must be discriminated on
    // bit9 and take B from bit10.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let arm = src
        .find("/* formats 7/8 */")
        .expect("regression: smc_exec_thumb lost its format 7/8 arm");
    let end = src[arm..]
        .find("format 9:")
        .map(|i| arm + i)
        .unwrap_or_else(|| (arm + 2400).min(src.len()));
    let block = &src[arm..end];
    assert!(
        !block.contains("(hw >> 12) & 1u"),
        "regression: the 0x5xxx arm is deriving B from bit12 again (always set)"
    );
    assert!(
        block.contains("if (hw & 0x0200u) {"),
        "regression: format 7 and format 8 are no longer split on bit 9"
    );
    assert!(
        block.contains("(hw >> 10) & 1u") && block.contains("r[(hw >> 3) & 7u] + r[ro]"),
        "regression: LDR/STR/LDRB/STRB register-offset no longer uses B=bit10 and R[Ro]"
    );
}

// --------------------------------------------------------------- class 17 ---
// Thumb format 8 (`0101 H S 1 Ro Rb Rd`) — STRH/LDRSB/LDRH/LDRSH with
// REGISTER offset.  Phase-5L deliberately left this arm as `return 0` so an
// unimplemented halfword form halted honestly; Phase-5M implements it so the
// Mobiclip player's register-offset halfword accesses execute from live RAM.
// The 4 (H,S) combinations are the whole class; bit 9 is the discriminator
// against format 7, bit 11 = H and bit 10 = S.
#[test]
fn thumb_format8_register_offset_halfword_and_signed() {
    for (hw, text, kind) in [
        (0x5241u16, "strh r1, [r0, r1]", HwKind::Strh),
        (0x5641, "ldrsb r1, [r0, r1]", HwKind::Ldrsb),
        (0x5A41, "ldrh r1, [r0, r1]", HwKind::Ldrh),
        (0x5E41, "ldrsh r1, [r0, r1]", HwKind::Ldrsh),
    ] {
        // fixture sanity: every one of these is format 8 (bits 15..12 = 0101,
        // bit 9 set) with H = bit11, S = bit10.
        assert_eq!(hw & 0xF000, 0x5000, "fixture sanity: {hw:04X} is 0101 H S 1");
        assert_eq!(hw & 0x0200, 0x0200, "fixture sanity: {hw:04X} is format 8");
        let o = decode_thumb(hw, 0, 0x0207_B218, 2);
        assert_eq!(o.text, text, "decode text for {hw:04X}");
        match o.kind {
            Kind::Hw { kind: got, rd: 1, rn: 0, .. } => assert_eq!(got, kind, "{hw:04X} kind"),
            other => panic!("{hw:04X} must be Kind::Hw {kind:?}, got {other:?}"),
        }
    }

    // Source-level lock for the C half: the 0x5xxx arm must no longer bail on
    // bit 9 and must take H from bit11 / S from bit10.
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let arm = src
        .find("/* formats 7/8 */")
        .expect("regression: smc_exec_thumb lost its format 7/8 arm");
    let end = src[arm..]
        .find("format 9:")
        .map(|i| arm + i)
        .unwrap_or_else(|| (arm + 3200).min(src.len()));
    let block = &src[arm..end];
    assert!(
        block.contains("unsigned H = (unsigned)((hw >> 11) & 1u);")
            && block.contains("unsigned S = (unsigned)((hw >> 10) & 1u);"),
        "regression: format 8 no longer derives H/S from bits 11/10"
    );
    assert!(
        !block.contains("if (hw & 0x0200u) return 0;"),
        "regression: format 8 is refused again instead of decoded"
    );
    assert!(
        block.contains("(uint32_t)(int32_t)(int16_t)ld16(ea)")
            && block.contains("(uint32_t)(int32_t)(int8_t)ld8(ea)"),
        "regression: the signed halfword/byte loads lost their sign extension"
    );
}

// --------------------------------------------------------------- class 18 ---
// The on-demand decode hook must cover the ITCM window as well as main RAM.
// Phase-5M, measured at step 2,256,160,152: the Mobiclip player's `blx r1` at
// 0x0207B218 (word 0xE12FFF31, melonDS disassembly `blx r1`) called
// 0x01FF8540 — an ITCM address.  The band dispatch routed it to the compiled
// nnk_part_itcm, that part has no case for it (its rooted ITCM range ends at
// 0x01FF8520's `pop {..,pc}`), so rc==0 and the run fell into
// nnk_smc_exec_any, which refused every PC outside 0x02000000.. only and
// halted with `untranslated pc=0x01ff8540`.  The ITCM array is live guest
// state (the guest loads it at boot; ld16/ld32/st16/st32 already bind it), so
// the same live decoder must be able to execute from it.
#[test]
fn runtime_live_decoder_covers_the_itcm_window() {
    // The two windows are disjoint and the measured halt PC is inside ITCM.
    const IMAGE_LOAD: u32 = 0x0200_0000;
    const ITCM_LO: u32 = 0x01FF_8000;
    const ITCM_HI: u32 = 0x0200_0000;
    const ITCM_PHYS: u32 = 0x8000;
    assert!(ITCM_LO < ITCM_HI && ITCM_HI <= IMAGE_LOAD);
    assert!((ITCM_LO..ITCM_HI).contains(&0x01FF_8540));
    assert_eq!(0x01FF_8540 - ITCM_LO, 0x540, "offset inside the 32 KiB array");
    assert!(0x540 + 4 <= ITCM_PHYS);

    // The Band-1 helper the dispatch uses must agree with these constants.
    let rp = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../nnktool/src/recompile.rs");
    let rs = std::fs::read_to_string(&rp).unwrap_or_else(|e| panic!("cannot read {}: {}", rp.display(), e));
    for needle in [
        "#define ITCM_LO 0x01FF8000u",
        "#define ITCM_HI 0x02000000u",
        "#define ITCM_PHYS 0x8000u",
    ] {
        assert!(rs.contains(needle), "regression: {needle} changed in the runtime prelude");
    }

    // Source-level lock for the C hook (no C harness, same honest option as
    // classes 13/16/17).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let f = src
        .find("int nnk_smc_exec_any(uint32_t *pcp)")
        .expect("regression: the on-demand hook disappeared");
    let end = src[f..]
        .find("smc_mode_done")
        .map(|i| f + i)
        .unwrap_or_else(|| (f + 2400).min(src.len()));
    let block = &src[f..end];
    assert!(
        block.contains("pc >= ITCM_LO && pc < ITCM_HI"),
        "regression: the on-demand hook is back to main RAM only (ITCM PCs halt)"
    );
    assert!(
        block.contains("itcm + (pc & (ITCM_PHYS - 1u))"),
        "regression: ITCM live decoding no longer reads the guest-owned itcm array"
    );
}

// --------------------------------------------------------------- class 18 ---
// `blx <imm>` must CARRY the state it switches to (Phase-5O, barrier I3).
//
// BLX immediate is the one interworking transfer whose target state is fully
// determined by the encoding: the Thumb halfword pair (suffix 11101) targets
// ARM, the ARM form (cond=1111, bits 27..25=101) targets Thumb (ARM ARM
// A7.1.4).  The compiled cases emitted `pc = target & ~1u;` with no
// `nnk_smc_note_branch`, so an UNROOTED callee was resolved by the live
// decoder's map/register/sticky guess.
//
// Measured: 0x020F6ED6 `blx 0x02054144` (0xF75D 0xE936) left smc_t_sticky =
// Thumb; the live decoder re-decoded the ARM `push {r4,lr}` at 0x02054144 as
// Thumb, ran 0x02054146..0x020541BE through the ARM bytes and branched into
// the middle of the movie-player table generator at 0x02054764 (its prologue
// skipped).  The 5th argument it reloads from [[sp,#0x128]] was 1, not a
// destination pointer, so `str r0,[r1,r7,lsl #2]` with r7 = 0x14 stored to
// guest address 0x51 == ITCM[0x51..0x54] and zeroed the IRQ dispatcher's
// literal pool 0x01FF8050/4 (the Phase-5N mechanism).
const T_BLX_I3_H1: u16 = 0xF75D; // blx 0x02054144 — measured at 0x020F6ED6
const T_BLX_I3_H2: u16 = 0xE936; // suffix 11101 = BLX (switches to ARM)
const T_BLX_I3_SITE: u32 = 0x020F_6ED6;
const T_BLX_I3_TARGET: u32 = 0x0205_4144;

#[test]
fn compiled_blx_immediate_carries_the_state_it_switches_to() {
    // (a) the encoding: Thumb BLX imm is Kind::BlxImm on the even target.
    let insn = decode_thumb(T_BLX_I3_H1, T_BLX_I3_H2, T_BLX_I3_SITE, 4);
    match insn.kind {
        Kind::BlxImm { target } => assert_eq!(
            target, T_BLX_I3_TARGET,
            "0xF75D 0xE936 at 0x020F6ED6 is `blx 0x02054144`"
        ),
        other => panic!("F75D E936 must decode as Kind::BlxImm, got {:?}", other),
    }
    // (b) NEGATIVE neighbour: flipping suffix bit12 gives BL, which stays in
    //     Thumb — a fix that switched state for every Thumb call would run the
    //     whole Thumb call graph in ARM state.
    let bl = decode_thumb(T_BLX_I3_H1, T_BLX_I3_H2 | 0x1000, T_BLX_I3_SITE, 4);
    assert!(
        !matches!(bl.kind, Kind::BlxImm { .. }),
        "0xF75D 0xF936 is `bl`, not `blx`: it must not be treated as interworking"
    );
    // (c) the EMITTER records the state the BLX switches to, derived from the
    //     state of the site (`thumb` param of emit_insn).
    let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src/recompile.rs");
    let em = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    let at = em.find("Kind::BlxImm { target } =>").expect("Kind::BlxImm arm missing");
    let end = at + em[at..].find("Kind::Mem {").expect("Kind::Mem arm missing");
    let block = &em[at..end];
    assert!(
        block.contains("let raw = if thumb { *target & !1u32 } else { *target | 1u32 };"),
        "regression: the BLX-immediate note no longer derives the callee state from the site state"
    );
    assert!(
        block.contains("nnk_smc_note_branch(0x{:08X}u);"),
        "regression: compiled `blx <imm>` drops the state bit again (unrooted callee re-decoded by guess)"
    );
    // (d) the FROZEN-image splice uses the same encoding test and never guesses.
    let t = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../tools/p5o_blx_note.py");
    let tool = std::fs::read_to_string(&t).unwrap_or_else(|e| panic!("cannot read {}: {}", t.display(), e));
    assert!(
        tool.contains("(w & 0xFE000000) == 0xFA000000") && tool.contains("(h2 & 0xF800) == 0xE800"),
        "regression: the BLX splice no longer decides the site state from the ARM/Thumb encodings"
    );
    assert!(
        tool.contains("if st == 'thumb' else (target | 1)"),
        "regression: the BLX splice no longer tags an ARM-form target as Thumb"
    );
    assert_eq!(T_BLX_I3_H2 & 0x1000, 0x0000, "fixture sanity: blx suffix bit12 = 0");
}

/// Class 19 (Phase-5U): the interrupt model the movie stall hangs on.
///
/// Measured (`PHASE5U.md` §3): after the movie start the guest parks in the
/// SDK scheduler (0x020067E0 / 0x02006E80 / 0x02008D20.., 1,097,539 hits per
/// hot PC over 64 M steps) with DISPSTAT bit 3 set, IE = 0x00040018 and
/// IF = 0x000C0019 — i.e. enabled+pending sources that CANNOT be delivered
/// because CPSR.I stays set (32/32 probe samples).  The two facts this class
/// locks are the ones a future fix must not silently lose:
///
///  * the runtime HAS a real VBlank IRQ producer (IF bit 0 on the VCOUNT==192
///    edge, gated on DISPSTAT bit 3) and rule-based delivery — both spliced by
///    `tools/split_dispatch.py`, not by the generated image, so a regenerated
///    image keeps them;
///  * the delivery gate is exactly `ime && (ie & iflags) && !I`, so the stall
///    is the guest's masked state and never a missing raise.
#[test]
fn runtime_interrupt_model_reaches_the_vblank_and_stops_at_cpsr_i() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let sd = std::fs::read_to_string(root.join("tools/split_dispatch.py"))
        .expect("cannot read tools/split_dispatch.py");
    // Phase-5F: the VBlank producer, on the DISPSTAT-bit-3-gated 192 edge.
    for needle in [
        "Phase-5F VBlank IRQ producer",
        "if (p5f_vc == 192u && (dispstat & 0x8u)) iflags |= 0x1u;",
    ] {
        assert!(sd.contains(needle), "regression: Phase-5F VBlank producer lost: {needle}");
    }
    // Phase-4N: rule-based delivery once the boot script is exhausted.
    assert!(
        sd.contains("\" && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])\""),
        "regression: rule-based IRQ delivery splice lost (only the 3 boot triggers would deliver)"
    );
    // The gate itself, in the generator prelude.
    let rs = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");
    assert!(
        rs.contains("&& ime && (ie & iflags) && !(cpsr_ctl & 0x80u))"),
        "regression: the IRQ delivery gate no longer honours CPSR.I/IME/IE&IF"
    );
    // The Phase-5U measurement harness: the probe hook and the two witness PCs.
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");
    for needle in [
        "static void p5u_hook(uint32_t pc)",
        "#define P5T_CB_ENTRY 0x01FF8540u",
        "#define P5T_CB_RET 0x0207B21Cu",
        "NNK_P5U_FROM",
    ] {
        assert!(inc.contains(needle), "regression: Phase-5U probe lost: {needle}");
    }
    let splice = std::fs::read_to_string(root.join("tools/p5u_probe_splice.py"))
        .expect("cannot read tools/p5u_probe_splice.py");
    assert!(
        splice.contains("p5u_hook(pc);") && splice.contains("static void trace_pc(uint32_t pc) {"),
        "regression: the probe no longer hooks trace_pc (the only call site both dispatch paths share)"
    );
    // The movie player's call site the callback cadence is counted at.
    assert_eq!(0x0207B218u32 & 3, 0, "fixture sanity: the `blx r1` is ARM-aligned");
}


/// Phase-5V — the CPSR.I stall is observable through ONE funnel.
///
/// Measured (PHASE5V.md): after the last delivered IRQ (step 2,256,303,812,
/// ctl D2->1F) the guest takes three balanced OS_DisableInterrupts /
/// OS_RestoreInterrupts pairs and then a LAST, unreleased disable at step
/// 2,256,356,359 (pc=0x02008D28, lr=0x02007070, ctl 1F->9F) inside the SDK
/// sleep/yield path — and from there to the end of the window not one further
/// CPSR write happens, so no interrupt can be delivered.
///
/// That count is only trustworthy because every architectural CPSR-control
/// write goes through `cpsr_set_ctl()`: the compiled `Kind::Msr` emitter, the
/// live re-decoder's `msr` cases, the exception entry (`0xD2`) and the
/// exception return (the SPSR restore).  This class locks that funnel plus the
/// probe that measures it, so a future fix cannot quietly lose either.
#[test]
fn cpsr_i_transitions_are_observable_through_the_single_ctl_funnel() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let rs = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");

    // The funnel: one definition per runtime text (RUNTIME, RUNTIME_ALL) and
    // one assignment site in each (a mode change is a CPSR write too, so both
    // banks and ctl are updated together).
    assert_eq!(
        rs.matches("static void cpsr_set_ctl(uint32_t ctl)").count(),
        2,
        "regression: cpsr_set_ctl definitions changed — the I-bit history would split"
    );
    assert_eq!(
        rs.matches("cpsr_ctl = ctl & 0xFFu;").count(),
        2,
        "regression: cpsr_ctl is no longer written only by cpsr_set_ctl (RUNTIME + RUNTIME_ALL)"
    );
    // Both exception paths must stay inside the funnel.
    for needle in [
        "cpsr_set_ctl(0xD2u);",              // exception entry: IRQ mode, I+F
        "cpsr_set_ctl(sr & 0xFFu);",         // exception return: SPSR restore
        "static void irq_enter(uint32_t *pc)",
        "static void bios_irq_return(uint32_t *pc)",
    ] {
        assert!(rs.contains(needle), "regression: exception CPSR path lost: {needle}");
    }
    // The compiled `msr` and the live-decoder `msr` must both write ctl only.
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");
    assert_eq!(
        inc.matches("cpsr_set_ctl(tv & 0xFFu);").count(),
        2,
        "regression: the live decoder's msr CPSR_c / msr spsr_c paths changed"
    );

    // The probe (tools/p5v_i_probe_splice.py) and its three measured sites.
    let splice = std::fs::read_to_string(root.join("tools/p5v_i_probe_splice.py"))
        .expect("cannot read tools/p5v_i_probe_splice.py");
    for needle in [
        "static void p5v_i_edge(uint32_t oldc, uint32_t newc)",
        "(((p5v_o ^ cpsr_ctl) & 0x80u) != 0u)",   // log I transitions only
        "NNK_P5V_FROM",
        "static void p5v_sched(uint32_t pc)",
        "0x020070B4u",                            // the yield loop that parks
        "0x020067E0u",                            // OS_RescheduleThread
        "0x0206C684u",                            // OSi_ThreadInfo block
        "p5v_watch(a, v, 4u)",
        "NNK_P5V_WATCH_FROM",
    ] {
        assert!(splice.contains(needle), "regression: Phase-5V probe lost: {needle}");
    }
    assert!(
        splice.contains("'static void trace_pc(uint32_t pc) {'")
            && splice.contains("p5v_sched(pc);"),
        "regression: the scheduler dump no longer hooks trace_pc (the shared dispatch point)"
    );
    // Fixture sanity: the two helpers the measured history names.
    assert_eq!(0x02008D28u32 & 3, 0, "fixture: OS_DisableInterrupts' msr is ARM-aligned");
    assert_eq!(0x02008D40u32 & 3, 0, "fixture: OS_RestoreInterrupts' msr is ARM-aligned");
}

#[test]
fn thumb_pic_base_register_is_not_an_interworking_target() {
    // Phase-5X, class 35: the post-skip halt at 0x020DDF08.
    //
    // Measured (operator skip path reproduced headlessly with periodic key
    // presses, halt step 331,706,950): the live re-decoder executed
    //     0x020DDE9A  hw 0x447B   add r3, pc        (Thumb format 5 case 0)
    // which writes Align(pc+4,4) = Align(0x020DDE9E,4) = 0x020DDE9C into r3.
    // The very next dispatch was 0x020DDE9C, i.e. r3 == pc.  The unclassified
    // PC fell through to smc_state_from_regs(), which reads "a register whose
    // value & ~1 equals pc" as a carried interworking target: even value ->
    // ARM state.  The ARM reading of the Thumb jump-table prologue
    // (0x88DB 0x041B 0x141B 0x449F 0x0006 ...) stepped 4 bytes at a time down
    // the halfword table until 0x020DDF08 — Thumb-2 `f753 e838` — decoded as
    // an ARM load whose target was pc=0, i.e. `untranslated pc=0x00000000`
    // with r13=r14=0 (a DESTROYED register file: the symptom, not the cause).
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let p = root.join("tools/nnk_smc.inc");
    let src = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));

    // (a) the tag exists, is one-step scoped, and is consulted by the state scan.
    assert!(
        src.contains("static int smc_is_pcbase(int k) {") &&
        src.contains("return smc_pcbase_reg == k && steps == smc_pcbase_step + 1ull;"),
        "regression: the PIC-base tag lost its one-dispatch lifetime"
    );
    assert!(
        src.contains("if (smc_is_pcbase(k)) continue;"),
        "regression: smc_state_from_regs reads an `add rd, pc` PIC base as an ARM target again"
    );
    // (b) both PC-relative register forms produce the tag, and only for rd != pc
    //     (a PC write is a branch, handled by the carry path instead).
    assert_eq!(
        src.matches("if (rs == 15u) smc_note_pcbase((int)rd);").count(), 2,
        "regression: `add rd, pc` / `mov rd, pc` no longer tag the register they fill"
    );
    // (c) the Thumb register-indirect branch carries its state (BX -> bit0,
    //     BLX Rm -> always ARM; ARM ARM A7.1.4).  It used to strip bit0 and
    //     note nothing, leaving the target on the stale sticky value.
    assert!(
        src.contains("smc_branch_mode((hw & 0x0080u) ? (*npc) : tv);"),
        "regression: the Thumb BX/BLX rM arm drops the interworking state again"
    );

    // (d) the geometry that made this possible: the Thumb `add rd, pc` base is
    //     the ALIGNED pc+4, so it coincides with the following instruction
    //     address whenever (pc+4) is not already word aligned.
    let pc: u32 = 0x020DDE9A;
    let base = (pc + 4) & !3u32;
    assert_eq!(base, 0x020DDE9Cu32, "fixture: add r3,pc base");
    assert_eq!(pc + 2, 0x020DDE9Cu32, "fixture: the coincident next instruction");
}

#[test]
fn input_file_is_frame_timed_and_masks_the_key_space() {
    // Phase-5X, operator request: `NNK_INPUT_FILE` — the DQ8 `DQ8_INPUT_FILE`
    // analogue (DQ8 src/native_hid.c:native_hid_pump re-reads the file every
    // HID pump and publishes the mask).  Step-timed NNK_INPUT_INJECT cannot
    // express "press when the prompt appears", which is why the operator's
    // frame-timed taps did not reproduce from a fixed-step injection.
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let p = root.join("tools/nnk_input.inc");
    let src = std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {}", p.display(), e));
    for needle in [
        "if (!p || !p[0]) return;",
        "if (gxd_frames == nnk_file_frame) return;",   // once per DISPLAY FRAME
        "v &= 0xFFFu;",                             // 10 keys + DS X/Y
        "if (v == nnk_mask) return;",                  // edge-driven, no churn
        "gxd_set_keymask(gxd, nnk_mask);",             // same path a key event takes
        "nnk_input_file_tick();",                      // ticked from nnk_input_tick
        "script applied frame=%u mask=0x%03X entry=%d/%d ", // visible under NNK_INPUT_TRACE
    ] {
        assert!(src.contains(needle), "regression: NNK_INPUT_FILE lost: {needle}");
    }
    // A missing file must HOLD the last mask (the host may write it only on
    // transitions): the only early returns before the read are the unset-env
    // and same-frame guards, and a failed fopen returns without touching state.
    let tick = &src[src.find("static void nnk_input_file_tick(void)").expect("tick missing")..];
    let body = &tick[..tick.find("\n}\n").expect("tick end missing")];
    assert_eq!(body.matches("return;").count(), 2, "fixture: file tick early-outs changed");
    assert!(!body.contains("nnk_mask = 0xFFFu"), "regression: a missing file clears the keys");
    // KEYINPUT is active-low: idle 0x3FF, B = bit1, A = bit0, Start = bit3.
    assert_eq!(0x3FFu32 & !(1u32 << 1), 0x3FDu32, "fixture: B mask");
    assert_eq!(0x3FFu32 & !(1u32 << 0), 0x3FEu32, "fixture: A mask");
    assert_eq!(0x3FFu32 & !(1u32 << 3), 0x3F7u32, "fixture: Start mask");
}

/// Class 21 (Phase-5W): the HLE movie plane may only land inside the guest's
/// movie arena.  The measured failure: at the movie start the codec/player
/// structs hand the HLE plane copy destinations that are not the ring slots
/// yet ([player+0x3C] read 0x20, and a stale source-plane field dropped a full
/// 256x192 plane on the SDK's OSi_ThreadInfo block 0x0206C684).  The block's
/// only write between 2.25e9 and 2.257e9 steps that did not come through
/// st8/st16/st32 is that memcpy, and the block went zero on the instruction
/// right after it ran (step 2,256,256,945, pc 0x0207B21C, prev 0x01FF9E00).
/// So `p5t_plane` must refuse anything outside 0x02100000..0x02400000 (the
/// range every measured movie buffer lives in: melonDS ring slot 0x022C0880,
/// native cursors 0x022E48E8 / 0x02355110, codecs 0x0227834C / 0x022E8B74,
/// player 0x02278264) and it must say so instead of silently swallowing it.
#[test]
fn movie_plane_writes_stay_inside_the_movie_arena() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");

    assert!(
        inc.contains("#define P5T_ARENA_LO 0x02100000u")
            && inc.contains("#define P5T_ARENA_HI 0x02400000u"),
        "regression: the movie arena bounds changed — the plane guard is what \
         keeps the SDK thread block intact"
    );

    // The guard must be the LAST check before the copy, and the copy must be
    // the only write path (one memcpy in the function).
    let body_start = inc
        .find("static int p5t_plane(uint32_t dst, const uint8_t *src, uint32_t n) {")
        .expect("p5t_plane signature missing");
    let body = &inc[body_start..];
    let end = body.find("\n}\n").expect("p5t_plane body end missing");
    let body = &body[..end];
    let guard = body
        .find("if (dst < P5T_ARENA_LO || dst + n > P5T_ARENA_HI) {")
        .expect("regression: p5t_plane lost its arena guard");
    let copy = body
        .find("memcpy(gmem + o, src, n);")
        .expect("regression: p5t_plane lost its copy");
    assert!(guard < copy, "regression: the copy runs before the arena guard");
    assert_eq!(
        body.matches("memcpy(gmem + o, src, n);").count(),
        1,
        "regression: p5t_plane grew a second, unguarded copy path"
    );
    assert_eq!(
        body.matches("return 0;").count(),
        3,
        "regression: p5t_plane's refusals (not RAM / past RAM / outside arena) changed"
    );
    assert!(
        inc.contains("movie: plane REFUSED dst=0x%08X"),
        "regression: a refused plane destination must be logged, not swallowed"
    );

    // The probe that measured it: a per-instruction state diff (cannot be
    // bypassed by any store path) plus the region store watch, on the two
    // addresses the measurement names.
    let splice = std::fs::read_to_string(root.join("tools/p5w_store_probe_splice.py"))
        .expect("cannot read tools/p5w_store_probe_splice.py");
    for needle in [
        "static void p5w_diff(uint32_t pc)",
        "static void p5w_store(uint32_t a, uint32_t v, unsigned n)",
        "static void p5w_snap(uint32_t *h)",
        "#define P5W_LO 0x0206C680u",
        "#define P5W_HI 0x0206C6B0u",
        "0x0206C684u",
        "2250000000ull",
        "NNK_P5W_FROM",
    ] {
        assert!(splice.contains(needle), "regression: Phase-5W probe lost: {needle}");
    }
    // trace_pc is the shared dispatch point the diff hooks (one call per
    // instruction: the diff attributes a change to the previous instruction).
    assert!(
        splice.contains("p5u_hook(pc);\\n    p5w_diff(pc);")
            && splice.contains("p5w_park(pc);"),
        "regression: the Phase-5W diff no longer hooks trace_pc"
    );

    // Fixture sanity: the payload the guard must exclude is the SDK block.
    assert_eq!(0x0206C684u32 >= 0x02000000 && 0x0206C684u32 < 0x02100000, true,
               "fixture: OSi_ThreadInfo sits below the movie arena");
    assert_eq!(0x022C0880u32 >= 0x02100000 && 0x022C0880u32 < 0x02400000, true,
               "fixture: the measured melonDS ring slot sits inside the arena");
}

// --------------------------------------------------------------- class 21 ---
// VRAMCNT_H / VRAMCNT_I live at 0x04000248 / 0x04000249, and 0x04000247 is
// WRAMCNT — not a VRAM bank (Phase-5Y, the first-screen render).
//
// Measured (`PHASE5Y.md` §2): the guest programs the banks with byte stores
//   W8 0x04000247 = 0x03   (WRAMCNT: 0x3000000 switched to ARM7)
//   W8 0x04000248 = 0x82   (VRAMCNT_H = enabled, mst 2)
//   W8 0x04000249 = 0x83   (VRAMCNT_I = enabled, mst 3 = engine-B BG ext pal)
// and melonDS's own state dump at the notice frame reads back
// H=0x82, I=0x83.  Both our runtime model (`tools/nnk_vramcnt.inc`, carried by
// the frozen dispatch image, and the same code emitted by `recompile.rs`) and
// `gxrender`'s VRAMCNT decode used a flat `addr - 0x04000240` bank table, which
// put H at 0x247 and I at 0x248: the WRAMCNT store became VRAMCNT_H, the real H
// store landed on I and the real I store was dropped.  Bank I then stopped
// serving "engine-B BG extended palette", DISPCNT_B bit30 (BG ext pal enable)
// made BG1 read that empty palette, and the notice panel + pirate figure +
// "We do not condone piracy" caption rendered black — only the OBJ "Tap to"
// prompt (B-OBJ palette, 4 colours) survived.  After the fix the same frame
// matches melonDS on BG1's flat VRAM, the BG palette *and* the BG ext palette.
const VRCY_MEASURED: [(u32, u32); 3] = [
    (0x0400_0247, 0x03), // WRAMCNT
    (0x0400_0248, 0x82), // VRAMCNT_H
    (0x0400_0249, 0x83), // VRAMCNT_I
];

/// The slot table both the C models and the melonDS reference use.
fn vrcy_slot(a: u32) -> Option<usize> {
    match a {
        0x0400_0240..=0x0400_0246 => Some((a - 0x0400_0240) as usize),
        0x0400_0248 => Some(7),
        0x0400_0249 => Some(8),
        _ => None, // 0x04000247 WRAMCNT, 0x0400024A/B unused
    }
}

#[test]
fn vramcnt_h_and_i_are_at_0x248_and_0x249_with_wramcnt_at_0x247() {
    // (a) the measured guest sequence reproduces melonDS's bank file.
    let mut banks = [0u8; 9];
    let mut wramcnt = 0u8;
    for (a, v) in VRCY_MEASURED {
        match vrcy_slot(a) {
            Some(b) => banks[b] = v as u8,
            None => wramcnt = v as u8,
        }
    }
    assert_eq!(banks[7], 0x82, "VRAMCNT_H must hold the 0x04000248 store");
    assert_eq!(banks[8], 0x83, "VRAMCNT_I must hold the 0x04000249 store");
    assert_eq!(wramcnt, 0x03, "0x04000247 is WRAMCNT, not a VRAM bank");
    assert_eq!(
        banks[8] & 7,
        3,
        "VRAMCNT_I = 0x83 selects mst 3 = engine-B BG extended palette"
    );
    // (b) NEGATIVE neighbour: 16-bit programming pairs (melonDS ARM9Write16).
    assert_eq!(vrcy_slot(0x0400_0246), Some(6), "0x246 is VRAMCNT_G");
    assert_eq!(vrcy_slot(0x0400_0247), None, "0x247 must never be a VRAM bank");

    // (c) all three implementations carry the same table.
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let inc = std::fs::read_to_string(root.join("tools/nnk_vramcnt.inc"))
        .expect("cannot read tools/nnk_vramcnt.inc");
    let re = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");
    let gxd = std::fs::read_to_string(root.join("gxrender/src/gx_display.c"))
        .expect("cannot read gxrender/src/gx_display.c");
    for (name, src) in [("nnk_vramcnt.inc", &inc), ("recompile.rs", &re)] {
        assert!(
            src.contains("case 0x04000248: return 7;"),
            "regression: {name} no longer maps 0x04000248 to VRAMCNT_H"
        );
        assert!(
            src.contains("case 0x04000249: return 8;"),
            "regression: {name} no longer maps 0x04000249 to VRAMCNT_I"
        );
        assert!(
            !src.contains("int bank = (int)(a - 0x04000240u)"),
            "regression: {name} is back on the flat 0x240+bank table"
        );
    }
    assert!(
        gxd.contains("case 0x04000248: case 0x04000249: {")
            && gxd.contains("static const int slot[10] = { 0, 1, 2, 3, 4, 5, 6, -1, 7, 8 };"),
        "regression: gx_display's VRAMCNT decode no longer routes 0x248/0x249"
    );
}

/// Class 38 (Phase-5AA): the intro drives TWO concurrent movies, one per
/// screen, so decode-step tracking must be PER CODEC.
///
/// Measured (melonDS REGPROBE at 0x0207B218, 40 consecutive hits, ts
/// 2,272,165,619..2,307,573,062): r1 is always the ITCM callback
/// 0x01FF8540, r5 alternates 0x02278264 / 0x022E8A8C, r0 alternates
/// 0x0227834C / 0x022E8B74 — two player objects, two codec structs, each with
/// its own MODS header at player+4 and its own six-slot Y ring.  melonDS's
/// main RAM at ts 2.6179e9 matches player 0x02278264's ring slot 0x022C0880
/// to op_u frame 130 and player 0x022E8A8C's ring slot 0x023310A8 to op_d
/// frame 130 (best-frame correlation 0.708 / MAE 16.9).  The old HLE kept ONE
/// global index and reset it whenever the codec changed, so every present of
/// the alternating session showed frame 0 (Phase-5W measured four byte-
/// identical display frames).  This class pins the per-codec stream table,
/// the per-stream cursor reset rule, the second source file, and the measured
/// owner relation player = codec - 0xE8 (with [player+0x34] == codec).
#[test]
fn movie_decode_progress_is_tracked_per_codec() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");

    // One entry per codec; the struct carries the per-stream cursor and index.
    assert!(inc.contains("#define P5T_MAXSTREAM 4"), "the stream table disappeared");
    let st = inc
        .find("typedef struct {\n    uint32_t codec;")
        .expect("regression: the per-codec stream struct is gone");
    let st_body = &inc[st..st + 400.min(inc.len() - st)];
    for needle in ["uint32_t cursor;", "long index;", "long frames, presents, shortr;", "FILE *fp;"] {
        assert!(st_body.contains(needle), "regression: stream field lost: {needle}");
    }

    // Registration is by codec address, and a new session starts at frame 0.
    let reg = inc
        .find("static P5tStream *p5t_stream(uint32_t codec) {")
        .expect("regression: p5t_stream is gone");
    let reg_body = &inc[reg..];
    let reg_body = &reg_body[..reg_body.find("\n}\n").expect("p5t_stream end missing")];
    assert!(
        reg_body.contains("if (p5t_str[i].codec == codec) return &p5t_str[i];"),
        "regression: codecs are no longer looked up by address (the session restarts)"
    );
    assert!(reg_body.contains("s->index = -1;"), "regression: a new stream no longer starts at frame 0");
    assert!(
        reg_body.contains("getenv(\"NNK_MOVIE_YUV2\")"),
        "regression: the second screen's source file (op_d) lost its env"
    );

    // Present uses THIS decode step's codec: its own stream, cursor and index.
    let pr = inc
        .find("static void p5t_present(void) {")
        .expect("regression: p5t_present is gone");
    let pr_body = &inc[pr..];
    let pr_body = &pr_body[..pr_body.find("\n}\n").expect("p5t_present end missing")];
    assert!(
        pr_body.contains("if (p5t_str[i].codec == p5t_codec) s = &p5t_str[i];"),
        "regression: present no longer selects the current codec's stream"
    );
    assert!(
        pr_body.contains("if (s->index < 0) s->index = 0;")
            && pr_body.contains("s->cursor = cur;")
            && pr_body.contains("s->presents++"),
        "regression: the frame index is no longer a plain per-stream decode-step count"
    );
    // The measured owner relation (player = codec - 0xE8) is used for the
    // report; the plane WRITE must stay on the codec's six ring slots — the
    // guest's present function fills player+0x3C/+0x40 itself from the arrays
    // at player+0x5C/+0x60, which hold exactly those slots.  Writing the
    // player's fields from the HLE is the Phase-5W stale-destination hazard.
    assert!(
        pr_body.contains("player = p5t_codec - 0xE8u;"),
        "regression: the measured owner relation left the present path"
    );
    assert!(
        !pr_body.contains("player + 0x3Cu") && !pr_body.contains("player + 0x40u"),
        "regression: the HLE writes the player's source-plane fields again (Phase-5W hazard)"
    );
    assert!(
        pr_body.contains("ld32(p5t_codec + 0x0Cu + 4u * i)"),
        "regression: the six codec Y ring slots are no longer the write target"
    );
    // The single global index must be gone entirely.
    assert!(
        !inc.contains("p5t_index") && !inc.contains("p5t_frames"),
        "regression: the removed single-stream globals came back"
    );
    // The hook still fires only at the measured callback return.
    assert!(
        inc.contains("if (npc == P5T_CB_RET && pc >= ITCM_LO && pc < ITCM_HI) p5t_present();"),
        "regression: the movie present hook left the callback return site"
    );

    // Fixture sanity from the two measured codec/player pairs.
    assert_eq!(0x0227_834Cu32 - 0xE8, 0x0227_8264u32, "fixture: player A = codec A - 0xE8");
    assert_eq!(0x022E_8B74u32 - 0xE8, 0x022E_8A8Cu32, "fixture: player B = codec B - 0xE8");
    assert_ne!(0x0227_834Cu32, 0x022E_8B74u32, "fixture: the two streams have distinct codecs");
}

/// Phase-5AB, class 39: the VRAM read shadow must make a guest store readable
/// back through the 0x06000000 window, and the BUILD must not silently drop it.
///
/// Measured (operator log, and reproduced headlessly to the exact register
/// file): the halt at 0x020E1B32 is Thumb
///     0x020E1B26  adds r0, r5, #4     ; r0 = object+4 = 0x06006004
///     0x020E1B28  ldr  r4, [r0, #0]   ; the C++ vptr stored there
///     0x020E1B2C  ldr  r4, [r4, #0x24]; the virtual method
///     0x020E1B32  blx  r4             ; -> 0xE3A03102, "untranslated pc"
/// and r5 = 0x06006000, i.e. a guest object kept in the VRAM window.  With the
/// read shadow in the runtime the round trip is exact and the branch executes
/// (Phase-5AB probe, `NNK_P5AB`, steps 333,957,453..333,957,727):
///     w 0x020429C8  0x06006004 <- 0x0206927C
///     w 0x020FB130  0x06006004 <- 0x0213D074
///     w 0x020F9004  0x06006004 <- 0x0213CF2C
///     r 0x020E1B28  0x06006004 -> 0x0213CF2C   (blx r4 taken)
/// The same guest parts built WITHOUT the shadow (scratch/p5aa/split2, the
/// 13b3c45 binary's runtime TU) read 0 back and halt at 0x020E1B32, so the
/// defect is in the build path as much as in the prelude: the frozen image
/// `work/phase3a/arm9_thumb_ovl_itcm_p5u.c` predates the shadow and
/// `tools/build_split.sh` used to build it verbatim.
#[test]
fn vram_read_shadow_round_trips_a_guest_store() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let rs = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");

    // (a) the canonical prelude carries the shadow: one offset function, a
    //     lazily allocated byte image, and a read helper that refuses an access
    //     straddling a window edge (it must not silently wrap into the next).
    for needle in [
        "static uint32_t gxd_sh_off(uint32_t a) {",
        "static uint8_t *gxd_sh_wr(uint32_t a) {",
        "static const uint8_t *gxd_sh_rd(uint32_t a, uint32_t n) {",
        "if (!gxd_shadow) gxd_shadow = (uint8_t *)calloc(1, GXD_SH_SIZE);",
        "if (gxd_sh_off(a + n - 1u) != o + (n - 1u)) return NULL;",
    ] {
        assert!(rs.contains(needle), "regression: the VRAM read shadow lost: {needle}");
    }
    // Every load consults it, with its own width.
    for n in [4u32, 2, 1] {
        let needle = format!("{{ const uint8_t *q = gxd_sh_rd(a, {}u);", n);
        assert!(rs.contains(&needle), "regression: a load no longer reads the shadow: {needle}");
    }
    // Every store keeps the shadow current before the GX forward (the
    // rendering path stays untouched: gxd_io_write still runs afterwards).
    assert_eq!(
        rs.matches("uint8_t *q = gxd_sh_wr(a);").count(), 3,
        "regression: st8/st16/st32 no longer all update the read shadow"
    );

    // (b) the offset math, re-implemented from the C text: the three windows
    //     are consecutive slabs of one buffer, and only they are mapped.
    const VRAM: u32 = 0x8A0000;
    let off = |a: u32| -> Option<u32> {
        if (0x0500_0000..0x0500_0000 + 0x800).contains(&a) {
            return Some(a - 0x0500_0000);
        }
        if (0x0600_0000..0x0600_0000 + VRAM).contains(&a) {
            return Some(0x800 + (a - 0x0600_0000));
        }
        if (0x0700_0000..0x0700_0000 + 0x800).contains(&a) {
            return Some(0x800 + VRAM + (a - 0x0700_0000));
        }
        None
    };
    // The measured round trip: the object word at 0x06006004 keeps its own
    // slot, and no other window address collides with it.
    let obj = off(0x0600_6004).expect("0x06006004 must be shadowed");
    assert_eq!(obj, 0x800 + 0x6004, "fixture: the vptr word's shadow slot");
    assert_eq!(off(0x0600_6000).unwrap(), 0x800 + 0x6000);
    assert_eq!(off(0x0600_6008).unwrap(), 0x800 + 0x6008);
    assert_eq!(off(0x0500_0000).unwrap(), 0);
    assert_eq!(off(0x0700_0000).unwrap(), 0x800 + VRAM);
    assert_eq!(off(0x0600_0000 + VRAM - 1).unwrap(), 0x800 + VRAM - 1);
    assert_eq!(off(0x0600_0000 + VRAM), None, "past VRAM: not shadowed");
    assert_eq!(off(0x0500_0800).unwrap_or(0xDEAD), 0xDEAD, "palette window is 2 KiB");
    assert_eq!(off(0x027E_3A30).unwrap_or(0xDEAD), 0xDEAD, "main RAM is not shadowed");
    // A 4-byte access at the last shadowed byte must be refused, not wrapped.
    let last = off(0x0600_0000 + VRAM - 1).unwrap();
    assert_eq!(off(0x0600_0000 + VRAM - 1 + 3).is_none(), true);
    assert_ne!(last + 3, off(0x0700_0000).unwrap(), "window edges must not alias");
    // Distinct windows never share a slot (a store to OAM cannot fabricate a
    // vptr in the VRAM slab).
    assert_ne!(off(0x0700_0000).unwrap(), obj);
    assert_ne!(off(0x0500_0000).unwrap(), obj);

    // (c) the delivery fix: build_split.sh applies the canonical prelude
    //     transform to a frozen image that lacks it (the 13b3c45 binary was
    //     built from such an image, which is why the operator still halted).
    let bs = std::fs::read_to_string(root.join("tools/build_split.sh"))
        .expect("cannot read tools/build_split.sh");
    assert!(
        bs.contains("! grep -q 'gxd_sh_rd' \"$IMAGE\"")
            && bs.contains("tools/p5x_runtime_splice.py \"$IMAGE\" \"$SPLICED_IMAGE\""),
        "regression: a frozen dispatch image can again be built without the read shadow"
    );
    assert!(
        bs.find("gxd_sh_rd")
            .unwrap()
            < bs.find("if [[ -n \"${NNK_MONO:-}\" ]]").unwrap_or(usize::MAX),
        "regression: the prelude splice must run before the mono/split build paths"
    );

    // Fixture sanity from the measured halt.
    assert_eq!(0x020E_1B32u32 - 0x020E_1B28u32, 10, "fixture: ldr..blx span is 10 bytes of Thumb");
    assert_eq!(0x0600_6004u32 & 3, 0, "fixture: the vptr word is 4-byte aligned");
}

/// Phase-5AC, class 40: `NNK_MOVIE_YUV` NAMES a frame source — it is not a
/// boolean, and a bare truthy value must not leave the HLE "enabled" with
/// nothing to present.
///
/// Measured in Phase-5AC on the same shadow-fixed binary and the same scripted
/// session: `NNK_MOVIE_YUV=1` (cfg1) and `NNK_MOVIE_YUV` unset (cfg0) produce
/// **14/14 byte-identical** display frames (f100..f1400), because p5t_init()
/// used to set `p5t_state = 2` for any non-empty value while p5t_stream() then
/// failed to `fopen("1")` for the first codec, so `p5t_nstr` stayed 0 and
/// p5t_present() returned without writing a byte — the whole hook was off
/// while the banner still printed `movie: hle src1=1`.  The startup gate is
/// now the source file itself: only a path that opens enables the present
/// path, and the off case says so and names the canonical pair.
#[test]
fn movie_hle_requires_a_readable_source() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");

    // (a) the gate: the source is probed with fopen BEFORE the HLE is enabled,
    //     and the enable line is not reachable on a failed probe.
    let init = inc
        .find("static void p5t_init(void) {")
        .expect("regression: p5t_init is gone");
    let init_body = &inc[init..];
    let init_body = &init_body[..init_body.find("\n}\n").expect("p5t_init end missing")];
    assert!(
        init_body.contains("if (!p || !p[0]) return;"),
        "regression: an unset/empty NNK_MOVIE_YUV no longer leaves the HLE off"
    );
    assert!(
        init_body.contains("probe = fopen(p, \"rb\");"),
        "regression: NNK_MOVIE_YUV is trusted without opening it (a bare \"1\" would enable the HLE)"
    );
    let gate = init_body.find("if (!probe) {").expect("regression: the fopen gate is gone");
    let enable = init_body.find("p5t_state = 2;").expect("regression: the enable line is gone");
    assert!(
        gate < enable,
        "regression: p5t_state = 2 is reached without a successful fopen (the Phase-5AC mis-wiring)"
    );
    assert!(
        init_body[gate..enable].contains("HLE OFF"),
        "regression: a rejected source no longer announces that the HLE is off"
    );
    assert!(
        init_body.contains("NNK_MOVIE_YUV2=<op_d.yuv>"),
        "regression: the canonical configuration (op_u top / op_d bottom) left the message"
    );
    // The per-frame size that makes a path a *source* rather than a flag.
    assert!(inc.contains("#define P5T_FRAME (P5T_YSZ + 2 * P5T_CSZ)"));
    assert_eq!(256u32 * 192 * 3 / 2, 73728, "fixture: one 256x192 yuv420p frame");

    // (b) the gate re-implemented from the C text: only a source that opens
    //     turns the hook on; every truthy non-file stays off (state 1).
    let decide = |v: Option<&str>, opens: &dyn Fn(&str) -> bool| -> u32 {
        match v {
            None => 1,
            Some(s) if s.is_empty() => 1,
            Some(s) if opens(s) => 2,
            Some(_) => 1,
        }
    };
    let only_op_u = |s: &str| s == "op_u.yuv";
    for v in [None, Some(""), Some("0"), Some("1"), Some("on"), Some("yes"), Some("/nonexistent/op_u.yuv")] {
        assert_eq!(decide(v, &only_op_u), 1, "{v:?} must leave the HLE off");
    }
    assert_eq!(decide(Some("op_u.yuv"), &only_op_u), 2, "a real source must enable the hook");
    // The measured equivalence that motivates the gate: cfg1 == cfg0 byte-exact.
    assert_eq!(decide(Some("1"), &only_op_u), decide(None, &only_op_u),
        "fixture: NNK_MOVIE_YUV=1 and unset both leave the HLE off (Phase-5AC, 14/14 identical frames)");

    // (c) a failed source must not be retried per codec forever: the probe is
    //     in p5t_init (once), and p5t_stream's own failure path still logs.
    assert_eq!(
        inc.matches("probe = fopen(p, \"rb\");").count(), 1,
        "regression: the source probe duplicated (it must run once, in p5t_init)"
    );
    assert!(
        inc.contains("movie: stream %d cannot open %s"),
        "regression: the per-stream open failure stopped being reported"
    );
}

/// Phase-5AD, class 41: the movie session ends on a **skip key edge**, not on a
/// starved ring — and the measured band defects of Phase-5AC came from the
/// input script, not from the renderer or the HLE.
///
/// Measured (Phase-5AD, same shadow-fixed binary, same canonical pair):
///   * with `tools/nnk_operator_session.nnk` the decode driver 0x0207B1D0 is
///     first called at step 323,391,401 (display frame 578) and presents
///     exactly 7 frames per stream; the last present is at step 326,421,022,
///     and the player's frame loop 0x0207D124..0x0207D2E8 then leaves at step
///     327,550,216 through `bne 0x0207D2EC` with `r1 = 8` (Start) —
///     `NNK_DEBUG_AT_PC=0x0207D248`;
///   * at that moment `player+0x0C` = 0x525 = 1317 (MODS frame total) while
///     `player+0x98` = 7 (frames decoded) and `player+0x9C` = 1 <= `+0xA0` = 7,
///     so neither the "finished" return at 0x0207AFC4 (`[0x98] == [0x0C]`) nor
///     the ring-full early return at 0x0207B1DC (`[0x9C] > [0xA0]`) can be the
///     exit;
///   * the only other exit of the loop 0x0207D124..0x0207D2E8 is the A/Start
///     edge (`and r1, r0, #9` at 0x0207D230, `bne 0x0207D2EC` at 0x0207D248) —
///     and the operator script presses Start every 36 display frames from
///     frame 13 on;
///   * the control run with the schedule cut after frame 555 presents the
///     identical 14 lines (n=1..7, same steps to the step) and then continues:
///     n=8 at step 327,673,343 — removing one later press extends the session,
///     so the ring was never the limit.
///
/// Locked here: (a) the script's press cadence, so a future band measurement
/// cannot silently keep a script that skips the movie; (b) the two exonerated
/// guards arithmetically; (c) the witness line this phase added, so a later
/// phase cannot drop the instrument that made the measurement possible.
#[test]
fn movie_session_ends_on_the_skip_key_edge_not_a_starved_ring() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");

    // (a) the operator session is a continuous Start mash, and it has a press
    //     in the movie window (frame 589) as well as one before it (553).
    let script = std::fs::read_to_string(root.join("tools/nnk_operator_session.nnk"))
        .expect("cannot read tools/nnk_operator_session.nnk");
    let mut keys: Vec<(u32, u32)> = Vec::new();
    for line in script.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut it = line.split_whitespace();
        let (f, m) = match (it.next(), it.next()) {
            (Some(f), Some(m)) => (f, m),
            _ => continue,
        };
        let frame: u32 = f.parse().expect("frame index");
        let mask = u32::from_str_radix(m.trim_start_matches("0x"), 16).expect("mask");
        keys.push((frame, mask));
    }
    assert!(keys.len() >= 100, "regression: the operator session shrank to {} entries", keys.len());
    const START: u32 = 0x3F7;
    const IDLE: u32 = 0x3FF;
    let start_frames: Vec<u32> = keys.iter().filter(|k| k.1 == START).map(|k| k.0).collect();
    assert!(start_frames.len() >= 30, "regression: the Start cadence is gone ({} presses)", start_frames.len());
    assert_eq!(start_frames[0], 13, "fixture: the first Start press is at frame 13");
    for w in start_frames.windows(2) {
        let d = w[1] - w[0];
        assert!(
            d == 36 || (d == 72 && w[0] == 877),
            "fixture: Start cadence changed at frame {} (gap {}) — the 72-frame gap is the \
             operator's A press at frame 917 replacing one Start",
            w[0], d
        );
    }
    assert!(start_frames.contains(&589), "regression: the press that ends the movie session (frame 589) left the script");
    assert!(start_frames.contains(&553), "fixture: the last press before the session starts is at frame 553");
    assert!(keys.iter().any(|k| k.0 == 917 && k.1 == 0x3FE),
        "fixture: the operator's A press at frame 917 (the movie skip) is gone");
    assert_eq!(keys.last().map(|k| k.1), Some(IDLE), "fixture: the session must end idle (no stuck key)");

    // (b) the two guards the card suspected, arithmetically exonerated with the
    //     measured words at the stop.
    let total_frames = 0x525u32;             // player+0x0C at the stop
    let decoded = 7u32;                       // player+0x98
    let presented = 7u32;                     // player+0xA0
    let ring_other = 1u32;                    // player+0x9C
    assert_eq!(total_frames, 1317, "fixture: op_u carries 1317 frames (MODS header)");
    assert_ne!(decoded, total_frames, "the 'movie finished' return at 0x0207AFC4 must not fire at the stop");
    assert!(ring_other <= presented, "the ring-full early return at 0x0207B1DC must not fire at the stop");
    assert_eq!(decoded, presented, "one present per decode step (measured: 7 and 7)");
    // The measured ordering that pins the exit to the skip edge: the last
    // present, then the taken `bne` at 0x0207D248, then — in the control where
    // that press is removed — the 8th present.
    let last_present: u64 = 326_421_022;
    let skip_exit: u64 = 327_550_216;
    let control_present8: u64 = 327_673_343;
    assert!(last_present < skip_exit && skip_exit < control_present8,
        "fixture: the skip edge must sit between the last present and the control's 8th present");
    assert_eq!(0x0207_D248u32 - 0x0207_D230u32, 6 * 4,
        "fixture: `and r1, r0, #9` is the 6th instruction before the `bne`");

    // A re-implementation of the loop's exit decision from the disassembly: the
    // session ends only on (i) finished, (ii) ring full, (iii) an A/Start edge.
    let skip_mask = |k: &[(u32, u32)], frame: u32| -> u32 {
        let mut v = IDLE;
        for (f, m) in k {
            if *f <= frame {
                v = *m;
            }
        }
        !v & 0x9                          // active-low: A = bit 0, Start = bit 3
    };
    assert_eq!(skip_mask(&keys, 553), 0x8, "fixture: frame 553 is a Start press");
    assert_eq!(skip_mask(&keys, 588), 0, "fixture: Start is released again at frame 555");
    assert_eq!(skip_mask(&keys, 589), 0x8, "fixture: frame 589 is the Start press that ends the session");
    let mut session_frames = 0u32;
    let mut ended_by_skip = false;
    for frame in 578u32..=700 {
        // The two non-key exits are evaluated with the measured stop words: the
        // "finished" test needs [0x98] to have reached 1317 (it is 7), the
        // ring-full test is false at the stop (checked above).
        if decoded >= total_frames || ring_other > presented {
            break;
        }
        if skip_mask(&keys, frame) != 0 {
            ended_by_skip = true;
            break;
        }
        session_frames += 1;
    }
    assert!(ended_by_skip, "regression: the scripted session must end on a skip edge");
    assert!(session_frames > 7 && session_frames < total_frames,
        "fixture: the scripted session dies inside the movie ({} frames)", session_frames);
    // The control: cut the schedule after frame 555 and there is no skip edge
    // left in the same window, so the loop must keep going (measured: the 8th
    // present appears at step 327,673,343 instead of never).
    let control: Vec<(u32, u32)> = keys.iter().cloned().filter(|k| k.0 <= 555).collect();
    assert_eq!(skip_mask(&control, 589), 0,
        "regression: the frame-555 control must leave no skip edge at frame 589");
    assert_eq!(skip_mask(&control, 700), 0, "fixture: the control has no press after frame 555");

    // (c) the witness line that made this measurable must survive.
    let inc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");
    for needle in [
        "pA[98]=%08X [9C]=%08X [A0]=%08X [A8]=%08X [AC]=%08X [B0]=%08X",
        "hd[00]=%08X [04]=%08X [08]=%08X [0C]=%08X",
        "clk[0C]=%08X [10]=%08X [14]=%08X",
        "ld32(p + 0x9Cu), ld32(p + 0xA0u)",
        "ld32(p + 0x0Cu), ld32(p + 0x10u), ld32(p + 0x14u)",
    ] {
        assert!(inc.contains(needle), "regression: the Phase-5AD witness field is gone: {needle}");
    }
    // The added fields are reads only: no guest-visible store in the block.
    let sample = inc.find("pA[98]=%08X").expect("witness line");
    let block = &inc[sample..(sample + 4000).min(inc.len())];
    assert!(!block.contains("st32(p"), "regression: the p5u sample must not write guest state");
}

/// Phase-5AE — the per-step caches are EXACT (a performance change must not
/// move a single guest-visible value).
///
/// Measured (PHASE5AE.md): the runtime spent ~1306 host cycles per dispatched
/// guest instruction on the notice-screen loop, 58% of it in two `getenv()`
/// calls and three 64-bit divisions inside the per-step clock/display/probe
/// chain.  The cures are caches, and a cache in an emulator core is only
/// admissible if it is bit-exact:
///
///   * `vcount()` — `(cycles / 2130) % 263` became a cached scanline window
///     (`vc_win_lo <= cycles < vc_win_hi`).  `cycles` is monotonic in the run
///     loop but NOT here: a card-bus stall adds `card_cycles - t0` in one jump,
///     so the cache must recompute outside the window.  This test re-implements
///     both forms and proves they agree over a unit walk, the scanline and
///     frame-wrap boundaries, arbitrary forward jumps and a backward jump.
///   * `nnk_display_tick()` — returns early when the scanline has not changed;
///     the frame edge it acts on is the same `<192 -> >=192` test.
///   * the debug/probe chain and the `NNK_P5U` hook — gated on parsed state
///     only (`ckpt_pc || dbg_pc || dc_npc || pred_npc`, `nnk_p5u_on`), never on
///     anything a run can change.
#[test]
fn phase_5ae_per_step_caches_are_bit_exact() {
    const CYCLES_PER_SCANLINE: u64 = 2130;
    const SCANLINES: u64 = 263;
    let reference = |cycles: u64| ((cycles / CYCLES_PER_SCANLINE) % SCANLINES) as u32;

    struct Vc {
        lo: u64,
        hi: u64,
        val: u32,
    }
    impl Vc {
        fn new() -> Self {
            Vc { lo: 0, hi: 0, val: 0 }
        }
        fn get(&mut self, cycles: u64) -> u32 {
            if cycles >= self.lo && cycles < self.hi {
                return self.val;
            }
            let q = cycles / CYCLES_PER_SCANLINE;
            self.lo = q * CYCLES_PER_SCANLINE;
            // C unsigned arithmetic wraps (defined behaviour): for a `cycles`
            // inside the last scanline of the 64-bit range `hi` wraps below
            // `lo`, and the window simply never matches, so the next call
            // recomputes.  Mirror that exactly.
            self.hi = self.lo.wrapping_add(CYCLES_PER_SCANLINE);
            self.val = (q % SCANLINES) as u32;
            self.val
        }
    }

    let mut vc = Vc::new();
    // (a) a unit walk across ~5 frames, in order (the run loop's steady state)
    for cycles in 0..(5 * SCANLINES * CYCLES_PER_SCANLINE) {
        assert_eq!(vc.get(cycles), reference(cycles), "unit walk at {cycles}");
    }
    // (b) every scanline/frame/64-bit boundary neighbourhood, in order
    let mut probes: Vec<u64> = Vec::new();
    for k in 0..(3 * SCANLINES + 2) {
        let b = k * CYCLES_PER_SCANLINE;
        probes.extend([b.saturating_sub(1), b, b + 1, b + CYCLES_PER_SCANLINE - 1]);
    }
    probes.extend([
        0,
        1,
        u64::MAX / 2,
        (u64::MAX / 2) - 1,
        u64::MAX - 1,
        u64::MAX,
    ]);
    probes.sort_unstable();
    for cycles in probes {
        assert_eq!(vc.get(cycles), reference(cycles), "boundary at {cycles}");
    }
    // (c) the card-stall jump: forward by many frames in one step, then BACK
    // (the cache window must not serve a stale value).
    for jumps in [1u64, 7, 2130, 559_590, 559_591, 1 << 20, 1 << 32] {
        let mut c = 1234u64;
        for _ in 0..64 {
            assert_eq!(vc.get(c), reference(c), "forward jump at {c}");
            c = c.wrapping_add(jumps);
        }
        let back = c / 3;
        assert_eq!(vc.get(back), reference(back), "backward jump at {back}");
    }

    // (d) the sources keep the caches and the gates.
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
    let rs = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");
    for needle in [
        "static unsigned long long vc_win_lo, vc_win_hi;",
        "if (cycles >= vc_win_lo && cycles < vc_win_hi) return vc_win_val;",
        "if (vc == (uint32_t)gxd_last_vc) return 0;",
    ] {
        assert!(rs.contains(needle), "regression: prelude cache lost: {needle}");
    }
    let splice = std::fs::read_to_string(root.join("tools/p5ae_runtime_splice.py"))
        .expect("cannot read tools/p5ae_runtime_splice.py");
    for needle in [
        "T1_OLD",
        "vc_win_val",
        "nnk_probe_mask",
        "nnk_p5u_on",
        "#define NNK_P5U_GATE 1",
    ] {
        assert!(splice.contains(needle), "regression: Phase-5AE splice lost: {needle}");
    }
    let build = std::fs::read_to_string(root.join("tools/build_split.sh"))
        .expect("cannot read tools/build_split.sh");
    assert!(
        build.contains("p5ae_runtime_splice.py") && build.contains("nnk_probe_mask"),
        "regression: the frozen image would silently be built without the Phase-5AE prelude"
    );
    let input = std::fs::read_to_string(root.join("tools/nnk_input.inc"))
        .expect("cannot read tools/nnk_input.inc");
    for needle in ["getenv(\"NNK_INPUT_FILE\")", "getenv(\"NNK_INPUT_RECORD\")"] {
        let cached = format!("if (!init) {{ init = 1; p = {}; }}", needle);
        assert!(input.contains(&cached), "regression: per-step env lookup returned: {needle}");
    }
    let smc = std::fs::read_to_string(root.join("tools/nnk_smc.inc"))
        .expect("cannot read tools/nnk_smc.inc");
    assert!(
        smc.contains("if (!smc_dblk[o >> 8]) return 0;"),
        "regression: the dispatch probe lost its 256-byte dirty block"
    );
    assert!(
        smc.contains("if (sg == -1) return 0;"),
        "regression: the dispatch probe lost the pre-resolved band map"
    );
    let dma = std::fs::read_to_string(root.join("tools/nnk_dma.inc"))
        .expect("cannot read tools/nnk_dma.inc");
    assert!(
        dma.contains("if (ticks < (uint64_t)(p4u_tm_ovf[t] + 1u) * (uint64_t)period) continue;"),
        "regression: the timer tick divides on every step again"
    );
}

// ---------------------------------------------------------------------------
// class 43 (Phase-5AF): the overlay map is the ROM's table, not a per-load dump
// ---------------------------------------------------------------------------
/// Overlay arenas produced several "untranslated pc" barriers, and the runtime
/// knew about them only through per-load RAM dumps (Phase-4G) -- and the frozen
/// image the acceptance runs are built from carried no overlay knowledge at
/// all.  Phase-5AF makes the map table-driven: the ROM's 21-entry ARM9 overlay
/// table (OVT) supplies base / code size / bss / static-constructor range for
/// every overlay, the runtime reports an untranslated PC against it, and the
/// build splices the identical canonical block into the frozen image.
///
/// Locked here (no game bytes needed -- the tool's --self-test parses a
/// synthetic ROM built inside the test process):
///   1. the measured OVT field offsets (id@+0, ram@+4, size@+8, bss@+0xC,
///      ctor@+0x10/+0x14, file_id@+0x18, flags|stored@+0x1C) and the
///      code/bss/ctor classification agree between tools/ovl_table.py's
///      self-test and the runtime's parser;
///   2. the emitter's registry block assigns every one of those fields;
///   3. the untranslated-PC path reports through the table instead of halting
///      with a bare address, and the dump hooks exist;
///   4. tools/build_split.sh splices that block into a frozen image, and the
///      splice copies the canonical text rather than a second copy of it.
#[test]
fn phase_5af_overlay_map_is_table_driven() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("..");

    // (1) the tool's parser: offsets + classification, exercised on a synthetic
    // 4-overlay ROM (two overlays in one arena = the aliasing case).
    let out = std::process::Command::new("python3")
        .arg(root.join("tools/ovl_table.py"))
        .arg("--self-test")
        .output()
        .expect("cannot run tools/ovl_table.py --self-test");
    assert!(
        out.status.success(),
        "tools/ovl_table.py --self-test failed:\n{}\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    );
    let tool = std::fs::read_to_string(root.join("tools/ovl_table.py"))
        .expect("cannot read tools/ovl_table.py");
    for needle in [
        "F_ID, F_RAM, F_SIZE, F_BSS, F_CTOR_LO, F_CTOR_HI, F_FILE, F_FLAGS = range(0, 8)",
        "id=w[F_ID], base=w[F_RAM], size=w[F_SIZE], bss=w[F_BSS],",
        "ctor_lo=w[F_CTOR_LO], ctor_hi=w[F_CTOR_HI], file_id=file_id,",
        "flags & 0x01000000",
        "kind = 'ctor'",
    ] {
        assert!(tool.contains(needle), "regression: ovl_table.py lost {needle:?}");
    }

    // (2) the emitter's canonical block carries the whole table.
    let rs = std::fs::read_to_string(root.join("nnktool/src/recompile.rs"))
        .expect("cannot read nnktool/src/recompile.rs");
    let begin = rs.find("BEGIN_NNK_OVL_BLOCK").expect("overlay block markers missing");
    let end = rs.find("END_NNK_OVL_BLOCK").expect("overlay block markers missing");
    let block = &rs[begin..end];
    for needle in [
        "ovl_tab[ovl_n].id = ovl_rd32(o);",
        "ovl_tab[ovl_n].ram = ovl_rd32(o + 4u);",
        "ovl_tab[ovl_n].ram_size = ovl_rd32(o + 8u);",
        "ovl_tab[ovl_n].bss = ovl_rd32(o + 12u);",
        "ovl_tab[ovl_n].ctor_lo = ovl_rd32(o + 16u);",
        "ovl_tab[ovl_n].ctor_hi = ovl_rd32(o + 20u);",
        "static void io_ovl_table_dump(const char *path)",
        "static void io_ovl_pc_report(uint32_t pc)",
        "kind = \"ctor\"",
        "static void io_ovl_dump_tick(uint32_t pc)",
        "static void io_ovl_dump_exit(void)",
    ] {
        assert!(block.contains(needle), "regression: overlay block lost {needle:?}");
    }

    // (3) the untranslated-PC path must classify through the table first.
    let default_arm = r#"            io_ovl_pc_report(pc);   /* Phase-5AF */\n            halt_reason(\"dispatch to untranslated pc\");"#;
    assert!(
        rs.contains(default_arm),
        "regression: an untranslated PC halts without the table-driven overlay report"
    );
    let run_hook = r#"        io_ovl_dump_tick(pc);   /* Phase-5AF */\n        switch (pc) {"#;
    assert!(rs.contains(run_hook), "regression: the arena dump hook left the dispatch loop");

    // (4) the build must splice it into a frozen image, from the SAME text.
    let build = std::fs::read_to_string(root.join("tools/build_split.sh"))
        .expect("cannot read tools/build_split.sh");
    assert!(
        build.contains("p5af_ovl_table_splice.py") && build.contains("io_ovl_pc_report"),
        "regression: the frozen image would be built without the overlay table"
    );
    let splice = std::fs::read_to_string(root.join("tools/p5af_ovl_table_splice.py"))
        .expect("cannot read tools/p5af_ovl_table_splice.py");
    assert!(
        splice.contains("BEGIN_NNK_OVL_BLOCK") && splice.contains("nnktool', 'src', 'recompile.rs'"),
        "regression: the splice kept its own copy of the block instead of the canonical one"
    );
    for hook in [
        "static void io_load_rom(void) {",
        "    fclose(f); rom_loaded = 1;\\n",
        "    card_op = card_cmd[0];\\n",
        "static void run(void) {",
        "        switch (pc) {\\n",
        "            halt_reason(\"dispatch to untranslated pc\");\\n",
    ] {
        assert!(splice.contains(hook), "regression: splice lost the anchor {hook:?}");
    }
}
