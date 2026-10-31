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
    // BLX must link pc+4|1 (ARM-state, 4-byte instruction) exactly like the
    // emitter's Kind::Bx arm.
    let block = &src[bx..dp];
    assert!(
        block.contains("r[14] = (pc + 4u) | 1u;"),
        "regression: smc BLX(register) no longer links pc+4|1"
    );
}
