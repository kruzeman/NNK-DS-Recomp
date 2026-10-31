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
