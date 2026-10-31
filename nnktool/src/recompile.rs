// recompile-entry: translate the ARM-state entry function of the ARM9 image
// into host C.  One C function per guest function, PC-dispatch switch loop,
// guest registers/flags, flat guest memory window.  Direct calls are emitted
// as logged call shims (Phase-1A scope: only the entry function is
// translated; every shimmed call is printed so nothing looks successful).

use crate::decode::*;
use crate::recover::{recover_seeds, trace_function, FuncKey, Mode};
use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

pub struct RecStats {
    pub insns: usize,
    pub calls_shimmed: usize,
    pub indirects: usize,
    pub halts: usize,
    pub bytes: usize,
}

fn reg_c(r: u8, pc: u32) -> String {
    if r == 15 {
        "(pc + 8u)".to_string()
    } else {
        format!("r[{}]", r)
    }
}

/// Register read for a Thumb instruction.  R15 reads as (addr + 4) in
/// Thumb; the caller uses `pc_lit` instead where the architecture forces the
/// word-aligned form (literal loads and ADR).
fn reg_c_t(r: u8, pc: u32) -> String {
    if r == 15 {
        "((pc + 4u))".to_string()
    } else {
        format!("r[{}]", r)
    }
}

/// Word-aligned Thumb PC for `LDR rd, [pc, #imm]` and `ADD rd, pc, #imm`:
/// (address of instruction + 4) & ~3, folded to a constant at emit time.
fn pc_lit(addr: u32) -> String {
    format!("0x{:08X}u", addr.wrapping_add(4) & !3)
}

fn reg_mode(r: u8, pc: u32, thumb: bool) -> String {
    if thumb {
        if r == 15 {
            // Only formats 6 (literal) and 12 (ADR) may read the PC in the
            // Thumb data-processing/memory forms this emitter sees, and both
            // use the aligned value.
            pc_lit(pc)
        } else {
            format!("r[{}]", r)
        }
    } else {
        reg_c(r, pc)
    }
}

/// Destination of a write: R15 writes update the dispatch `pc`, never the
/// register file slot (the dispatch loop's `pc` is authoritative).  Bit0 of
/// a value written to the PC selects the instruction state — the switch key
/// is the even address in both states, so it is masked off here.
fn dst_c(rd: u8) -> String {
    if rd == 15 {
        "pc".to_string()
    } else {
        format!("r[{}]", rd)
    }
}

/// Assignment form of `dst_c` for a C expression (see `dst_c`).
fn dst_assign(rd: u8, expr: &str) -> String {
    if rd == 15 {
        format!("pc = {} & ~1u;", expr)
    } else {
        format!("r[{}] = {};", rd, expr)
    }
}

/// (C expression, does-it-shift) for a shifter operand.
fn operand_c(o: &Operand, pc: u32, thumb: bool) -> (String, bool) {
    match o {
        Operand::Imm(v) => (format!("0x{:08X}u", v), false),
        Operand::Reg { rm, kind, amount, by_reg } => {
            let src = if thumb { reg_c_t(*rm, pc) } else { reg_c(*rm, pc) };
            let shifted = *by_reg || *kind != 0 || *amount != 0;
            // A shift by a register with amount 0 is a NO-OP (flags
            // unchanged) in both ARM and Thumb; `sh_op`'s immediate-0 path
            // means LSR #32 / RRX instead, so by-register shifts go through
            // `sh_op_reg`.
            let (fnm, amt) = if *by_reg {
                ("sh_op_reg", format!("r[{}] & 255u", amount))
            } else {
                ("sh_op", format!("{}u", amount))
            };
            (format!("{}({}, {}u, {}, c)", fnm, src, kind, amt), shifted)
        }
    }
}

fn imm_off_c(off: i32) -> String {
    if off >= 0 {
        format!("0x{:X}u", off)
    } else {
        format!("-0x{:X}u", -(off as i64) as u32)
    }
}

/// Emit the effective-address computation for `ea` on base register `rn`.
/// Opens a `{` block; the caller closes it.  Returns the address expression.
fn ea_emit(out: &mut Vec<String>, ea: &Ea, rn: u8, pc: u32, thumb: bool) -> String {
    let base = reg_mode(rn, pc, thumb);
    match ea {
        Ea::Imm { off, pre, wb, .. } => {
            let offc = imm_off_c(*off);
            if *pre {
                out.push(format!("uint32_t _b = {}; uint32_t _ea = _b + {};", base, offc));
                if *wb {
                    out.push(format!("r[{}] = _ea;", rn));
                }
                "_ea".to_string()
            } else {
                out.push(format!("uint32_t _b = {}; uint32_t _ea = _b;", base));
                if *wb {
                    out.push(format!("r[{}] = _b + {};", rn, offc));
                }
                "_ea".to_string()
            }
        }
        Ea::Reg { rm, kind, amount, pre, wb, up } => {
            // Register-offset addressing has no shift; the shifted-register
            // addressing form uses kind/amount (amount holds Rs for by_reg).
            let src = if thumb { reg_c_t(*rm, pc) } else { reg_c(*rm, pc) };
            let off = if *kind == 0 && *amount == 0 {
                src
            } else {
                let amt = if by_reg_of(ea) {
                    format!("r[{}] & 255u", amount)
                } else {
                    format!("{}u", amount)
                };
                format!("sh_op({}, {}u, {}, c)", src, kind, amt)
            };
            let combine = if *up { "+" } else { "-" };
            if *pre {
                out.push(format!("uint32_t _b = {}; uint32_t _ea = _b {} {};", base, combine, off));
                if *wb {
                    out.push(format!("r[{}] = _ea;", rn));
                }
                "_ea".to_string()
            } else {
                out.push(format!("uint32_t _b = {}; uint32_t _ea = _b;", base));
                if *wb {
                    out.push(format!("r[{}] = _b {} {};", rn, combine, off));
                }
                "_ea".to_string()
            }
        }
    }
}

fn by_reg_of(ea: &Ea) -> bool {
    // Register-offset addressing modes in the decoder never set by_reg; the
    // shifted form stores Rs in `amount` and is signalled by kind != 0.
    match ea {
        Ea::Reg { kind, .. } => *kind != 0,
        _ => false,
    }
}

fn cond_expr(c: u8) -> &'static str {
    match c {
        0 => "z",
        1 => "!z",
        2 => "c",
        3 => "!c",
        4 => "n",
        5 => "!n",
        6 => "v",
        7 => "!v",
        8 => "(c && !z)",
        9 => "(!c || z)",
        10 => "(n == v)",
        11 => "(n != v)",
        12 => "(!z && n == v)",
        13 => "(z || n != v)",
        _ => "1",
    }
}

fn cond_of(w: u32) -> u8 {
    ((w >> 28) & 15) as u8
}

struct EmitOutcome {
    /// Measured card-bus cycle cost of the instruction (Phase-4D).
    cycles: u32,
    body: Vec<String>,
    flow: Flow,
    guarded: bool,
    cond: String,
    pc_written: bool,
    shimmed: bool,
    halts: bool,
}

/// Emit C statements for one instruction in either state.
///
/// `thumb` selects the instruction set: PC reads (R15) are (addr + 4) in
/// Thumb and (addr + 8) in ARM, a Thumb BL links with bit0 set (the return
/// address is a Thumb address), and a Thumb SVC carries its number in bits
/// 7..0 instead of 16..23.
///
/// `global == false` is the Phase-1A single-function mode (direct calls are
/// logged shims, returns leave the dispatch function, SWI halts).  `global ==
/// true` is the `recompile-all` mode: one program-wide PC-dispatch image, so
/// direct calls/returns become real pc updates, SWIs become HLE calls that
/// return to the next instruction, and interworking (ARM <-> Thumb) is just
/// the next dispatch key — case labels are even addresses in both states, so
/// Card-bus cycle cost of one guest instruction (Phase-4D).
///
/// Measured, not guessed: the melonDS ARM9 IO capture carries the master
/// timestamp (`ts=`) of every bus access, so the guest's card poll loop at
/// 0x02016B30..0x02016B50 (ROMCNT=0xA1416657) gives the costs directly:
///   poll iteration (DRQ clear)      9 = ldr(1) tst(1) beq-taken(3) tst(1) bne-taken(3)
///   DRQ poll -> ROMDATA read        6 = ldr(1) tst(1) beq-not-taken(3) ldr(1)
///   ROMDATA read -> next poll       9 = cmp(1) strcc(3) addcc(1) tst(1) bne-taken(3)
/// That is: loads 1, stores 3, every branch 3, everything else 1.  With it the
/// model reproduces melonDS's 909 ROMCNT polls before the first DRQ of that
/// transfer (see p4d_sim3.py and the "io: card drq after 909 polls" log).
fn card_cost(kind: &Kind) -> u32 {
    match kind {
        Kind::Mem { load, .. } => if *load { 1 } else { 3 },
        /* LDM/STM.  A loaded PC (a return) costs the same as any other load in
         * this four-class table.  melonDS's ARMv5 pipeline charges far more
         * for it — measured: `str ROMCnt (0x02016930); pop {r3,pc}; mov;
         * ldr ROMCnt (poll)` spans 23 master cycles against 3 here — and that
         * residual is exactly the remaining 5-PC trace offset documented in
         * PHASE4D_CARD_TIMING.md.  Fitting this single cost does NOT help:
         * measured sweep (run, loop visits before the first word, strict
         * prefix) = (3, 910, 3,643,483), (12, 909, 3,643,455),
         * (21, 908, 3,643,450) — it only trades one offset for another, so
         * the value stays at the honest dts-measured 1/3/3/1 table. */
        Kind::LdmStm { load, .. } => if *load { 1 } else { 3 },
        Kind::Swp { .. } => 3,
        Kind::B { .. } | Kind::Bx { .. } | Kind::BlxImm { .. } => 3,
        _ => 1,
    }
}

/// the state is implied by which case is reached.
fn emit_insn(
    insn: &Insn,
    addr: u32,
    thumb: bool,
    guarded: bool,
    ce: String,
    out: &mut Vec<String>,
    global: bool,
) -> EmitOutcome {
    let _ = out;
    let mut body: Vec<String> = Vec::new();
    let mut flow = Flow::Next;
    let mut pc_written = false;
    let mut shimmed = false;
    let mut halts = false;

    match &insn.kind {
        Kind::Dp { op, s, rd, rn, src } => {
            let (opnd, shifted) = operand_c(src, addr, thumb);
            let rn_v = reg_mode(*rn, addr, thumb);
            let test = matches!(op, 8..=11);
            body.push(format!("{{ uint32_t _rn = {}; uint32_t _op = {};", rn_v, opnd));
            match op {
                0 | 1 | 12 | 14 | 15 => {
                    let expr = match op {
                        0 => "(_rn & _op)".to_string(),
                        1 => "(_rn ^ _op)".to_string(),
                        12 => "(_rn | _op)".to_string(),
                        14 => "(_rn & ~_op)".to_string(),
                        _ => "(~_op)".to_string(),
                    };
                    body.push(format!("uint32_t _res = {};", expr));
                    if !test {
                        body.push(dst_assign(*rd, "_res"));
                    }
                    if *s {
                        body.push("z = (_res == 0); n = (_res >> 31) & 1;".into());
                        if shifted {
                            body.push("c = carry_out;".into());
                        }
                    }
                }
                13 => {
                    body.push(format!("uint32_t _res = (uint32_t)_op;"));
                    if !test {
                        body.push(dst_assign(*rd, "_res"));
                    }
                    if *s {
                        body.push("z = (_res == 0); n = (_res >> 31) & 1;".into());
                        if shifted {
                            body.push("c = carry_out;".into());
                        }
                    }
                }
                2 | 3 | 4 | 5 | 6 | 7 | 10 | 11 => {
                    // Arithmetic: compute result, unsigned carry, signed overflow.
                    let (res, uw, sw) = match op {
                        2 | 10 => (
                            "(_rn - _op)".to_string(),
                            "(uint64_t)_rn - (uint64_t)_op".to_string(),
                            "(int64_t)(int32_t)_rn - (int64_t)(int32_t)_op".to_string(),
                        ),
                        3 => (
                            // RSB (reverse subtract): op - rn.
                            "(_op - _rn)".to_string(),
                            "(uint64_t)_op - (uint64_t)_rn".to_string(),
                            "(int64_t)(int32_t)_op - (int64_t)(int32_t)_rn".to_string(),
                        ),
                        4 | 11 => (
                            // ADD and CMN (compare negative = rn + op).
                            "(_rn + _op)".to_string(),
                            "(uint64_t)_rn + (uint64_t)_op".to_string(),
                            "(int64_t)(int32_t)_rn + (int64_t)(int32_t)_op".to_string(),
                        ),
                        5 => (
                            "(_rn + _op + c)".to_string(),
                            "(uint64_t)_rn + (uint64_t)_op + c".to_string(),
                            "(int64_t)(int32_t)_rn + (int64_t)(int32_t)_op + (int64_t)c"
                                .to_string(),
                        ),
                        6 => (
                            "(_rn - _op - (c ^ 1u))".to_string(),
                            "(uint64_t)_rn - (uint64_t)_op - (c ^ 1u)".to_string(),
                            "(int64_t)(int32_t)_rn - (int64_t)(int32_t)_op - (int64_t)(c ^ 1u)"
                                .to_string(),
                        ),
                        _ => (
                            "(_op - _rn - (c ^ 1u))".to_string(),
                            "(uint64_t)_op - (uint64_t)_rn - (c ^ 1u)".to_string(),
                            "(int64_t)(int32_t)_op - (int64_t)(int32_t)_rn - (int64_t)(c ^ 1u)"
                                .to_string(),
                        ),
                    };
                    body.push(format!("uint32_t _res = (uint32_t)({});", res));
                    body.push(format!("uint64_t _uw = {};", uw));
                    body.push(format!("int64_t _sw = {};", sw));
                    if !test {
                        body.push(dst_assign(*rd, "_res"));
                    }
                    if *s {
                        body.push("z = (_res == 0); n = (_res >> 31) & 1;".into());
                        match op {
                            // SUB/RSB/SBC/RSC/CMP: C = NOT borrow of the
                            // 64-bit difference (_uw>>32 != 0 iff borrow).
                            // CMP (op 10) computes rn - op, same as SUB.
                            2 | 3 | 6 | 7 | 10 => {
                                body.push("c = (_uw >> 32) ? 0u : 1u;".into())
                            }
                            // ADD/ADC/CMN: C = unsigned carry out of the sum.
                            _ => body.push("c = (uint32_t)(_uw >> 32);".into()),
                        }
                        body.push(
                            "v = ((_sw > (int64_t)0x7FFFFFFF) || (_sw < -(int64_t)0x80000000)) ? 1u : 0u;".into(),
                        );
                    }
                }
                8 | 9 => {
                    let expr = if *op == 8 { "(_rn & _op)" } else { "(_rn ^ _op)" };
                    body.push(format!("uint32_t _res = (uint32_t)({});", expr));
                    if *s {
                        body.push("z = (_res == 0); n = (_res >> 31) & 1;".into());
                        if shifted {
                            body.push("c = carry_out;".into());
                        }
                    }
                }
                _ => body.push("/* unreachable dp op */".into()),
            }
            body.push("}".into());
            if *rd == 15 && *s && !test {
                // SUBS/MOVS ... PC outside USR/SYS mode is the ARM exception
                // return: the PC takes the computed value AND CPSR <- SPSR
                // (flags + mode, so r13/r14 swap back to the interrupted
                // bank).  In USR/SYS mode it is a plain branch.  Measure:
                // 0x0200729C `subs pc, lr, #4` under SPSR 0x0000001F returns
                // to SYS mode at lr-4 = 0x02015EC4.
                body.push(
                    "if (mode_priv()) { uint32_t _spsr = spsr_bank[mode_cur]; \
                     cpsr_set_ctl(_spsr & 0xFFu); n = (_spsr >> 31) & 1; z = (_spsr >> 30) & 1; \
                     c = (_spsr >> 29) & 1; v = (_spsr >> 28) & 1; }"
                        .into(),
                );
            }
            if *rd == 15 && !test {
                pc_written = true;
                flow = Flow::Indirect(None);
            }
        }
        Kind::Mul { kind, s, rd, rn, rm, ra } => {
            let rmv = reg_c(*rm, addr);
            let rav = reg_c(*ra, addr);
            let rnv = reg_c(*rn, addr);
            let (lo, hi) = match kind {
                MulKind::Mul => (
                    format!("(uint32_t)((uint64_t){} * (uint64_t){})", rmv, rav),
                    "0u".to_string(),
                ),
                MulKind::Mla => (
                    format!("(uint32_t)((uint64_t){} * (uint64_t){} + (uint64_t){})", rmv, rav, rnv),
                    "0u".to_string(),
                ),
                MulKind::Umull => (
                    format!("(uint32_t)((uint64_t){} * (uint64_t){})", rmv, rav),
                    format!("(uint32_t)(((uint64_t){} * (uint64_t){}) >> 32)", rmv, rav),
                ),
                MulKind::Smull => (
                    format!(
                        "(uint32_t)((int64_t)(int32_t){} * (int64_t)(int32_t){})",
                        rmv, rav
                    ),
                    format!(
                        "(uint32_t)(((int64_t)(int32_t){} * (int64_t)(int32_t){}) >> 32)",
                        rmv, rav
                    ),
                ),
                MulKind::Umlal => (
                    format!(
                        "(uint32_t)((uint64_t){} * (uint64_t){} + (((uint64_t)r[{}] << 32) | r[{}]))",
                        rmv, rav, rn, rd
                    ),
                    format!(
                        "(uint32_t)(((uint64_t){} * (uint64_t){} + (((uint64_t)r[{}] << 32) | r[{}])) >> 32)",
                        rmv, rav, rn, rd
                    ),
                ),
                MulKind::Smlal => (
                    format!(
                        "(uint32_t)((int64_t)(int32_t){} * (int64_t)(int32_t){} + (int64_t)(((uint64_t)r[{}] << 32) | r[{}]))",
                        rmv, rav, rn, rd
                    ),
                    format!(
                        "(uint32_t)(((int64_t)(int32_t){} * (int64_t)(int32_t){} + (int64_t)(((uint64_t)r[{}] << 32) | r[{}])) >> 32)",
                        rmv, rav, rn, rd
                    ),
                ),
            };
            body.push("{".into());
            body.push(format!("uint32_t _lo = {};", lo));
            let long = matches!(kind, MulKind::Umull | MulKind::Umlal | MulKind::Smull | MulKind::Smlal);
            if long {
                body.push(format!("uint32_t _hi = {};", hi));
            }
            body.push(format!("r[{}] = _lo;", rd));
            if long {
                body.push(format!("r[{}] = _hi;", rn));
            }
            if *s {
                body.push("z = (_lo == 0); n = (_lo >> 31) & 1;".into());
            }
            body.push("}".into());
        }
        Kind::Mrs { spsr, rd } => {
            if *spsr {
                // SPSR of the CURRENT mode (melonDS keeps one per mode).
                body.push(format!("r[{}] = spsr_bank[mode_cur];", rd));
            } else {
                body.push(format!(
                    "r[{}] = (n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x10u | cpsr_ctl;",
                    rd
                ));
            }
        }
        Kind::Msr { spsr, mask, src } => {
            let (opnd, _) = operand_c(src, addr, thumb);
            body.push(format!("{{ uint32_t _v = {};", opnd));
            if *spsr {
                // A write to SPSR never touches the current CPSR, and it goes
                // to the SPSR of the CURRENT mode (ARM ARM B4.1.3).
                let mut e = String::from("spsr_bank[mode_cur]");
                if mask & 1 != 0 {
                    e = format!("(({} & ~0xFFu) | (_v & 0xFFu))", e);
                }
                if mask & 8 != 0 {
                    e = format!("(({} & 0x0FFFFFFFu) | (_v & 0xF0000000u))", e);
                }
                body.push(format!("spsr_bank[mode_cur] = {};", e));
            } else {
                if mask & 1 != 0 {
                    body.push("cpsr_set_ctl(_v & 0xFFu);".into());
                }
                if mask & 8 != 0 {
                    body.push(
                        "n = (_v >> 31) & 1; z = (_v >> 30) & 1; c = (_v >> 29) & 1; v = (_v >> 28) & 1;"
                            .into(),
                    );
                }
            }
            body.push("}".into());
        }
        Kind::Bx { rm, link } => {
            let src = if thumb { reg_c_t(*rm, addr) } else { reg_c(*rm, addr) };
            pc_written = true;
            if *link {
                // BLX rm (either state): the return address is the next
                // instruction, and it is a THUMB address — BLX always
                // returns to Thumb state, so the link gets bit0 set (the
                // dispatcher masks it again on the way back).
                body.push(format!("r[14] = 0x{:08X}u;", addr.wrapping_add(4) | 1));
            }
            if *rm == 14 && !*link {
                if global {
                    // Return.  r[14] may carry the Thumb bit (Thumb callees
                    // link with bit0 set); the dispatch key is the even
                    // address in both states, so just mask it — this is the
                    // ARM<->Thumb interworking return path in both
                    // directions.
                    flow = Flow::Indirect(None);
                    body.push("{ uint32_t _t = r[14];".into());
                    body.push("pc = _t & ~1u; }".into());
                } else {
                    flow = Flow::Return;
                    body.push("pc = r[14];".into());
                }
            } else {
                // Register-indirect jump/call.  Bit0 of the target selects
                // the instruction state; the switch keys are even addresses
                // in both states, so the target's case is the state.
                flow = Flow::Indirect(None);
                body.push(format!("{{ uint32_t _t = {};", src));
                body.push("pc = _t & ~1u; }".into());
            }
        }
        Kind::Clz { rd, rm } => {
            body.push(format!("r[{}] = clz32({});", rd, reg_c(*rm, addr)));
        }
        Kind::Mem { load, byte, rd, rn, ea } => {
            body.push("{".into());
            let addr_expr = ea_emit(&mut body, ea, *rn, addr, thumb);
            if *load {
                if *rd == 15 {
                    // LDR pc: bit0 selects the target instruction state, the
                    // dispatch key is the even address either way.  The VALUE
                    // AT the effective address is the target (the SDK stub's
                    // `popeq {pc}` encodes as post-indexed ldr pc, [sp], #4 —
                    // using the address itself here jumped to the stack slot).
                    body.push(format!(
                        "uint32_t _t = {};",
                        if *byte {
                            format!("ld8({})", addr_expr)
                        } else {
                            format!("ld32({})", addr_expr)
                        }
                    ));
                    body.push("pc = _t & ~1u;".into());
                    pc_written = true;
                    flow = Flow::Indirect(None);
                } else {
                    body.push(format!(
                        "r[{}] = {};",
                        rd,
                        if *byte { format!("ld8({})", addr_expr) } else { format!("ld32({})", addr_expr) }
                    ));
                }
            } else {
                body.push(format!(
                    "{}({}, {});",
                    if *byte { "st8" } else { "st32" },
                    addr_expr,
                    if thumb { reg_c_t(*rd, addr) } else { reg_c(*rd, addr) }
                ));
            }
            body.push("}".into());
        }
        Kind::Hw { kind, rd, rn, ea } => {
            body.push("{".into());
            let addr_expr = ea_emit(&mut body, ea, *rn, addr, thumb);
            match kind {
                HwKind::Strh => body.push(format!("st16({}, (uint16_t){});", addr_expr, reg_c(*rd, addr))),
                HwKind::Ldrh => body.push(format!("r[{}] = ld16({});", rd, addr_expr)),
                HwKind::Ldrsb => body.push(format!("r[{}] = (uint32_t)(int32_t)(int8_t)ld8({});", rd, addr_expr)),
                HwKind::Ldrsh => body.push(format!("r[{}] = (uint32_t)(int32_t)(int16_t)ld16({});", rd, addr_expr)),
                HwKind::Strd => {
                    body.push(format!("st32({}, {});", addr_expr, reg_c(*rd, addr)));
                    body.push(format!("st32({} + 4u, {});", addr_expr, reg_c(*rd + 1, addr)));
                }
                HwKind::Ldrd => {
                    body.push(format!("r[{}] = ld32({});", rd, addr_expr));
                    body.push(format!("r[{}] = ld32({} + 4u);", rd + 1, addr_expr));
                }
            }
            body.push("}".into());
        }
        Kind::LdmStm { load, rn, wb, list, pu, s } => {
            let regs: Vec<u8> = (0..16).filter(|i| list & (1 << i) != 0).map(|i| i as u8).collect();
            let cnt = regs.len() as u32;
            let base = reg_c(*rn, addr);
            // S-bit with no PC in the list = the USER-bank form: in a
            // privileged mode r13/r14 are transferred to/from the user bank,
            // NOT the current one (ARMv5 LDM/STM `^`).  With the PC in the
            // list it is the exception-return form (not modelled: the boot
            // uses `subs pc, lr, #4` instead).
            let user_bank = *s && (list & 0x8000) == 0;
            // Lowest transfer address per addressing mode:
            // DA: base-4n+4, IA: base, DB: base-4n, IB: base+4.
            let start = match pu {
                0 => format!("(_base - {}u + 4u)", cnt * 4),
                1 => "_base".to_string(),
                2 => format!("(_base - {}u)", cnt * 4),
                _ => format!("(_base + 4u)"),
            };
            body.push(format!("{{ uint32_t _base = {};", base));
            body.push(format!("uint32_t _a = {};", start));
            let mut idx = 0u32;
            for rr in &regs {
                if *load {
                    if *rr == 15 {
                        // LDM with pc (Thumb `pop {..., pc}` is THE return
                        // idiom): the loaded word's bit0 selects the target
                        // state, the dispatch key is its even address.
                        body.push(format!("uint32_t _t = ld32(_a + {}u);", idx * 4));
                        body.push("pc = _t & ~1u;".into());
                        pc_written = true;
                        flow = Flow::Indirect(None);
                    } else if user_bank && *rr == 13 {
                        body.push(format!("bank13_set_usr(ld32(_a + {}u));", idx * 4));
                    } else if user_bank && *rr == 14 {
                        body.push(format!("bank14_set_usr(ld32(_a + {}u));", idx * 4));
                    } else {
                        body.push(format!("r[{}] = ld32(_a + {}u);", rr, idx * 4));
                    }
                } else if user_bank && *rr == 13 {
                    body.push(format!("st32(_a + {}u, bank13_get_usr());", idx * 4));
                } else if user_bank && *rr == 14 {
                    body.push(format!("st32(_a + {}u, bank14_get_usr());", idx * 4));
                } else {
                    body.push(format!("st32(_a + {}u, {});", idx * 4, if thumb { reg_c_t(*rr, addr) } else { reg_c(*rr, addr) }));
                }
                idx += 1;
            }
            if *wb && !regs.is_empty() {
                let sign = if *pu == 1 || *pu == 3 { "+" } else { "-" };
                body.push(format!("r[{}] = _base {} {}u;", rn, sign, cnt * 4));
            }
            body.push("}".into());
        }
        Kind::Swp { byte, rd, rn, rm } => {
            body.push(format!(
                "{{ uint32_t _ea = {}; uint32_t _t = {}; {}(_ea, {}); r[{}] = _t; }}",
                reg_c(*rn, addr),
                if *byte { format!("ld8(_ea)") } else { "ld32(_ea)".into() },
                if *byte { "st8" } else { "st32" },
                reg_c(*rm, addr),
                rd
            ));
        }
        Kind::Mcr { load, coproc, rd, crn, crm, opc2, .. } => {
            // CP15 (p15) TCM configuration is modelled: melonDS keys its
            // register file on id = (crn<<8)|(crm<<4)|opc2 (ARMInterpreter
            // MCR/MRC), and the ITCM/DTCM routing in ld*/st* depends on it.
            // Other coprocessors keep the legacy 0 read / no-op write.
            if *coproc == 15 {
                let id = ((*crn as u32) << 8) | ((*crm as u32) << 4) | (*opc2 as u32);
                if *load {
                    body.push(format!("r[{}] = cp15_read(0x{:03X}u);", rd, id));
                } else {
                    body.push(format!("cp15_write(0x{:03X}u, r[{}]);", id, rd));
                }
            } else if *load {
                body.push(format!("r[{}] = cp15_reg;", rd));
            } else {
                body.push("/* mcr: no-op in this CP15 model */".into());
            }
        }
        Kind::Coproc | Kind::Hint => {
            body.push("/* hint / coprocessor: no guest-visible effect */".into());
        }
        Kind::Svc { imm } => {
            if global {
                // SWI/BIOS HLE: logged and executed by the runtime, which
                // returns to the next instruction (Flow::Next).  The SWI
                // number lives in bits 16..23 of an ARM comment field but in
                // bits 7..0 of a Thumb `svc #imm8` — exactly the two cases
                // FreeBIOS's `ldrb r12, [lr, #-2]` distinguishes, so the flag
                // is passed through instead of being re-derived.
                body.push(format!(
                    "if (!hle_swi(0x{:06X}u, 0x{:08X}u, {})) return;",
                    imm, addr, u8::from(thumb)
                ));
                flow = Flow::Next;
            } else {
                halts = true;
                body.push(format!("halt_reason(\"svc 0x{:X}\"); return;", imm));
                flow = Flow::Halt;
                pc_written = true;
            }
        }
        Kind::Undef => {
            halts = true;
            body.push(format!(
                "halt_reason(\"undefined instruction 0x{:08X}\"); return;",
                insn.words[0]
            ));
            flow = Flow::Halt;
            pc_written = true;
        }
        Kind::B { link: true, target, .. } => {
            // BL: in Thumb the link register gets bit0 set (the return
            // address is a Thumb address); ARM BL links a plain ARM address.
            let linkv = if thumb { addr.wrapping_add(4) | 1 } else { addr.wrapping_add(4) };
            if global {
                // real call: link the return address and dispatch to the
                // callee inside the shared PC-dispatch loop.
                flow = Flow::Indirect(None);
                pc_written = true;
                body.push(format!("r[14] = 0x{:08X}u;", linkv));
                body.push(format!("pc = 0x{:08X}u;", target & !1));
            } else {
                shimmed = true;
                flow = Flow::Call(*target, thumb);
                body.push(format!("r[14] = 0x{:08X}u;", linkv));
                body.push(format!("call_shim(0x{:08X}u, 0x{:08X}u);", target, addr));
                pc_written = true;
            }
        }
        Kind::BlxImm { target } => {
            // BLX (ARM imm form targets Thumb, the Thumb pair targets ARM):
            // the return address is the next instruction in THUMB state, so
            // the link always carries bit0.
            let linkv = addr.wrapping_add(4) | 1;
            if global {
                // Interworking call — dispatch to the callee's even address;
                // which case exists there (ARM or Thumb) selects the state.
                flow = Flow::Indirect(None);
                pc_written = true;
                body.push(format!("r[14] = 0x{:08X}u;", linkv));
                body.push(format!("pc = 0x{:08X}u;", target & !1));
            } else {
                shimmed = true;
                flow = Flow::Call(*target, true);
                body.push(format!("r[14] = 0x{:08X}u;", linkv));
                body.push(format!(
                    "call_shim(0x{:08X}u, 0x{:08X}u); /* blx: interworking call */",
                    target, addr
                ));
                pc_written = true;
            }
        }
        Kind::B { link: false, target, .. } => {
            flow = if guarded { Flow::BranchCond(*target) } else { Flow::Branch(*target) };
            pc_written = true;
        }
    }

    EmitOutcome { cycles: card_cost(&insn.kind), body, flow, guarded, cond: ce,
                  pc_written, shimmed, halts }
}

/// Emit C for one ARM-state instruction (condition code from bits 31..28).
fn emit_arm(w: u32, addr: u32, out: &mut Vec<String>, global: bool) -> EmitOutcome {
    let insn = decode_arm(w, addr);
    let c = cond_of(w);
    emit_insn(&insn, addr, false, c != 14, cond_expr(c).to_string(), out, global)
}

/// Emit C for one Thumb-state instruction (or a BL/BLX halfword pair).
/// Thumb has no per-instruction condition field: the only conditional form
/// is the format-16 branch, so `guarded` comes from its condition.
fn emit_thumb(h: u16, h2: u16, addr: u32, size: usize, out: &mut Vec<String>, global: bool) -> EmitOutcome {
    let insn = decode_thumb(h, h2, addr, size);
    let (guarded, ce) = match &insn.kind {
        Kind::B { cond, .. } if *cond != 14 => (true, cond_expr(*cond).to_string()),
        _ => (false, "1".to_string()),
    };
    emit_insn(&insn, addr, true, guarded, ce, out, global)
}

/// Emit C for the `Func.lines` entry at `addr` in the function's mode.
fn emit_line(mode: Mode, words: &[u32], addr: u32, out: &mut Vec<String>, global: bool) -> EmitOutcome {
    match mode {
        Mode::Arm => emit_arm(words[0], addr, out, global),
        Mode::Thumb => {
            let h = words[0] as u16;
            let h2 = (words[0] >> 16) as u16;
            let size = if words[0] > 0xFFFF { 4 } else { 2 };
            emit_thumb(h, h2, addr, size, out, global)
        }
    }
}

fn pad(stmts: &[String], p: &str) -> String {
    stmts.iter().map(|x| format!("{}{}\n", p, x)).collect()
}

/// Assemble one `case 0x...:` body of a PC-dispatch loop.  Shared by
/// `recompile-entry` (single-function loop) and `recompile-all` (one
/// image-wide loop): in global mode calls/returns arrive here as
/// `Flow::Indirect` with the body already assigning `pc`.
fn case_string(addr: u32, next: u32, text: &str, oc: &EmitOutcome) -> String {
    let mut s = String::new();
    s.push_str(&format!("        case 0x{:08X}u:\n", addr));
    s.push_str(&format!("            trace_pc(0x{:08X}u);\n", addr));
    s.push_str(&format!("            card_tick({}u);\n", oc.cycles));
    s.push_str(&format!("            /* 0x{:08X}: {} */\n", addr, text.replace("*/", "* /")));
    match oc.flow {
        Flow::Branch(t) => {
            s.push_str(&format!("            pc = 0x{:08X}u; break;\n", t));
        }
        Flow::BranchCond(t) => {
            s.push_str(&format!("            if ({}) {{ pc = 0x{:08X}u; break; }}\n", oc.cond, t));
            s.push_str(&format!("            pc = 0x{:08X}u; break;\n", next));
        }
        Flow::Call(_, _) => {
            if oc.guarded {
                s.push_str(&format!("            if ({}) {{\n", oc.cond));
                s.push_str(&pad(&oc.body, "                "));
                s.push_str("            }\n");
            } else {
                s.push_str(&pad(&oc.body, "            "));
            }
            s.push_str(&format!("            pc = 0x{:08X}u; break;\n", next));
        }
        Flow::Return => {
            if oc.guarded {
                s.push_str(&format!("            if ({}) {{ pc = r[14]; break; }}\n", oc.cond));
                s.push_str(&format!("            pc = 0x{:08X}u; break;\n", next));
            } else {
                s.push_str(&pad(&oc.body, "            "));
                s.push_str("            return;\n");
            }
        }
        Flow::Indirect(_) => {
            if oc.guarded {
                s.push_str(&format!("            if ({}) {{\n", oc.cond));
                s.push_str(&pad(&oc.body, "                "));
                s.push_str("                break;\n");
                s.push_str("            }\n");
                s.push_str(&format!("            pc = 0x{:08X}u; break;\n", next));
            } else {
                s.push_str(&pad(&oc.body, "            "));
                s.push_str("            break;\n");
            }
        }
        Flow::Halt => {
            s.push_str(&pad(&oc.body, "            "));
            if !oc.pc_written {
                s.push_str("            return;\n");
            }
        }
        Flow::Next => {
            if oc.guarded {
                s.push_str(&format!("            if ({}) {{\n", oc.cond));
                s.push_str(&pad(&oc.body, "                "));
                s.push_str("            }\n");
            } else {
                s.push_str(&pad(&oc.body, "            "));
            }
            s.push_str(&format!("            pc = 0x{:08X}u; break;\n", next));
        }
    }
    s
}

const RUNTIME: &str = r#"/* Generated by nnktool recompile-entry — Ni no Kuni DS (B2KJ) ARM9. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- guest state: flat memory window, registers, flags ---- */
#define GBASE 0x02000000u
#define GSIZE 0x01000000u
static uint8_t gmem[GSIZE];
static uint32_t r[16];
static uint32_t n, z, c, v;      /* CPSR NZCV (single-bank model) */
static uint32_t cpsr_ctl = 0x13; /* control byte: mode/IRQ bits */
static uint32_t cp15_reg = 0;    /* CP15 reads return 0 in this model */
static uint32_t carry_out;       /* shifter carry-out of the last sh_op */
static uint64_t steps;
/* ARM9 mode banking — same model as the image-wide runtime below (see the
 * long comment there); the entry-function translation needs it because the
 * crt0 switches modes with msr CPSR_c. */
#define MODE_USR 0
#define MODE_FIQ 1
#define MODE_IRQ 2
#define MODE_SVC 3
#define MODE_ABT 4
#define MODE_UND 5
#define MODE_N   6
static uint32_t bank13[MODE_N], bank14[MODE_N], spsr_bank[MODE_N];
static int mode_cur = MODE_SVC;
static int mode_ix(uint32_t ctl) {
    switch (ctl & 0x1Fu) {
    case 0x11: return MODE_FIQ;
    case 0x12: return MODE_IRQ;
    case 0x13: return MODE_SVC;
    case 0x17: return MODE_ABT;
    case 0x1B: return MODE_UND;
    default:   return MODE_USR;
    }
}
/* The `ldm/stm ...^` user-bank form: r13/r14 of the USER bank.  Emitted case
 * bodies call these (they never see the MODE_* macros: the split build keeps
 * the case bodies in separate TUs). */
static uint32_t bank13_get_usr(void) { return mode_cur == MODE_USR ? r[13] : bank13[MODE_USR]; }
static uint32_t bank14_get_usr(void) { return mode_cur == MODE_USR ? r[14] : bank14[MODE_USR]; }
static void bank13_set_usr(uint32_t v) { if (mode_cur == MODE_USR) r[13] = v; else bank13[MODE_USR] = v; }
static void bank14_set_usr(uint32_t v) { if (mode_cur == MODE_USR) r[14] = v; else bank14[MODE_USR] = v; }
/* Is the current mode privileged?  (Gate for the exception-return idiom.) */
static int mode_priv(void) { return mode_cur != MODE_USR; }

static uint32_t bank13_get(int b) { return b == mode_cur ? r[13] : bank13[b]; }
static uint32_t bank14_get(int b) { return b == mode_cur ? r[14] : bank14[b]; }
static void bank13_set(int b, uint32_t v) { if (b == mode_cur) r[13] = v; else bank13[b] = v; }
static void bank14_set(int b, uint32_t v) { if (b == mode_cur) r[14] = v; else bank14[b] = v; }
static void cpsr_set_ctl(uint32_t ctl) {
    int nw = mode_ix(ctl);
    if (nw != mode_cur) {
        bank13[mode_cur] = r[13]; bank14[mode_cur] = r[14];
        r[13] = bank13[nw]; r[14] = bank14[nw];
        mode_cur = nw;
    }
    cpsr_ctl = ctl & 0xFFu;
}
static uint64_t shimmed_calls;
static uint64_t traced;
static uint64_t traced_exec;

#ifndef TRACE_LIMIT
#define TRACE_LIMIT 4096ull
#endif
#ifndef STEP_LIMIT
#define STEP_LIMIT 500000000ull
#endif

static const char *halt_msg = NULL;

/* Guest PCs already dispatched, for the first-visit trace (default mode). */
static uint8_t seen[(IMAGE_LEN / 4u + 7u) / 8u];

static inline uint32_t ld32(uint32_t a) {
    if (a < GBASE || (uint64_t)a + 4u > (uint64_t)GBASE + GSIZE) return 0;
    const uint8_t *p = gmem + (a - GBASE);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void st32(uint32_t a, uint32_t v) {
    if (a < GBASE || (uint64_t)a + 4u > (uint64_t)GBASE + GSIZE) return;
    uint8_t *p = gmem + (a - GBASE);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t ld16(uint32_t a) {
    if (a < GBASE || (uint64_t)a + 2u > (uint64_t)GBASE + GSIZE) return 0;
    const uint8_t *p = gmem + (a - GBASE);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline void st16(uint32_t a, uint16_t v) {
    if (a < GBASE || (uint64_t)a + 2u > (uint64_t)GBASE + GSIZE) return;
    uint8_t *p = gmem + (a - GBASE);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline uint32_t ld8(uint32_t a) {
    if (a < GBASE || (uint64_t)a + 1u > (uint64_t)GBASE + GSIZE) return 0;
    return gmem[a - GBASE];
}
static inline void st8(uint32_t a, uint32_t v) {
    if (a < GBASE || (uint64_t)a + 1u > (uint64_t)GBASE + GSIZE) return;
    gmem[a - GBASE] = (uint8_t)v;
}

/* Barrel shifter: value, kind (0 lsl / 1 lsr / 2 asr / 3 ror|rrx), amount, carry-in.
 * Carry-out is published in `carry_out`. */
static uint32_t sh_op(uint32_t v, unsigned kind, unsigned amt, uint32_t carry) {
    switch (kind) {
    case 0: /* LSL */
        if (amt == 0) { carry_out = carry; return v; }
        if (amt < 32) { carry_out = (v >> (32 - amt)) & 1u; return v << amt; }
        if (amt == 32) { carry_out = v & 1u; return 0; }
        carry_out = 0; return 0;
    case 1: /* LSR */
        if (amt == 0) { carry_out = (v >> 31) & 1u; return 0; }              /* LSR #32 */
        if (amt < 32) { carry_out = (v >> (amt - 1)) & 1u; return v >> amt; }
        carry_out = (amt == 32) ? ((v >> 31) & 1u) : 0u;
        return 0;
    case 2: /* ASR */
        if (amt == 0) amt = 32;
        if (amt >= 32) { carry_out = (v >> 31) & 1u; return (v >> 31) ? 0xFFFFFFFFu : 0u; }
        carry_out = (v >> (amt - 1)) & 1u;
        return (uint32_t)(((int32_t)v) >> amt);
    default: /* ROR / RRX */
        if (amt == 0) { uint32_t res = (carry << 31) | (v >> 1); carry_out = v & 1u; return res; }
        amt &= 31;
        if (amt == 0) { carry_out = (v >> 31) & 1u; return v; }
        carry_out = (v >> (amt - 1)) & 1u;
        return (v >> amt) | (v << (32 - amt));
    }
}

static inline uint32_t clz32(uint32_t x) {
    uint32_t k = 0;
    if (x == 0) return 32;
    while ((x & 0x80000000u) == 0) { x <<= 1; k++; }
    return k;
}

/* Guest-PC trace.
 *  default          : prints each guest PC the FIRST time the dispatch loop
 *                     reaches it (executed-code coverage order).
 *  -DNNK_TRACE_EXEC : prints every executed instruction (loops repeat).
 * Both print one "0x%08x" line per guest PC. */
static void trace_pc(uint32_t pc) {
#ifdef NNK_TRACE_EXEC
    if (traced_exec < TRACE_LIMIT) printf("0x%08x\n", pc);
    traced_exec++;
#else
    if (pc >= IMAGE_LOAD) {
        uint32_t idx = (pc - IMAGE_LOAD) >> 2;
        if (idx < IMAGE_LEN / 4u && (seen[idx >> 3] & (1u << (idx & 7))) == 0) {
            seen[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            if (traced < TRACE_LIMIT) printf("0x%08x\n", pc);
            traced++;
        }
    }
#endif
}

/* Direct calls from the entry function are recorded, not executed: the
 * Phase-1A translation covers the entry function only. */
static void call_shim(uint32_t target, uint32_t from) {
    shimmed_calls++;
    fprintf(stderr, "call-shim 0x%08X from 0x%08X (callee not executed in Phase-1A)\n", target, from);
}

static void halt_reason(const char *msg) { halt_msg = msg; }

static void guest_init(void) { memset(r, 0, sizeof r); n = z = c = v = 0; }

"#;

/// Translate the entry function (ARM state) into host C.
pub fn recompile_entry(
    image: &[u8],
    base: u32,
    entry: u32,
    out_c: &Path,
    limit: usize,
) -> Result<RecStats, String> {
    if entry & 1 != 0 {
        return Err("entry has Thumb bit set; only ARM-state entry translation is implemented".into());
    }
    let text_end = base.wrapping_add(image.len() as u32);
    let key = FuncKey { addr: entry, mode: Mode::Arm };
    let func = trace_function(image, base, text_end, key, limit);
    if func.lines.is_empty() {
        return Err(format!("no instructions recovered at entry 0x{entry:08X}"));
    }

    let mut cases: Vec<String> = Vec::new();
    let mut stats = RecStats { insns: 0, calls_shimmed: 0, indirects: 0, halts: 0, bytes: 0 };
    for (&addr, (words, text)) in &func.lines {
        let w = words[0];
        let size = (words.len() * 4) as u32;
        let next = addr.wrapping_add(size);
        let mut scratch: Vec<String> = Vec::new();
        let oc = emit_arm(w, addr, &mut scratch, false);
        stats.insns += 1;
        if oc.shimmed {
            stats.calls_shimmed += 1;
        }
        if oc.halts {
            stats.halts += 1;
        }
        if matches!(oc.flow, Flow::Indirect(_)) {
            stats.indirects += 1;
        }
        let indented = |stmts: &[String], pad: &str| -> String {
            stmts.iter().map(|x| format!("{}{}\n", pad, x)).collect::<String>()
        };
        let _ = &indented;
        cases.push(case_string(addr, next, text, &oc));
    }

    let mut src = String::new();
    src.push_str(&format!(
        "#define IMAGE_LEN {}u\n#define ENTRY_CONST 0x{:08X}u\n#define IMAGE_LOAD 0x{:08X}u\n",
        image.len(),
        entry,
        base
    ));
    src.push_str(RUNTIME);
    src.push_str(&format!(
        "static void fn_{:08X}(void) {{\n    uint32_t pc = ENTRY_CONST;\n    for (;;) {{\n        if (++steps > STEP_LIMIT) {{ halt_reason(\"step budget exhausted\"); return; }}\n        switch (pc) {{\n",
        entry
    ));
    for case in &cases {
        src.push_str(case);
    }
    src.push_str(
        "        default:\n            printf(\"0x%08x\\n\", pc);\n            halt_reason(\"dispatch to untranslated pc\");\n            return;\n        }\n    }\n}\n\n",
    );
    src.push_str(
        "int main(int argc, char **argv) {\n    const char *path = argc > 1 ? argv[1] : \"arm9_image.bin\";\n    FILE *fp = fopen(path, \"rb\");\n    if (!fp) { fprintf(stderr, \"cannot open guest image %s\\n\", path); return 2; }\n    size_t got = fread(gmem, 1, IMAGE_LEN, fp);\n    fclose(fp);\n    if (got != IMAGE_LEN) { fprintf(stderr, \"short read: got %zu, want %u\\n\", got, IMAGE_LEN); return 2; }\n    guest_init();\n    printf(\"# nnk recompiled ARM9 entry 0x%08X\\n\", ENTRY_CONST);\n    printf(\"# guest image %s: %zu bytes at 0x%08X\\n\", path, got, IMAGE_LOAD);\n    fn_ENTRY_NAME();\n    printf(\"# done steps=%llu traced_first_visit=%llu traced_exec=%llu calls_shimmed=%llu halt=%s\\n\",\n        (unsigned long long)steps, (unsigned long long)traced, (unsigned long long)traced_exec,\n        (unsigned long long)shimmed_calls, halt_msg ? halt_msg : \"none\");\n    return 0;\n}\n"
    );
    src = src.replace("fn_ENTRY_NAME", &format!("fn_{:08X}", entry));

    if let Some(parent) = out_c.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| format!("mkdir {}: {}", parent.display(), e))?;
    }
    fs::write(out_c, &src).map_err(|e| format!("write {}: {}", out_c.display(), e))?;
    stats.bytes = src.len();
    Ok(stats)
}

// ---------------------------------------------------------- recompile-all ---

/// Per-case bookkeeping for `recompile-all` statistics.
struct CaseMeta {
    swi: Option<u32>,
    call: bool,
    indirect: bool,
    ret: bool,
}

#[derive(Default)]
pub struct AllStats {
    pub seeds: usize,
    pub funcs: usize,
    pub funcs_arm: usize,
    pub funcs_thumb: usize,
    pub thumb_insns: usize,
    pub insns: usize,
    pub calls: usize,
    pub returns: usize,
    pub indirects: usize,
    pub jump_tables: usize,
    pub swi_sites: usize,
    /// (imm24, address) of every SWI site, address order.
    pub swis: Vec<(u32, u32)>,
    pub dup_cases: usize,
    pub unresolved_sites: usize,
    pub bytes: usize,
}

/// Translate the whole recovered set into ONE PC-dispatch image: a single C
/// function with one `switch (pc)` covering every recovered ARM AND Thumb
/// instruction, real direct calls/returns (link register + pc update),
/// runtime dispatch for register-indirect control flow, ARM<->Thumb
/// interworking (the switch key is the even address; the state is implied by
/// which case exists there), and SWI/BIOS HLE that returns to the next
/// instruction.  Functions are inserted ARM-first, so an address recovered in
/// both states keeps its ARM case (a Thumb case never shadows ARM code).
pub fn recompile_all(
    image: &[u8],
    base: u32,
    seeds: &[FuncKey],
    entry: u32,
    out_c: &Path,
    limit_per_fn: usize,
    max_funcs: usize,
) -> Result<AllStats, String> {
    set_image(image, base);
    let text_end = base.wrapping_add(image.len() as u32);
    let rec = recover_seeds(image, base, text_end, seeds, limit_per_fn, max_funcs);

    let mut stats = AllStats { seeds: seeds.len(), ..Default::default() };
    stats.funcs = rec.funcs.len();
    for (k, f) in &rec.funcs {
        stats.jump_tables += f.jump_tables;
        stats.unresolved_sites += f
            .indirects
            .iter()
            .filter(|(_, d)| d.starts_with("indirect branch (unresolved)"))
            .count();
        if k.mode == Mode::Thumb {
            stats.funcs_thumb += 1;
        } else {
            stats.funcs_arm += 1;
        }
    }

    let mut cases: BTreeMap<u32, (String, CaseMeta)> = BTreeMap::new();
    // Two passes, ARM first: an address recovered in both states keeps its
    // ARM case (a Thumb body must never shadow ARM code at the same address).
    for mode_pass in [Mode::Arm, Mode::Thumb] {
    for (k, f) in &rec.funcs {
        if k.mode != mode_pass {
            continue;
        }
        for (&addr, (words, text)) in &f.lines {
            if cases.contains_key(&addr) {
                stats.dup_cases += 1;
                continue;
            }
            let thumb = k.mode == Mode::Thumb;
            let w = words[0];
            let size = if thumb { if w > 0xFFFF { 4u32 } else { 2 } } else { (words.len() * 4) as u32 };
            let next = addr.wrapping_add(size);
            let mut scratch: Vec<String> = Vec::new();
            let oc = emit_line(k.mode, words, addr, &mut scratch, true);
            let insn = if thumb {
                decode_thumb(w as u16, (w >> 16) as u16, addr, size as usize)
            } else {
                decode_arm(w, addr)
            };
            let meta = match &insn.kind {
                Kind::Svc { imm } => CaseMeta { swi: Some(*imm), call: false, indirect: false, ret: false },
                Kind::B { link: true, .. } | Kind::BlxImm { .. } => {
                    CaseMeta { swi: None, call: true, indirect: false, ret: false }
                }
                Kind::Bx { rm: 14, link: false } => {
                    CaseMeta { swi: None, call: false, indirect: false, ret: true }
                }
                Kind::Bx { .. } | Kind::Mem { load: true, rd: 15, .. } => {
                    CaseMeta { swi: None, call: false, indirect: true, ret: false }
                }
                // test ops write no register: rd == 15 has no effect there
                Kind::Dp { op: 8..=11, .. } => {
                    CaseMeta { swi: None, call: false, indirect: false, ret: false }
                }
                Kind::Dp { rd: 15, .. } => {
                    CaseMeta { swi: None, call: false, indirect: true, ret: false }
                }
                Kind::LdmStm { load: true, list, .. } if list & 0x8000 != 0 => {
                    CaseMeta { swi: None, call: false, indirect: true, ret: false }
                }
                _ => CaseMeta { swi: None, call: false, indirect: false, ret: false },
            };
            if thumb {
                stats.thumb_insns += 1;
            }
            cases.insert(addr, (case_string(addr, next, text, &oc), meta));
        }
    }
    }

    for (_, meta) in cases.values() {
        stats.insns += 1;
        if meta.swi.is_some() {
            stats.swi_sites += 1;
        }
        if meta.call {
            stats.calls += 1;
        }
        if meta.ret {
            stats.returns += 1;
        }
        if meta.indirect {
            stats.indirects += 1;
        }
    }
    stats.swis = cases
        .iter()
        .filter_map(|(&a, (_, m))| m.swi.map(|i| (i, a)))
        .collect();

    let mut src = String::new();
    src.push_str(&format!(
        "/* Generated by nnktool recompile-all — Ni no Kuni DS (B2KJ) ARM9.\n\
         * One program-wide PC-dispatch image over the recovered set\n\
         * ({} ARM instructions, {} cases after de-duplication). */\n\
         #define IMAGE_LEN {}u\n#define ENTRY_CONST 0x{:08X}u\n#define IMAGE_LOAD 0x{:08X}u\n",
        stats.insns,
        cases.len(),
        image.len(),
        entry,
        base
    ));
    src.push_str(RUNTIME_ALL);
    src.push_str(
        "static void run(void) {\n    uint32_t pc = ENTRY_CONST;\n    for (;;) {\n        if (++steps > step_limit) { halt_reason(\"step budget exhausted\"); return; }\n        arm7_evt_tick();\n        irq_tick();\n        if (irq_active && pc >= BIOS_IRQ_LO) { bios_irq_return(&pc); }\n        else if (!irq_active\n                 && (irq_trig_n >= IRQ_TRIG_N || steps > irq_trig_log[irq_trig_n])\n                 && ime && (ie & iflags) && !(cpsr_ctl & 0x80u)) {\n            irq_trig_n++;\n            irq_enter(&pc);\n        }\n        switch (pc) {\n",
    );
    for (case, _) in cases.values() {
        src.push_str(case);
    }
    src.push_str(
        "        default:\n            fprintf(stderr, \"untranslated pc=0x%08x\\n\", pc);\n            halt_reason(\"dispatch to untranslated pc\");\n            return;\n        }\n    }\n}\n\n",
    );
    src.push_str(RUNTIME_ALL_MAIN);

    if let Some(parent) = out_c.parent().filter(|p| !p.as_os_str().is_empty()) {
        fs::create_dir_all(parent).map_err(|e| format!("mkdir {}: {}", parent.display(), e))?;
    }
    fs::write(out_c, &src).map_err(|e| format!("write {}: {}", out_c.display(), e))?;
    stats.bytes = src.len();
    Ok(stats)
}

/// Runtime for the image-wide dispatch loop: guest state, flat memory window,
/// per-execution PC trace, and SWI/BIOS HLE.  Emitted before `run()`.
const RUNTIME_ALL: &str = r##"#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Guest state.
 * Flat window 0x02000000..0x03800000 (24 MiB): main RAM (4 MiB), the gap
 * above it and the shared WRAM at 0x03000000 — exactly the regions melonDS
 * zeroes in NDS::Reset (memset MainRAM / SharedWRAM / ARM7WRAM to 0).
 * I/O space 0x04000000+ is OUTSIDE the window: reads return 0 and writes are
 * dropped (documented Phase-2A simplification). */
#define GBASE 0x02000000u
#define GSIZE 0x01800000u
static uint8_t gmem[GSIZE];
/* Main-RAM mirror: melonDS maps ALL of 0x02000000..0x02FFFFFF onto the 4 MiB
 * of main RAM with `addr & MainRAMMask` (NDS.cpp ARM9Read32/Write32, `case
 * 0x02000000`), so e.g. 0x02FFFF8C IS main-RAM 0x3FFF8C.  Scripted ARM7-side
 * writes must go through the same alias the guest reads from, or the two
 * views diverge (measured: the 0x02008E1C wait(12,1) loop never saw the ARM7
 * ready flag once the guest read the mirrored address).  Guest ld/st go
 * through win_off() below, which applies exactly this mask. */
static inline uint8_t *main_ram_ptr(uint32_t a) { return gmem + (a & 0x3FFFFFu); }
static uint32_t r[16];
static uint32_t n, z, c, v;      /* CPSR NZCV (flags of the CURRENT mode) */
static uint32_t cpsr_ctl = 0x13; /* control byte: mode/IRQ bits */
static uint32_t cp15_reg = 0;    /* CP15 reads return 0 in this model */
static uint32_t carry_out;       /* shifter carry-out of the last sh_op */
static uint64_t steps;
static uint64_t traced;
static uint64_t trace_from;      /* NNK_TRACE_FROM: only print from this step */
static uint64_t distinct_pcs;

/* ---- ARM9 mode banking (Phase-4C) -----------------------------------------
 * r[] holds the registers of the CURRENT mode; bank13/bank14 keep the shadow
 * copy of every other mode and are refreshed on each switch (the ARM has one
 * r13/r14 pair per mode; USR and SYS share theirs).  Every exception mode
 * also owns an SPSR.  The boot's thread-context switch is the first guest code
 * that depends on this and it pins the semantics:
 *   0x0200081C  msr CPSR_c, #0x13   crt0 selects SVC      (no-op here: 0x13)
 *   0x02000830  msr CPSR_c, #0x12   -> IRQ bank, then 0x02000834 computes the
 *                                   IRQ stack from a constant (0x027E3F7C)
 *   0x02000858  msr CPSR_fsxc,#0x1F -> SYS bank, stack from r1 (0x027E3B7C)
 *   0x02007238  msr CPSR_c, #0xD3   SaveContext: SVC bank, saves the SVC sp
 *   0x02007288  msr SPSR_fsxc, r1   writes the CURRENT mode's SPSR
 *   0x02007294  ldmia r0, {..}^     r13/r14 of the USER bank, not of the
 *                                   current (SVC) bank
 *   0x0200729C  subs pc, lr, #4      exception return: PC = lr-4 AND
 *                                   CPSR <- SPSR, i.e. back to SYS mode
 * A single-bank model reaches 0x0200729C with the USER-bank lr already in
 * r[14] (0x02006AF0) and branches to 0x02006AEC; melonDS branches to
 * 0x02015EC4.  Measured at 0x0200729C (tools/add_regprobe.py): CPSR
 * 0x600000D3 (SVC), r13=0x0206EF00 r14=0x02015EC8, SPSR 0x0000001F -> SYS
 * mode with r13=0x0206EEC0 r14=0x02006AF0 at 0x02015EC4. */
#define MODE_USR 0
#define MODE_FIQ 1
#define MODE_IRQ 2
#define MODE_SVC 3
#define MODE_ABT 4
#define MODE_UND 5
#define MODE_N   6
/* Sized with a literal 6: tools/split_dispatch.py externalises every runtime
 * array the case bodies touch into nnk_rt.h, which has no MODE_N. */
static uint32_t bank13[6], bank14[6], spsr_bank[6];
static int mode_cur = MODE_SVC;  /* the 0x13 reset value above */
static int mode_trace;           /* NNK_MODE_TRACE=1: log every switch */

static int mode_ix(uint32_t ctl) {
    switch (ctl & 0x1Fu) {
    case 0x11: return MODE_FIQ;
    case 0x12: return MODE_IRQ;
    case 0x13: return MODE_SVC;
    case 0x17: return MODE_ABT;
    case 0x1B: return MODE_UND;
    default:   return MODE_USR;  /* 0x10 USR and 0x1F SYS share one bank */
    }
}
/* Banked r13/r14 access, used by the `ldm/stm ...^` user-bank form. */
/* The `ldm/stm ...^` user-bank form: r13/r14 of the USER bank.  Emitted case
 * bodies call these (they never see the MODE_* macros: the split build keeps
 * the case bodies in separate TUs). */
static uint32_t bank13_get_usr(void) { return mode_cur == MODE_USR ? r[13] : bank13[MODE_USR]; }
static uint32_t bank14_get_usr(void) { return mode_cur == MODE_USR ? r[14] : bank14[MODE_USR]; }
static void bank13_set_usr(uint32_t v) { if (mode_cur == MODE_USR) r[13] = v; else bank13[MODE_USR] = v; }
static void bank14_set_usr(uint32_t v) { if (mode_cur == MODE_USR) r[14] = v; else bank14[MODE_USR] = v; }
/* Is the current mode privileged?  (Gate for the exception-return idiom.) */
static int mode_priv(void) { return mode_cur != MODE_USR; }

static uint32_t bank13_get(int b) { return b == mode_cur ? r[13] : bank13[b]; }
static uint32_t bank14_get(int b) { return b == mode_cur ? r[14] : bank14[b]; }
static void bank13_set(int b, uint32_t v) { if (b == mode_cur) r[13] = v; else bank13[b] = v; }
static void bank14_set(int b, uint32_t v) { if (b == mode_cur) r[14] = v; else bank14[b] = v; }

/* A write to CPSR control: switch banks when the mode changes. */
static void cpsr_set_ctl(uint32_t ctl) {
    int nw = mode_ix(ctl);
    if (nw != mode_cur) {
        bank13[mode_cur] = r[13]; bank14[mode_cur] = r[14];
        r[13] = bank13[nw]; r[14] = bank14[nw];
        if (mode_trace)
            fprintf(stderr, "mode %d->%d step=%llu sp=0x%08X lr=0x%08X\n",
                    mode_cur, nw, (unsigned long long)steps, r[13], r[14]);
        mode_cur = nw;
    }
    cpsr_ctl = ctl & 0xFFu;
}

static uint64_t hle_hits;
static uint32_t last_pc;

/* ---- CP15 TCM model (Phase-4B, melonDS CP15.cpp semantics) ---------------
 * ITCM: control bit18 + size register 0x911 -> ITCMSize = 0x200 <<
 * ((setting>>1)&0x1F).  melonDS routes EVERY access with addr < ITCMSize to
 * ITCM[addr & 0x7FFF] — one check covering both the 0x00000000 mirror and
 * the 0x01FF8000..0x01FFFFFF window; the measured BIOS size word 0x20 gives
 * ITCMSize = 0x02000000 exactly, so the window maps 1:1 while main RAM
 * (0x02000000+) stays outside it.  DTCM: control bit16 + base register
 * 0x910; size = max(0x200 << ((setting>>1)&0x1F), 0x1000); an access hits
 * DTCM when (addr & mask) == base (the disabled base 0xFFFFFFFF can never
 * match, so the same expression works with TCMs off).
 *
 * The native run starts at the ARM9 entry point 0x02000800, SKIPPING the
 * ARM9 BIOS, so the BIOS's post-init CP15 configuration is the INITIAL state
 * here — measured with the MELONDS_CP15TRACE hook (tools/add_cp15trace.py):
 *   control 0x00052078 -> stored masked 0x00052000 (ITCM+DTCM on)
 *   0x911 = 0x00000020 -> ITCMSize 0x02000000
 *   0x910 = 0x0300000A -> DTCM base 0x03000000 size 16 KiB
 * The boot stub then re-programs both TCMs at 0x02000A84..0x02000B28
 * through cp15_write() below, exactly as melonDS sees them.  Cache/TLB/PU
 * CP15 writes stay no-ops (this runtime models neither). */
#define ITCM_LO 0x01FF8000u
#define ITCM_HI 0x02000000u
#define ITCM_PHYS 0x8000u
#define DTCM_PHYS 0x4000u
static uint8_t itcm[ITCM_PHYS];
static uint8_t dtcm[DTCM_PHYS];
static uint32_t cp15_control = 0x00052000u;
static uint32_t cp15_dtcm_setting = 0x0300000Au;
static uint32_t cp15_itcm_setting = 0x00000020u;
static uint32_t itcm_size = 0;
static uint32_t dtcm_base = 0xFFFFFFFFu;
static uint32_t dtcm_mask = 0;
static void cp15_update_tcm(void) {
    if (cp15_control & (1u << 18))
        itcm_size = 0x200u << ((cp15_itcm_setting >> 1) & 0x1Fu);
    else
        itcm_size = 0;
    if (cp15_control & (1u << 16)) {
        uint32_t s = 0x200u << ((cp15_dtcm_setting >> 1) & 0x1Fu);
        if (s < 0x1000u) s = 0x1000u;
        dtcm_mask = 0xFFFFF000u & ~(s - 1u);
        dtcm_base = cp15_dtcm_setting & dtcm_mask;
    } else {
        dtcm_base = 0xFFFFFFFFu;
        dtcm_mask = 0;
    }
}
static uint32_t cp15_read(uint32_t id) {
    switch (id) {
    case 0x100: return cp15_control;
    case 0x910: return cp15_dtcm_setting;
    case 0x911: return cp15_itcm_setting;
    default: return 0;
    }
}
static void cp15_write(uint32_t id, uint32_t val) {
    switch (id) {
    case 0x100:
        cp15_control = (cp15_control & ~0x000FF085u) | (val & 0x000FF085u);
        cp15_update_tcm();
        fprintf(stderr, "cp15 W id=0x%03X val=0x%08X pc=0x%08X -> control=0x%08X itcm_size=0x%08X dtcm_base=0x%08X dtcm_mask=0x%08X\n",
                id, val, last_pc, cp15_control, itcm_size, dtcm_base, dtcm_mask);
        return;
    case 0x910:
        cp15_dtcm_setting = val & 0xFFFFF03Eu;
        cp15_update_tcm();
        fprintf(stderr, "cp15 W id=0x%03X val=0x%08X pc=0x%08X -> dtcm_base=0x%08X dtcm_mask=0x%08X\n",
                id, val, last_pc, dtcm_base, dtcm_mask);
        return;
    case 0x911:
        cp15_itcm_setting = val & 0x0000003Eu;
        cp15_update_tcm();
        fprintf(stderr, "cp15 W id=0x%03X val=0x%08X pc=0x%08X -> itcm_size=0x%08X\n",
                id, val, last_pc, itcm_size);
        return;
    default:
        /* cache/TLB/PU CP15 writes: no guest-visible effect in this model. */
        return;
    }
}

#ifndef TRACE_LIMIT
#define TRACE_LIMIT 4000000ull
#endif
#ifndef STEP_LIMIT
#define STEP_LIMIT 200000000ull
#endif
/* Runtime override for probing the boot timeline without recompiling:
 * NNK_STEP_LIMIT=<n> replaces the compile-time budget for this run. */
static uint64_t step_limit = STEP_LIMIT;

static const char *halt_msg = NULL;
/* Distinct-PC bitmap, one bit per HALFWORD: Thumb code lives at 2-byte
 * addresses, so a word-granular bitmap would merge two Thumb instructions
 * into one "distinct PC" (and split none for ARM, whose PCs stay 4-aligned). */
static uint8_t seen[(IMAGE_LEN / 2u + 7u) / 8u];

/* ---- IO engine (Phase-3A2) ---------------------------------------------
 * Guest loads/stores in 0x04000000..0x04100014 route here for real register
 * semantics derived from melonDS (NDS.cpp ARM9IOWrite32/ARM9IORead32 and
 * NDSCart.cpp).  An ARM7 responder stub completes the IPCSync countdown
 * handshake (7->0) and the single observed IPCFIFO request/answer pair; a
 * card server streams real ROM bytes for command 0xB7 (NNK_ROM_PATH).
 * Timing is a coarse approximation (1 guest instruction ~= 1 cycle, 2130
 * cycles/scanline) so VCOUNT advances and vblank polls terminate; poll-loop
 * ITERATION COUNTS differ from melonDS — an honest, documented divergence.
 * Unmapped IO reads return 0 and writes are dropped (logged once). */
#define IO_LO 0x04000000u
#define IO_HI 0x04100014u
#define CYCLES_PER_SCANLINE 2130u
#define SCANLINES 263u
static uint64_t cycles;
static uint32_t vcount(void) { return (uint32_t)((cycles / CYCLES_PER_SCANLINE) % SCANLINES); }

/* ---- live display bridge (Phase-4E) --------------------------------------
 * gx_display (gxrender) owns the 2D/3D graphics and keypad/touch state; the
 * runtime forwards exactly the MMIO + palette/VRAM/OAM traffic it owns and
 * renders one frame per emulated VBlank (vcount crossing into 192..262).
 * Compiled in only with -DNNK_GX (tools/build_split.sh NNK_GX=1), so the
 * default build keeps its no-gxrender dependency and every pre-existing
 * acceptance check is unaffected.
 *
 *   NNK_GX=window|headless   create the gxd instance (SDL2 window / headless)
 *   NNK_GX_SHOT=<base>       dump <base>.<frame>.top.ppm / .bot.ppm
 *   NNK_GX_SHOT_EVERY=<n>    dump every n-th frame (default 1)
 */
#ifdef NNK_GX
#include "gx_display.h"
static Gxd *gxd;
static uint32_t gxd_last_vc = 0xFFFFFFFFu;
static unsigned gxd_frames;
static const char *gxd_shot;
static unsigned gxd_shot_every = 1;

static int nnk_display_open(void) {
    const char *mode = getenv("NNK_GX");
    GxdConfig cfg;
    if (!mode || !mode[0]) return 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.windowed = (strcmp(mode, "window") == 0);
    cfg.gl_3d = 0;                 /* software 3D: the byte-exact path */
    cfg.scale = 2;
    cfg.layout = GXD_LAYOUT_SIDE;
    cfg.title = "Ni no Kuni DS";
    gxd = gxd_open(&cfg);
    if (!gxd) { fprintf(stderr, "gx: gxd_open failed (mode=%s)\\n", mode); return 0; }
    { const char *s = getenv("NNK_GX_SHOT");
      if (s && s[0]) gxd_shot = s;
      s = getenv("NNK_GX_SHOT_EVERY");
      if (s && s[0]) gxd_shot_every = (unsigned)strtoul(s, NULL, 0); }
    fprintf(stderr, "gx: display open mode=%s windowed=%d shot=%s\\n",
            mode, cfg.windowed, gxd_shot ? gxd_shot : "(none)");
    return 1;
}
static void nnk_display_shot(void) {
    char p[512];
    if (!gxd_shot || !gxd) return;
    if (gxd_shot_every != 0u && (gxd_frames % gxd_shot_every) != 0u) return;
    snprintf(p, sizeof p, "%s.%05u.top.ppm", gxd_shot, gxd_frames);
    gxd_write_screen_ppm(gxd, 0, p);
    snprintf(p, sizeof p, "%s.%05u.bot.ppm", gxd_shot, gxd_frames);
    gxd_write_screen_ppm(gxd, 1, p);
}
/* Window capture (windowed mode only): proves the SDL2/GL present path, i.e.
 * the pixels the user actually sees in the window, not just the compositor's
 * memory image.  NNK_GX_WINDOW_PPM=<path> overwrites it at every dump. */
static void nnk_display_window_shot(void) {
    const char *w = getenv("NNK_GX_WINDOW_PPM");
    if (!w || !w[0] || !gxd) return;
    if (gxd_shot_every != 0u && (gxd_frames % gxd_shot_every) != 0u) return;
    gxd_write_window_ppm(gxd, w);
}
/* Once per dispatched instruction: detect the VBlank edge, render+present.
 * Returns nonzero when the user closed the window. */
static int nnk_display_tick(void) {
    uint32_t vc;
    if (!gxd) return 0;
    vc = vcount();
    if (gxd_last_vc < 192u && vc >= 192u) {
        gxd_frame(gxd);
        gxd_frames++;
        nnk_display_shot();
        nnk_display_window_shot();
    }
    gxd_last_vc = vc;
    return gxd_should_quit(gxd);
}
/* Palette / VRAM windows / OAM live in gx_display's banks, not in gmem. */
static inline int gxd_mem_io(uint32_t a) {
    return (a >= 0x05000000u && a < 0x05000800u) ||
           (a >= 0x06000000u && a < 0x068A0000u) ||
           (a >= 0x07000000u && a < 0x07000800u);
}
static void nnk_display_close(void) {
    if (!gxd) return;
    gxd_frame(gxd);                /* final state of an unfinished frame */
    gxd_frames++;
    nnk_display_shot();
    nnk_display_window_shot();
    fprintf(stderr, "gx: %u frames presented, %u unhandled io writes\\n",
            gxd_frames, gxd_unhandled_io(gxd));
    gxd_close(gxd);
    gxd = NULL;
}
#else
#define nnk_display_tick() 0
#define nnk_display_open() 0
#define nnk_display_close() ((void)0)
#endif

/* ARM7 responder stub: IPCSync send-value countdown + recv FIFO. */
static uint32_t arm7_sync_send = 7;   /* value ARM7 sends to ARM9 (counts 7..0) */
static uint32_t arm9_sync_send = 0;   /* value ARM9 sends to ARM7 (the echo) */
static uint32_t arm7_hold = 0;        /* IPCSync reads left before ARM7 advances its nibble */
/* ARM7 responder hold schedule, measured read-for-read from the melonDS IO
 * bus trace (537-access ground truth): after ARM9's echo write the responder
 * keeps the echoed nibble visible for exactly this many reads, then steps
 * 7->6->...->0.  Round 0 is short (ARM7 waiting at the sync point); later
 * rounds are longer (ARM7 does FIFO/processing work between syncs).
 * Full ARM7 comes later — this is a scripted responder, not an emulator. */
static const uint32_t arm7_hold_sched[8] = { 33, 81, 78, 84, 78, 80, 80, 0 };
static int arm7_round = 0;
/* IPCSync read: returns the current state, then advances the responder's
 * nibble once its hold expires — reproduces the observed value runs
 * (0x0707 x33, 0x0606 x81, 0x0505 x78, ... ) exactly. */
static uint32_t io_ipcsync_read(void) {
    uint32_t ret = ((arm9_sync_send & 0xFu) << 8) | (arm7_sync_send & 0xFu);
    if (arm7_hold > 0) {
        arm7_hold--;
        if (arm7_hold == 0 && arm7_sync_send > 0) arm7_sync_send--;
    }
    return ret;
}
static void io_ipcsync_write(uint32_t v) {
    arm9_sync_send = (v >> 8) & 0xFu;
    if (arm7_round < 8) { arm7_hold = arm7_hold_sched[arm7_round]; arm7_round++; }
    /* Handshake terminator: ARM9's final echo (send=0) completes the IPCSync
     * handshake; the real ARM7 then raises its ready flag in the shared
     * control block — the boot's next action is wait(12,1) @0x02008E1C,
     * observed to succeed on the FIRST call in melonDS (before any FIFO
     * traffic).  Scripted from the observed sequence; ARM7 not executed. */
    if (arm9_sync_send == 0) {
        uint32_t fa = 0x02FFFF8Cu;
        uint8_t *p = main_ram_ptr(fa);
        uint32_t wv = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        wv |= (1u << 12);
        p[0] = (uint8_t)wv; p[1] = (uint8_t)(wv >> 8);
        p[2] = (uint8_t)(wv >> 16); p[3] = (uint8_t)(wv >> 24);
    }
}
static uint32_t ipc_fifo_cnt = 0;     /* IPCFIFOCNT stored config bits */
static uint32_t arm9_recv_fifo[16];   /* words ARM9 can pop at 0x04100000 */
static int arm9_recv_n = 0;

/* ARM7 init-event chain (Phase-4A).  The ARM7's IRQ-driven init posts its
 * event bits one by one into the shared main-RAM flag word (Main RAM
 * 4 MiB-mirror offset 0x3FFF8C — the ARM9 polls it at 0x02FFFF8C through
 * the SDK test_bit subroutine 0x0200BA4C; the loop at 0x0201C4B8 is
 * wait(13,1) on bit 13 of that word).  The ARM7 is NOT executed here: this
 * chain is scripted from a measured melonDS capture (the new MELONDS_MEMWATCH_*
 * hook in melonDS/src/NDS.cpp logs every write to the flag word from either
 * CPU together with the ARM9 PC-trace index at that moment):
 *
 *   a9idx      value      meaning (delta from previous)
 *   3633523 -> 0x00001000  bit12 (ARM9 spinning at 0x02009B44)
 *   3633806 -> 0x00003000  bit13 (ARM9 inside test_bit = wait(13,1) iter 2;
 *                              melonDS exits that loop on iteration 3)
 *   3635544 -> 0x00013000  +bit16
 *   3635618 -> 0x00033000  +bit17
 *   3650209 -> 0x00033080  +bit7   (ARM9 now takes IRQs: pc9=0xFFFF01DC)
 *   3650651 -> 0x00033880  +bit11
 *   3650668 -> 0x00037880  +bit14
 *   3652929 -> 0x000378A0  +bit5
 *   3654408 -> 0x00037CA0  +bit10
 *   3768189 -> 0x00037CE0  +bit6   (these four post in one burst while the
 *   3768189 -> 0x00037EE0  +bit9    melonDS ARM9 sits in its BIOS IRQ
 *   3768189 -> 0x00037FE0  +bit8    handler, so the measured index is frozen
 *   3768189 -> 0x00037FF0  +bit4    there; native steps keep counting)
 *
 * Clock: this run's `steps` counter, which equals the melonDS ARM9 trace
 * index throughout the matched prefix (both count one per dispatched guest
 * instruction).  After the first model-gap divergence the correspondence
 * degrades — honest and documented (IO_MAP.md). */
struct Arm7Evt { uint64_t step; uint32_t val; };
static const struct Arm7Evt arm7_evt_log[] = {
    {3633523, 0x00001000u},
    {3633806, 0x00003000u},
    {3635544, 0x00013000u},
    {3635618, 0x00033000u},
    {3650209, 0x00033080u},
    {3650651, 0x00033880u},
    {3650668, 0x00037880u},
    {3652929, 0x000378A0u},
    {3654408, 0x00037CA0u},
    {3768189, 0x00037CE0u},
    {3768189, 0x00037EE0u},
    {3768189, 0x00037FE0u},
    {3768189, 0x00037FF0u},
};
#define ARM7_EVT_N ((int)(sizeof arm7_evt_log / sizeof arm7_evt_log[0]))
static int arm7_evt_n = 0;
/* Called once per dispatch from run(); fast path is one load+compare. */
static void arm7_evt_tick(void) {
    if (arm7_evt_n < ARM7_EVT_N && steps >= arm7_evt_log[arm7_evt_n].step) {
        uint32_t fa = 0x02FFFF8Cu;   /* = MainRAM[0x3FFF8C] in melonDS */
        uint32_t wv = arm7_evt_log[arm7_evt_n].val;
        uint8_t *p = main_ram_ptr(fa);
        p[0] = (uint8_t)wv; p[1] = (uint8_t)(wv >> 8);
        p[2] = (uint8_t)(wv >> 16); p[3] = (uint8_t)(wv >> 24);
        fprintf(stderr, "arm7 evt step=%llu val=0x%08X\n",
                (unsigned long long)steps, wv);
        arm7_evt_n++;
    }
}

/* ---- Card bus state (Phase-4D: melonDS NDSCart.cpp transfer timing) ------
 * The card interface is a *scheduled* state machine, not an instantaneous
 * copy.  Transcribed from melonDS 1.1 src/NDSCart.cpp (WriteROMCnt,
 * ROMTransfer_ReceiveData, ROMAdvanceReceive, ReadROMData, ROMEndTransfer).
 * Ground truth: the timestamped ARM9 IO bus capture (tools/melonds-io-trace.patch
 * with ts= = NDS::ARM9Timestamp >> ARM9ClockShift).  For the boot's
 * steady-state card read (ROMCNT=0xA1416657 -> delay13=0x657, gap2=1,
 * xfercycle=5, 0x200 bytes) the guest polls ROMCNT 909 times before DRQ
 * (bit23) first reads set, then streams 128 words, and the poll loop itself
 * costs 9 master cycles per iteration (measured dts: poll->poll 9, set-poll
 * -> data 6, data -> next poll 9).  The runtime keeps a second clock,
 * card_cycles, advanced by a measured per-instruction cost (emitted as
 * `card_tick(<cost>)` at the top of every dispatch case), and drives the
 * transfer in melonDS's own units:
 *     xfercycle = (ROMCnt & (1<<27)) ? 8 : 5
 *     cmddelay  = 8 + (ROMCnt & 0x1FFF) [+ ((ROMCnt >> 16) & 0x3F) if size]
 *     first word after xfercycle * (cmddelay + 4) cycles, each further word
 *     after xfercycle * (4 [+ gap2 when pos is at a 0x200-byte boundary]).
 * A separate clock is used on purpose: the pre-existing DISPSTAT/VCOUNT/timer
 * model derives from `cycles` (1 per instruction) and still trace-matches, so
 * it is left untouched. */
static uint8_t card_cmd[8];
static uint8_t *rom = NULL; static size_t rom_len = 0;
static uint32_t spicnt = 0, romcnt = 0;
static int rom_loaded = 0;

#define CARD_NEVER (~0ull)
static uint64_t card_cycles;         /* measured-cost clock for the card bus */
static uint64_t card_next = CARD_NEVER;  /* next scheduled event time */
static int card_end_pending;         /* scheduled event is ROMTransfer_End */
static int card_drq;                 /* ROMCNT bit23 (word available) */
static uint32_t card_addr, card_len, card_pos;  /* ROM byte addr / size / pos */
static uint32_t card_fifo[2];
static int card_poscpu, card_poscart, card_count, card_late;
static uint32_t card_xfer = 5, card_gap2;
static uint32_t card_op;              /* opcode byte of the running transfer */
static int card_poll_n;              /* ROMCNT polls since the transfer began */
static int card_drq_log_n;           /* DRQ-onset logs emitted */
static int card_xfer_log_n;          /* transfer-start logs emitted */

/* Timers, DMA, interrupts, display, misc. */
static uint32_t dispstat = 0, dispcnt = 0;
static uint32_t ime = 0, ie = 0, iflags = 0;
static uint32_t tm_reload[4], tm_ctrl[4], tm_val[4];
static uint32_t dma_sad[4], dma_dad[4], dma_cnt[4];
static uint32_t exmemcnt = 0xE880u, powerctl = 0; static uint8_t postflg = 1;
/* exmemcnt initial value = the ARM9 BIOS's pre-entry EXMEMCNT state,
 * measured as the first EXMEMCNT read-back before any guest write
 * (melonDS IO trace: R16 0x04000204 0x0000E880 pc=0x02006268); melonDS's
 * Reset value alone is 0x6000, so the BIOS writes this before entry.  The
 * runtime starts at the entry point, so the value is initial state here.
 * With it, the guest's read-modify-write EXMEMCNT sequence (0xE880 ->
 * 0xE800 -> 0xE80C -> 0x680C -> ...) reproduces melonDS's exactly, and the
 * GBA-slot open-bus decay derived from bits 3..2 matches at every read. */
static uint32_t div_num_lo = 0, div_num_hi = 0, div_den_lo = 0, div_den_hi = 0;
static uint32_t div_ctrl = 0, div_quot = 0, div_rem = 0;
static uint32_t sqrt_in_lo = 0, sqrt_in_hi = 0, sqrt_ctrl = 0, sqrt_res = 0;
static uint32_t vramcnt[9]; static uint32_t wramcnt_map = 0;

/* ---- Phase-4M: MMIO readback for the display-programming registers --------
 * THREE measured barriers, one class: a register the guest READS through a
 * read-modify-write sequence must return what it wrote (melonDS GPU::Read16/
 * Read32 serve every one of these from the live GPU state).
 *
 * (a) VRAMCNT_A..I (0x04000240..0x04000248) were STORE-ONLY and 8-BIT-ONLY:
 *     io_write16_sw/io_write32_sw had no case (a wide store was dropped) and
 *     io_read16/io_read32 had none either (reads returned 0).  melonDS's own
 *     sequence uses both wide forms first (W32 clear of A..D, W16 clear of
 *     H..I) and then programs the banks byte-wise; the mapping those bytes
 *     select is what the 2D compositor reads its layers through (gx_display
 *     owns the MapVRAM_A..I table and the flat-view rebuild).
 * (b) POWCNT1 (0x04000304) had NO readback.  The guest's display-init at
 *     0x02002A3C..0x02002A64 is a read-modify-write over it; reading 0 made
 *     the final store 0x0001 instead of melonDS's 0x820F, so the screen and
 *     both 2D engines stayed OFF -> every frame was uniform black.
 * (c) The engine B 2D register block (0x04001000..0x0400106F) had no
 *     readback at all (those registers live only in gx_display, which is
 *     write-only from the runtime).  The guest's RMW at 0x02041710 therefore
 *     wrote DISPCNT_B = 0x00200010 instead of melonDS's 0x80211013, leaving
 *     engine B in DS display mode 0 ("display off" = white) with no BG
 *     enables -> uniform white frames.
 *
 * The model below reproduces melonDS: width-correct register file for the 2D
 * blocks (DISPSTAT/VCOUNT stay live), width-correct VRAMCNT with VRAMSTAT
 * derived from the ARM7-mapped banks, and POWCNT1 readback.  gx_display
 * remains the authority for the RENDERING effect (every store is still
 * forwarded to gxd_io_write with the guest's width); this is the guest's view.
 * tools/nnk_vramcnt.inc carries the same model, plus write tracing and a
 * bank-map report, for the already-generated image. */
static int vrc_in(uint32_t a) { return a >= 0x04000240u && a <= 0x0400024Bu; }
static void vrc_write(uint32_t a, uint32_t v, int width) {
    int bank = (int)(a - 0x04000240u), i;
    for (i = 0; i < width && bank + i < 9; i++) vramcnt[bank + i] = (v >> (8 * i)) & 0xFFu;
}
static uint32_t vrc_read(uint32_t a, int width) {
    int bank = (int)(a - 0x04000240u), i; uint32_t r = 0;
    for (i = 0; i < width && bank + i < 9; i++) r |= (vramcnt[bank + i] & 0xFFu) << (8 * i);
    return r;
}
/* VRAMSTAT (0x0400024E): bit0 = VRAM_C mapped to ARM7, bit1 = VRAM_D. */
static uint32_t vrc_stat(void) {
    uint32_t s = 0;
    if ((vramcnt[2] & 0x87u) == 0x82u) s |= 1u;
    if ((vramcnt[3] & 0x87u) == 0x82u) s |= 2u;
    return s;
}
/* 2D register-block shadow: what the guest wrote, byte for byte.
 * DISPSTAT (0x04) and VCOUNT (0x06) are excluded - they are live. */
static uint8_t io2d_sh[2][0x70];
static int io2d_off(uint32_t a, int *eng, uint32_t *off) {
    if (a >= 0x04000000u && a < 0x04000070u) { *eng = 0; *off = a - 0x04000000u; return 1; }
    if (a >= 0x04001000u && a < 0x04001070u) { *eng = 1; *off = a - 0x04001000u; return 1; }
    return 0;
}
static void io2d_write(uint32_t a, uint32_t v, int width) {
    int e, i; uint32_t off;
    if (!io2d_off(a, &e, &off) || off == 0x04u || off == 0x06u) return;
    for (i = 0; i < width && off + (uint32_t)i < 0x70u; i++)
        io2d_sh[e][off + (uint32_t)i] = (uint8_t)(v >> (8 * i));
}
static int io2d_read(uint32_t a, int width, uint32_t *out) {
    int e, i; uint32_t off, r = 0;
    if (!io2d_off(a, &e, &off) || off == 0x04u || off == 0x06u) return 0;
    for (i = 0; i < width && off + (uint32_t)i < 0x70u; i++)
        r |= (uint32_t)io2d_sh[e][off + (uint32_t)i] << (8 * i);
    *out = r; return 1;
}

/* DIV (0x04000280 mode: 0=32/32, 1=64/32, 2=64/64) and SQRT computed on write. */
static void io_div_do(void) {
    uint32_t mode = div_ctrl & 3u;
    int64_t num, den; int64_t q, rm;
    if (mode == 0) { num = (int32_t)div_num_lo; den = (int32_t)div_den_lo; }
    else if (mode == 1) { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int32_t)div_den_lo; }
    else { num = (int64_t)(((uint64_t)div_num_hi << 32) | div_num_lo); den = (int64_t)(((uint64_t)div_den_hi << 32) | div_den_lo); }
    if (den == 0) { div_ctrl |= 0x4000u; div_quot = 0; div_rem = 0; return; }
    q = num / den; rm = num % den;
    div_quot = (uint32_t)q; div_rem = (uint32_t)rm;
    div_ctrl |= 0x8000u;   /* busy then immediately clear (approximation) */
    div_ctrl &= ~0x8000u;
}
static void io_sqrt_do(void) {
    uint64_t x = (sqrt_ctrl & 1u) ? (((uint64_t)sqrt_in_hi << 32) | sqrt_in_lo) : (uint64_t)sqrt_in_lo;
    uint64_t res = 0, bit = 1ull << 62; while (bit > x) bit >>= 2; while (bit) { if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
    sqrt_res = (uint32_t)res; sqrt_ctrl |= 0x8000u; sqrt_ctrl &= ~0x8000u;
}

static void io_ovl_init(void);   /* Phase-4G overlay-slot registry (defined below) */
static void io_load_rom(void) {
    const char *p = getenv("NNK_ROM_PATH");
    if (!p || !p[0]) { fprintf(stderr, "io: NNK_ROM_PATH unset; card reads return 0\\n"); return; }
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "io: cannot open ROM %s\\n", p); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { rom = malloc((size_t)n); rom_len = fread(rom, 1, (size_t)n, f); }
    fclose(f); rom_loaded = 1;
    fprintf(stderr, "io: card rom %zu bytes from %s\\n", rom_len, p);
    io_ovl_init();
}

/* ---- overlay-slot registry + per-load overlay dumps (Phase-4G) -----------
 * The dispatch image has ONE case per PC, but a NITRO overlay ARENA holds a
 * different module per load (overlays 0..13 all load at 0x02079F80, 14..16 at
 * 0x020C18E0, 17..18 at 0x020D0A00, 19..20 at 0x02159FE0), and the guest
 * decrypts + initialises pages of the resident module at run time.  This
 * registry is the one place that answers "which overlay is resident in which
 * arena, in which load generation", derived from the card transfers the guest
 * actually performs (a 0xB7 read whose address is the first byte of an ARM9
 * overlay's FAT range IS that overlay's load) plus the ROM's overlay table.
 *
 *   NNK_OVL_TRACE=<path>  write one `ovl_load ...` line per load; this is the
 *                         build-time input of tools/ovl_registry.py --trace
 */
static struct { uint32_t file_id, rom_off, rom_end, ram, ram_size; } ovl_tab[32];
static int ovl_n;
static struct { uint32_t arena; int id; int gen; } ovl_arena[8];
static int ovl_arena_n;
static FILE *ovl_fp;
static int ovl_init_done, ovl_load_seq;

static uint32_t ovl_rd32(size_t o) {
    if (!rom || o + 4u > rom_len) return 0;
    return (uint32_t)rom[o] | ((uint32_t)rom[o + 1u] << 8) |
           ((uint32_t)rom[o + 2u] << 16) | ((uint32_t)rom[o + 3u] << 24);
}
static void io_ovl_init(void) {
    const char *p;
    if (ovl_init_done) return;
    ovl_init_done = 1;
    if (!rom || rom_len < 0x60u) return;
    {
        uint32_t fat_off = ovl_rd32(0x48), ovt_off = ovl_rd32(0x50), ovt_size = ovl_rd32(0x54);
        uint32_t i, n = ovt_size / 32u;
        if (n > 32u) n = 32u;
        for (i = 0; i < n; i++) {
            size_t o = (size_t)ovt_off + 32u * (size_t)i;
            uint32_t file_id = ovl_rd32(o + 24u);
            if ((size_t)fat_off + 8u * (size_t)file_id + 8u > rom_len) continue;
            ovl_tab[ovl_n].file_id = file_id;
            ovl_tab[ovl_n].rom_off = ovl_rd32((size_t)fat_off + 8u * (size_t)file_id);
            ovl_tab[ovl_n].rom_end = ovl_rd32((size_t)fat_off + 8u * (size_t)file_id + 4u);
            ovl_tab[ovl_n].ram = ovl_rd32(o + 4u);
            ovl_tab[ovl_n].ram_size = ovl_rd32(o + 8u);
            ovl_n++;
        }
    }
    p = getenv("NNK_OVL_TRACE");
    if (p && p[0]) {
        ovl_fp = fopen(p, "w");
        if (ovl_fp) setvbuf(ovl_fp, NULL, _IOLBF, 0);
    }
    if (ovl_n)
        fprintf(stderr, "io: overlay registry: %d ARM9 overlays from the ROM overlay table\\n", ovl_n);
}
static void io_ovl_note(uint32_t addr) {
    int i;
    if (!ovl_init_done) io_ovl_init();
    for (i = 0; i < ovl_n; i++) {
        int g = 0, j;
        if (addr != ovl_tab[i].rom_off) continue;
        for (j = 0; j < ovl_arena_n; j++)
            if (ovl_arena[j].arena == ovl_tab[i].ram) {
                ovl_arena[j].id = i; ovl_arena[j].gen++; g = ovl_arena[j].gen; break;
            }
        if (j == ovl_arena_n && ovl_arena_n < 8) {
            ovl_arena[ovl_arena_n].arena = ovl_tab[i].ram;
            ovl_arena[ovl_arena_n].id = i;
            ovl_arena[ovl_arena_n].gen = 1;
            g = 1;
            ovl_arena_n++;
        }
        ovl_load_seq++;
        if (ovl_fp)
            fprintf(ovl_fp, "ovl_load seq=%d id=%d rom=0x%08X size=0x%X dest=0x%08X ram_size=0x%X gen=%d\\n",
                    ovl_load_seq, i, ovl_tab[i].rom_off,
                    ovl_tab[i].rom_end - ovl_tab[i].rom_off,
                    ovl_tab[i].ram, ovl_tab[i].ram_size, g);
        fprintf(stderr, "ovl: load id=%d rom=0x%08X size=0x%X dest=0x%08X gen=%d\\n",
                i, ovl_tab[i].rom_off, ovl_tab[i].rom_end - ovl_tab[i].rom_off,
                ovl_tab[i].ram, g);
        return;
    }
}

/* ---- BIOS boot state (Phase-4C2) -----------------------------------------
 * The runtime starts at the ARM9 entry, so everything the DS BIOS had written
 * into main RAM before entry is initial state here (same precedent as the
 * pre-entry EXMEMCNT value above).  Two measured blocks matter, both captured
 * pre-entry from melonDS (tools/add_memwatch_window.py, memwatch dump):
 *
 *  (1) 0x027FFE00 — BIOS ARM9 boot params: a 0x200-byte copy of the cartridge
 *      header.  The guest reads it through the main-RAM mirror as 0x02FFFE00;
 *      at 0x02016FB8 it does `bl 0x02015B50` -> 0x02FFFE00 then
 *      `ldr r0,[r0,#128]` and needs header word 0x80 (0x1D12663C) nonzero to
 *      take `bl 0x020064F8` at 0x02016FC8.  With this block zero the guest
 *      took the `popeq` path to 0x02015A8C and the PC trace diverged from
 *      melonDS at index 3,638,594 — that is the Phase-4C2 first divergence.
 *      The bytes come from the ROM the runtime already loads for the card
 *      server (NNK_ROM_PATH), so no ROM data is stored in this source.  The
 *      BIOS zeroes header bytes 0x160..0x1BF of the copy (measured: the only
 *      byte that differs from the ROM header is offset 0x1BC, 0x60 in the ROM
 *      vs 0 in the copied block).
 *  (2) 0x027FF800 / 0x027FFC00 — BIOS/firmware boot params: the WifiFlash
 *      chip id pair and the firmware user-settings block (0x027FFC40 = WFC
 *      enabled, 0x027FFC80 = firmware version + nickname string, 0x027FFCE0 =
 *      WIFI calibration).  Values are the measured pre-entry contents of the
 *      melonDS firmware image it boots with; they are boot state, not game or
 *      ROM data.  Written before the guest runs so a guest read of the
 *      firmware area sees the same bytes melonDS returned. */
static const struct { uint32_t a, v; } bios_bootparam[20] = {
    {0x027FF800u, 0x0000FEC2u}, {0x027FF804u, 0x0000FEC2u},
    {0x027FF808u, 0x1DE63A18u}, {0x027FF850u, 0x00005835u},
    {0x027FF868u, 0x0001FE00u},
    {0x027FFC00u, 0x0000FEC2u}, {0x027FFC04u, 0x0000FEC2u},
    {0x027FFC08u, 0x1DE63A18u}, {0x027FFC10u, 0x00005835u},
    {0x027FFC30u, 0x0000FFFFu}, {0x027FFC40u, 0x00000001u},
    {0x027FFC80u, 0x01000005u}, {0x027FFC84u, 0x006D0001u},
    {0x027FFC88u, 0x006C0065u}, {0x027FFC8Cu, 0x006E006Fu},
    {0x027FFC90u, 0x00530044u}, {0x027FFC98u, 0x00070000u},
    {0x027FFCDCu, 0x0FF00000u}, {0x027FFCE0u, 0xBFFF0BF0u},
    {0x027FFCE4u, 0x00000031u}
};
static void bios_boot_seed(void) {
    unsigned i;
    if (rom_loaded && rom_len >= 0x200) {
        memcpy(main_ram_ptr(0x027FFE00u), rom, 0x200);
        memset(main_ram_ptr(0x027FFE00u) + 0x160, 0, 0x60);
    } else {
        fprintf(stderr, "bios: no ROM loaded; header copy at 0x027FFE00 stays zero\\n");
    }
    for (i = 0; i < sizeof bios_bootparam / sizeof bios_bootparam[0]; i++)
        *(uint32_t *)main_ram_ptr(bios_bootparam[i].a) = bios_bootparam[i].v;
    fprintf(stderr, "bios: boot state seeded (header copy 0x027FFE00, %u bootparam words)\\n", i);
}

static uint32_t io_read8(uint32_t a);
static void io_write8(uint32_t a, uint32_t v);
static uint32_t io_read16(uint32_t a);
static void io_write16(uint32_t a, uint32_t v);
static uint32_t io_read32(uint32_t a);
static void io_write32(uint32_t a, uint32_t v);
static uint32_t io_read8_sw(uint32_t a);
static void io_write8_sw(uint32_t a, uint32_t v);
static uint32_t io_read16_sw(uint32_t a);
static void io_write16_sw(uint32_t a, uint32_t v);
static uint32_t io_read32_sw(uint32_t a);
static void io_write32_sw(uint32_t a, uint32_t v);
static void halt_reason(const char *msg);

/* GBA slot bus (Phase-4B): no GBA cart is attached, so reads follow
 * melonDS's empty-slot open-bus (GBACart.cpp GBACartSlot::ROMRead):
 *   halfword = ((addr >> 1) & 0xFFFF) | decay,  decay = openbus[(EXMEMCNT
 *   >> 2) & 3],  openbus[] = {0xFE08, 0x0000, 0x0000, 0xFFFF} (NDS.cpp),
 * and EXMEMCNT bit7 (=1: ARM7 owns the slot) fills the deselected CPU's
 * reads with 0 (melonDS ARM9Read8/16/32, "deselected CPU is 00h-filled").
 * Measured for this boot: at the ITCM DMA1 halfword fill from 0x08000080
 * (melonDS IO trace: W32 0x040000BC=0x08000080 / 0x040000C0=0x0206F500 /
 * 0x040000C4=0x80000020 at pc9=0x01FF84C8..0x01FF84D0, EXMEMCNT still
 * 0x680C per R16 0x04000204 pc=0x0201C5D8) the slot reads 0xFFFF, which is
 * the fill value that later flows 0x0206F500..0x0206F53E -> 0x02FFFC30 and
 * gates the 0x0201C198 wait — an unmapped-0 read there deadlocked the run. */
static const uint32_t gba_openbus_tbl[4] = { 0xFE08u, 0u, 0u, 0xFFFFu };
static inline int is_gba_rom(uint32_t a) {
    return (a & 0xFF000000u) == 0x08000000u || (a & 0xFF000000u) == 0x09000000u;
}
static inline int is_gba_ram(uint32_t a) { return (a & 0xFF000000u) == 0x0A000000u; }
static inline uint32_t gba_bus16(uint32_t a) {
    if (exmemcnt & (1u << 7)) return 0;
    return ((a >> 1) & 0xFFFFu) | gba_openbus_tbl[(exmemcnt >> 2) & 3u];
}

/* Guest accessors.  TCM routing comes FIRST, in melonDS's data-path order
 * (CP15.cpp DataRead32/DataWrite32: ITCM, then DTCM, then the bus — IO and the
 * flat window are "the bus" here): an ITCM hit lands in itcm[addr & 0x7FFF],
 * a DTCM hit in dtcm[addr & 0x3FFF].  With TCMs disabled the checks can
 * never match (itcm_size == 0; dtcm_base == 0xFFFFFFFF), so unmapped low
 * addresses keep reading 0 / dropping writes as before. */
/* Bus address -> gmem offset.  melonDS maps the ENTIRE 0x02000000..0x02FFFFFF
 * block onto main RAM with `addr & MainRAMMask` (NDS.cpp ARM9Read32/Write32,
 * `case 0x02000000`), i.e. the 4 MiB of main RAM is mirrored four times, and
 * the SDK depends on it: boot params sit at 0x027FFE00 (= main-RAM 0x3FFE00)
 * and the 0x02FFxxxx block the boot uses aliases main-RAM 0x3Fxxxx.  A flat
 * window without the mirror diverged at trace index 3,638,594 (0x02016FBC
 * `ldr r0,[r0,#128]` from the constant 0x02FFFE00 read back 0 instead of
 * 0x1D12663C).  Addresses outside the block use the window as before
 * (shared WRAM at 0x03000000+).  Returns 0 when the address is not memory. */
static inline int win_off(uint32_t a, uint32_t n, uint32_t *off) {
    if ((a & 0xFF000000u) == 0x02000000u) {
        uint32_t o = a & 0x3FFFFFu;     /* MainRAMMask: 4 MiB main RAM */
        if (o + n > 0x400000u) return 0;
        *off = o; return 1;
    }
    if (a < GBASE || (uint64_t)a + n > (uint64_t)GBASE + GSIZE) return 0;
    *off = a - GBASE; return 1;
}
static inline uint32_t ld32(uint32_t a) {
    if (a < itcm_size) {
        const uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if ((a & dtcm_mask) == dtcm_base) {
        const uint8_t *p = dtcm + (a & (DTCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (is_gba_rom(a)) return gba_bus16(a) | (gba_bus16(a + 2u) << 16);
    if (is_gba_ram(a)) return (exmemcnt & (1u << 7)) ? 0u : 0xFFFFFFFFu;
    if (a >= IO_LO && a < IO_HI) return io_read32(a);
    { uint32_t o; if (win_off(a, 4u, &o)) { const uint8_t *p = gmem + o;
      return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); } }
    return 0;
}
/* Phase-4J: code-window write tracking (SMC).  Defined in tools/nnk_smc.inc,
 * which the dispatch image pulls in at the end of this prelude; every guest
 * store reports its address so the affected 32 KiB band's generation counter
 * advances and compiled cases for that band are invalidated at dispatch. */
static inline void smc_note(uint32_t a, uint32_t n);

static inline void st32(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 4); return; }
#endif
    smc_note(a, 4u);
    if (a < itcm_size) {
        uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        return;
    }
    if ((a & dtcm_mask) == dtcm_base) {
        uint8_t *p = dtcm + (a & (DTCM_PHYS - 1u));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
        return;
    }
    if (a >= IO_LO && a < IO_HI) { io_write32(a, v); return; }
    { uint32_t o; if (win_off(a, 4u, &o)) { uint8_t *p = gmem + o;
      p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); } }
}
static inline uint32_t ld16(uint32_t a) {
    if (a < itcm_size) {
        const uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    }
    if ((a & dtcm_mask) == dtcm_base) {
        const uint8_t *p = dtcm + (a & (DTCM_PHYS - 1u));
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    }
    if (is_gba_rom(a)) return gba_bus16(a);
    if (is_gba_ram(a)) return (exmemcnt & (1u << 7)) ? 0u : 0xFFFFu;
    if (a >= IO_LO && a < IO_HI) return io_read16(a);
    { uint32_t o; if (win_off(a, 2u, &o)) { const uint8_t *p = gmem + o;
      return (uint32_t)p[0] | ((uint32_t)p[1] << 8); } }
    return 0;
}
static inline void st16(uint32_t a, uint16_t v) {
#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 2); return; }
#endif
    smc_note(a, 2u);
    if (a < itcm_size) {
        uint8_t *p = itcm + (a & (ITCM_PHYS - 1u));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        return;
    }
    if ((a & dtcm_mask) == dtcm_base) {
        uint8_t *p = dtcm + (a & (DTCM_PHYS - 1u));
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        return;
    }
    if (a >= IO_LO && a < IO_HI) { io_write16(a, v); return; }
    { uint32_t o; if (win_off(a, 2u, &o)) { uint8_t *p = gmem + o;
      p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); } }
}
static inline uint32_t ld8(uint32_t a) {
    if (a < itcm_size) return itcm[a & (ITCM_PHYS - 1u)];
    if ((a & dtcm_mask) == dtcm_base) return dtcm[a & (DTCM_PHYS - 1u)];
    if (is_gba_rom(a)) return (gba_bus16(a & ~1u) >> ((a & 1u) * 8)) & 0xFFu;
    if (is_gba_ram(a)) return (exmemcnt & (1u << 7)) ? 0u : 0xFFu;
    if (a >= IO_LO && a < IO_HI) return io_read8(a);
    { uint32_t o; if (win_off(a, 1u, &o)) return gmem[o]; }
    return 0;
}
static inline void st8(uint32_t a, uint32_t v) {
#ifdef NNK_GX
    if (gxd && gxd_mem_io(a)) { gxd_io_write(gxd, a, v, 1); return; }
#endif
    smc_note(a, 1u);
    if (a < itcm_size) { itcm[a & (ITCM_PHYS - 1u)] = (uint8_t)v; return; }
    if ((a & dtcm_mask) == dtcm_base) { dtcm[a & (DTCM_PHYS - 1u)] = (uint8_t)v; return; }
    if (a >= IO_LO && a < IO_HI) { io_write8(a, v); return; }
    { uint32_t o; if (win_off(a, 1u, &o)) gmem[o] = (uint8_t)v; }
}

/* IO register semantics (melonDS-derived).  Implemented for the measured
 * boot set; everything else in the window reads 0 / drops writes (logged). */
static uint32_t io_dispstat_read(void) {
    uint32_t vc = vcount();
    uint32_t r = dispstat & 0xFFF8;          /* keep irq enables + vcount-match value */
    if (vc >= 192) r |= 1;                   /* vblank */
    if (((dispstat >> 8) & 0xFF) == vc) r |= 4; /* vcount-match */
    return r;
}
static void io_do_dma(int i) {
    uint32_t cnt = dma_cnt[i];
    if (!(cnt & 0x80000000u)) return;
    uint32_t timing = (cnt >> 27) & 7u;      /* Phase-4L: ARM9 field is bits 27..29
                                              * (melonDS DMA::WriteCnt StartMode);
                                              * bits 24/26 are the source-address
                                              * control / 32-bit size flags, so the
                                              * old `(cnt >> 24) & 3` misread e.g.
                                              * 0x85000018 (immediate, 32-bit, fixed
                                              * source) as a timed transfer. */
    if (timing != 0u) return;                /* vblank/hblank: tools/nnk_dma.inc */
    uint32_t count = cnt & 0x7FFFu; if (!count) count = 0x8000u;
    int words = (cnt & (1u << 30)) != 0u;    /* bit30: word(1)/half(0) — approximate */
    uint32_t s = dma_sad[i], d = dma_dad[i];
    uint32_t step = words ? 4u : 2u;
    for (uint32_t k = 0; k < count; k++) {
        if (words) st32(d, ld32(s)); else st16(d, (uint16_t)ld16(s));
        s += step; d += step;
    }
    dma_cnt[i] = cnt & ~0x80000000u;         /* clear enable (non-repeating) */
    fprintf(stderr, "io: DMA%d immediate count=%u words=%d\n", i, count, words);
}
/* Card transfer state machine (melonDS NDSCart.cpp, see the state block). */
static void card_advance_receive(void) {
    uint64_t d = (uint64_t)card_xfer * (4u + ((card_pos & 0x1FFu) ? 0u : card_gap2));
    /* Phase-4T (melonDS NDSCart.cpp exact): ROMAdvanceReceive returns WITHOUT
     * scheduling an end once the last word has been produced — in read mode
     * the transfer ends only when the CPU pops that last FIFO word
     * (ReadROMData -> ROMEndTransfer).  Phase-4N scheduled ROMTransfer_End
     * here for a "data-read" transfer, but that is ROMAdvanceSend (write
     * mode) semantics; scheduling it in read mode cleared busy+DRQ ~20
     * cycles after the last word, so the guest's B8 dummy read waited for a
     * DRQ that had already been dropped (deadlock at pc 0x02016974, measured
     * Phase-4T: the B7 stream froze after 1398 starts, melonDS continues). */
    if (card_pos >= card_len) return;
    card_next = card_cycles + d;
}
/* ROMTransfer_ReceiveData: put the next word in the FIFO, then raise DRQ. */
static void card_receive_data(void) {
    uint32_t word = 0, a = card_addr + card_pos;
    if (card_op == 0xB7) {
        if (rom && (size_t)a + 4u <= rom_len)
            word = (uint32_t)rom[a] | ((uint32_t)rom[a+1] << 8) |
                   ((uint32_t)rom[a+2] << 16) | ((uint32_t)rom[a+3] << 24);
    } else if (card_op == 0xB8) {
        /* "get cart chip id" (melonDS NDSCart ROMCommand 0xB8 -> Cart->ID()).
         * Measured in the melonDS capture for THIS cart: the 4-byte answer is
         * 0x0000FEC2 (tools/card_commands.py on p4d_io_ts.txt, the 0xB8
         * transfer right after the boot's first 0xB7 read; melonDS also logs
         * "Inserted cart with ID: 0000FEC2").  Not guessed. */
        word = (card_pos == 0u) ? 0x0000FEC2u : 0u;
    } else if (card_pos == 0u) {
        fprintf(stderr, "io: card cmd 0x%02X has no measured data model (returns 0) pc=0x%08X\n",
                card_op, last_pc);
    }
    card_fifo[card_poscart] = word;
    card_poscart ^= 1;
    card_count++;
    card_pos += 4u;
    card_drq = 1;
    if (card_count < 2) card_advance_receive();
    else { card_late = 1; card_next = CARD_NEVER; }
}
/* ROMEndTransfer: clear busy, drop the transfer. */
static void card_end_transfer(void) {
    romcnt &= ~0x80000000u;
    card_drq = 0;
    card_pos = 0; card_len = 0;
    card_next = CARD_NEVER; card_end_pending = 0;
    /* ROMEndTransfer (melonDS NDSCart.cpp): raise the cart transfer-done IRQ
     * when AUXSPICNT bit14 (transfer IRQ enable) is set.  Phase-4D left this
     * out because the boot capture up to ts 23.1M had no such raise; Phase-4N
     * measured it: the ARM9 WFI idle loop at 0x0200712C wakes on exactly this
     * IRQ (IF/IE bit 19 = IRQ_CartXferDone) once per transfer, every ~21,700
     * card-bus cycles (p4n_wake.txt "irqwake ... enabled=0x00080000"). */
    if (spicnt & (1u << 14)) iflags |= 0x00080000u;
}
/* Called once per dispatched instruction with that instruction's measured
 * cost; fires any card event that has come due. */
static void card_tick(uint32_t cost) {
    card_cycles += cost;
    while (card_next != CARD_NEVER && card_cycles >= card_next) {
        card_next = CARD_NEVER;
        if (card_end_pending) { card_end_pending = 0; card_end_transfer(); }
        else card_receive_data();
    }
}
static void io_card_start(uint32_t val) {
    uint32_t code = (val >> 24) & 7u;
    uint32_t datasize = (code == 7u) ? 4u : (code > 0u ? (0x100u << code) : 0u);
    uint32_t addr = 0, cmddelay;
    if (card_cmd[0] == 0xB7)
        addr = ((uint32_t)card_cmd[1] << 24) | ((uint32_t)card_cmd[2] << 16) |
               ((uint32_t)card_cmd[3] << 8)  | (uint32_t)card_cmd[4];
    else if (card_cmd[0] != 0xB8)
        fprintf(stderr, "io: card cmd 0x%02X (0xB7/0xB8 modelled) pc=0x%08X\n",
                card_cmd[0], last_pc);
    card_op = card_cmd[0];
    if (card_cmd[0] == 0xB7) io_ovl_note(addr);   /* Phase-4G: overlay load event */
    card_xfer = (val & (1u << 27)) ? 8u : 5u;
    card_gap2 = (val >> 16) & 0x3Fu;
    cmddelay = 8u + (val & 0x1FFFu) + (datasize ? card_gap2 : 0u);
    card_drq = 0;
    card_addr = addr; card_len = datasize; card_pos = 0;
    card_poscpu = card_poscart = card_count = card_late = 0;
    card_poll_n = 0;
    card_end_pending = (datasize == 0u);
    card_next = card_cycles + (uint64_t)card_xfer * (cmddelay + (datasize ? 4u : 0u));
    if (card_xfer_log_n < 4) {
        fprintf(stderr, "io: card start cmd=0x%02X addr=0x%08X size=0x%X xfer=%u gap2=%u cmddelay=%u first_event_in=%llu card_cycles=%llu pc=0x%08X\n",
                card_cmd[0], addr, datasize, card_xfer, card_gap2, cmddelay,
                (unsigned long long)(card_next - card_cycles),
                (unsigned long long)card_cycles, last_pc);
        card_xfer_log_n++;
    }
}
static uint32_t io_read32(uint32_t a) {
    { uint32_t _s; if (io2d_read(a, 4, &_s)) return _s; }
    if (vrc_in(a)) return vrc_read(a, 4);
    if (a == 0x0400024Eu) return vrc_stat();
    if ((a & ~3u) == 0x04000304u) return powerctl & 0xFFFFu;
    switch (a) {
    case 0x04000130: case 0x04000132: return io_read16(a);
    case 0x04000000: return dispcnt;
    case 0x04000004: return io_dispstat_read() | (vcount() << 16);
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
        int i = (int)((a - 0x04000100u) >> 2);
        if (tm_ctrl[i] & 0x80u) {
            uint32_t prescale = (tm_ctrl[i] & 3u) == 0 ? 1u : ((tm_ctrl[i] & 3u) == 1 ? 64u : ((tm_ctrl[i] & 3u) == 2 ? 256u : 1024u));
            return (tm_reload[i] + (uint32_t)(cycles / prescale)) & 0xFFFFu;
        }
        return tm_val[i];
    }
    case 0x040000B0: case 0x040000BC: case 0x040000C8: case 0x040000D4: return dma_sad[(a-0x040000B0u)/12u];
    case 0x040000B4: case 0x040000C0: case 0x040000CC: case 0x040000D8: return dma_dad[(a-0x040000B4u)/12u];
    case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: return dma_cnt[(a-0x040000B8u)/12u];
    case 0x04000180: return io_ipcsync_read();
    case 0x04000184: { uint32_t r = ipc_fifo_cnt & 0x8404u; r |= 0x1u; if (arm9_recv_n == 0) r |= 0x100u; return r; }
    case 0x04000188: return 0;
    case 0x04000204: return exmemcnt;
    case 0x04000208: return ime;
    case 0x04000210: return ie;
    case 0x04000214: return iflags;
    case 0x04000280: return div_ctrl;
    case 0x04000290: return div_num_lo;
    case 0x04000294: return div_num_hi;
    case 0x04000298: return div_den_lo;
    case 0x0400029C: return div_den_hi;
    case 0x040002A0: return div_quot;
    case 0x040002A8: return div_rem;
    case 0x040002B0: return sqrt_ctrl;
    case 0x040002B4: return sqrt_res;
    case 0x040002B8: return sqrt_in_lo;
    case 0x040002BC: return sqrt_in_hi;
    case 0x040001A0: return spicnt;
    case 0x040001A4: {
        /* ROMCnt bit31 = transfer busy, bit23 = DRQ (melonDS NDSCart.cpp
         * ReadROMCnt returns the register; RaiseDRQ sets bit23 and
         * ROMEndTransfer clears bit31).  The poll counter is Phase-4D
         * evidence: melonDS needs 909 ROMCNT polls before the first DRQ. */
        if (romcnt & 0x80000000u) {
            card_poll_n++;
            if (card_drq && card_drq_log_n < 4) {
                fprintf(stderr, "io: card drq after %d polls card_cycles=%llu\n",
                        card_poll_n, (unsigned long long)card_cycles);
                card_drq_log_n++;
            }
        }
        return (romcnt & ~0x00800000u) | (card_drq ? 0x00800000u : 0u);
    }
    case 0x04100000: if (arm9_recv_n > 0) { uint32_t v = arm9_recv_fifo[0]; for (int k=1;k<arm9_recv_n;k++) arm9_recv_fifo[k-1]=arm9_recv_fifo[k]; arm9_recv_n--; return v; } ipc_fifo_cnt |= 0x4000u; return 0;
    case 0x04100010: {
        /* NDSCart.cpp ReadROMData: pop the FIFO, clear DRQ, and either resume
         * a stalled (FIFO-full) transfer or end it after the last word. */
        uint32_t ret = card_fifo[card_poscpu];
        if (romcnt & (1u << 30)) return ret;      /* write-mode: no FIFO move */
        card_poscpu ^= 1;
        if (card_count > 0) card_count--;
        card_drq = 0;
        if (card_pos < card_len) {
            if (card_late) { card_late = 0; card_advance_receive(); }
        } else {
            if (card_count == 0) card_end_transfer();
            else card_drq = 1;
        }
        return ret;
    }
    default: return 0;
    }
}
static void io_write32_sw(uint32_t a, uint32_t v) {
    if (vrc_in(a)) { vrc_write(a, v, 4); return; }
    switch (a) {
    case 0x04000000: dispcnt = v; return;
    case 0x04000004: dispstat = v & 0xFFF8u; return;
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
        int i = (int)((a - 0x04000100u) >> 2); tm_reload[i] = v & 0xFFFFu; tm_ctrl[i] = (v >> 16) & 0xFFFFu; return;
    }
    case 0x040000B0: case 0x040000BC: case 0x040000C8: case 0x040000D4: dma_sad[(a-0x040000B0u)/12u] = v; return;
    case 0x040000B4: case 0x040000C0: case 0x040000CC: case 0x040000D8: dma_dad[(a-0x040000B4u)/12u] = v; return;
    case 0x040000B8: case 0x040000C4: case 0x040000D0: case 0x040000DC: { int i=(int)((a-0x040000B8u)/12u); dma_cnt[i]=v; io_do_dma(i); return; }
    case 0x04000180: { /* IPCSync write: ARM9 echoes; responder holds then advances. */
        io_ipcsync_write(v);
        return;
    }
    case 0x04000184: ipc_fifo_cnt = v & 0x8F04u; return;
    case 0x04000188: { /* IPCFIFO_WR: ARM9 request -> ARM7 responder answer. */
        if (v == 0x037A404Du) { arm9_recv_fifo[arm9_recv_n++] = 0x0000004Du; return; }
        halt_reason("ARM7 responder: unscripted IPCFIFO request (see log)");
        fprintf(stderr, "io: unscripted IPCFIFO request 0x%08X at pc=0x%08X\n", v, last_pc);
        return;
    }
    case 0x04000204:
        exmemcnt = v;
        fprintf(stderr, "io: EXMEMCNT=0x%08X pc=0x%08X gba_decay=0x%04X\n",
                v, last_pc, gba_openbus_tbl[(v >> 2) & 3u]);
        return;
    case 0x04000208: ime = v & 1u; return;
    case 0x04000210: ie = v; return;
    case 0x04000214: iflags &= ~v; return;
    case 0x04000280: div_ctrl = v & 0xC003u; return;
    case 0x04000290: div_num_lo = v; return;
    case 0x04000294: div_num_hi = v; return;
    case 0x04000298: div_den_lo = v; io_div_do(); return;
    case 0x0400029C: div_den_hi = v; io_div_do(); return;
    case 0x040002B0: sqrt_ctrl = v & 0x8001u; return;
    case 0x040002B8: sqrt_in_lo = v; io_sqrt_do(); return;
    case 0x040002BC: sqrt_in_hi = v; io_sqrt_do(); return;
    case 0x040001A4: {
        /* WriteROMCnt: bit31 only *starts* a transfer on a 0->1 edge, and only
         * with AUXSPICNT bit15 set and bit13 clear.  ROMCnt holds
         * (val & 0xFF7F7FFF): bit23 is DRQ state, not writable. */
        int xferstart = (v & 0x80000000u) && !(romcnt & 0x80000000u);
        romcnt = v & 0xFF7F7FFFu;
        if ((spicnt & 0x8000u) && !(spicnt & 0x2000u) && xferstart) io_card_start(v);
        return;
    }
    case 0x040001A8: card_cmd[0]=(uint8_t)v; card_cmd[1]=(uint8_t)(v>>8); card_cmd[2]=(uint8_t)(v>>16); card_cmd[3]=(uint8_t)(v>>24); return;
    case 0x040001AC: card_cmd[4]=(uint8_t)v; card_cmd[5]=(uint8_t)(v>>8); card_cmd[6]=(uint8_t)(v>>16); card_cmd[7]=(uint8_t)(v>>24); return;
    case 0x04000304: powerctl = v; return;
    default: return;
    }
}
static uint32_t io_read16(uint32_t a) {
    { uint32_t _s; if (io2d_read(a, 2, &_s)) return _s; }
    switch (a) {
    case 0x04000130:
        /* KEYINPUT: live SDL keypad through gx_display; headless 0x03FF =
         * no key pressed. */
#ifdef NNK_GX
        if (gxd) return gxd_io_read(gxd, a, 2);
#endif
        return 0x03FFu;
    case 0x04000132:
#ifdef NNK_GX
        if (gxd) return gxd_io_read(gxd, a, 2);
#endif
        return 0;
    case 0x04000004: return io_dispstat_read();
    case 0x04000006: return vcount();
    case 0x04000180: return io_ipcsync_read();
    case 0x04000184: { uint32_t r = ipc_fifo_cnt & 0x8404u; r |= 0x1u; if (arm9_recv_n == 0) r |= 0x100u; return r; }
    case 0x040001A0: return spicnt;
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: {
        int i=(int)((a-0x04000100u)>>2); if (tm_ctrl[i]&0x80u){uint32_t ps=(tm_ctrl[i]&3u)==0?1u:((tm_ctrl[i]&3u)==1?64u:((tm_ctrl[i]&3u)==2?256u:1024u)); return (tm_reload[i]+(uint32_t)(cycles/ps))&0xFFFFu;} return tm_val[i];
    }
    case 0x04000102: case 0x04000106: case 0x0400010A: case 0x0400010E: return tm_ctrl[(a-0x04000102u)>>2];
    case 0x04000280: return div_ctrl;
    case 0x040002B0: return sqrt_ctrl;
    case 0x04000300: return postflg;
    default: return io_read32(a) & 0xFFFFu;
    }
}
static void io_write16_sw(uint32_t a, uint32_t v) {
    if (vrc_in(a)) { vrc_write(a, v, 2); return; }
    switch (a) {
    case 0x04000004: dispstat = v & 0xFFF8u; return;
    case 0x04000180: { io_ipcsync_write(v); return; }
    case 0x04000184: ipc_fifo_cnt = v & 0x8F04u; return;
    case 0x040001A0: spicnt = v; return;
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: tm_reload[(a-0x04000100u)>>2] = v; return;
    case 0x04000102: case 0x04000106: case 0x0400010A: case 0x0400010E: tm_ctrl[(a-0x04000102u)>>2] = v; return;
    case 0x04000300: postflg = (uint8_t)v; return;
    default: io_write32_sw(a & ~3u, (io_read32(a & ~3u) & ~(0xFFFFu << ((a&2)*8))) | ((v & 0xFFFFu) << ((a&2)*8))); return;
    }
}
static uint32_t io_read8(uint32_t a) {
    if (a == 0x04000300) return postflg;
    if (vrc_in(a)) return vrc_read(a, 1);
    { uint32_t _s; if (io2d_read(a, 1, &_s)) return _s; }
    return io_read16(a & ~1u) >> ((a & 1u) * 8);
}
static void io_write8_sw(uint32_t a, uint32_t v) {
    if (a == 0x04000300) { postflg = (uint8_t)v; return; }
    if (vrc_in(a)) { vrc_write(a, v, 1); return; }
    io_write16_sw(a & ~1u, v);
}

/* ---- bus-write entry points (Phase-4E: forward display traffic) ----------
 * Every guest store reaches the bus through io_write32/16/8 (from st32/16/8)
 * or directly (DMA).  They forward the write to gx_display with the SAME
 * width the guest used, then run the runtime's own register model (which
 * keeps DISPSTAT/VCOUNT/timers/IPC/card state).  gxd ignores addresses it
 * does not own, so over-forwarding is safe; the *_sw variants are the raw
 * models used for internal read-modify-write composition (no second
 * forwarding, no double-width clobber of the display state). */
static void io_write32(uint32_t a, uint32_t v) {
    io2d_write(a, v, 4);
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 4);
#endif
    io_write32_sw(a, v);
}
static void io_write16(uint32_t a, uint32_t v) {
    io2d_write(a, v, 2);
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 2);
#endif
    io_write16_sw(a, v);
}
static void io_write8(uint32_t a, uint32_t v) {
    io2d_write(a, v, 1);
#ifdef NNK_GX
    if (gxd) gxd_io_write(gxd, a, v, 1);
#endif
    io_write8_sw(a, v);
}

/* ---- ARM9 IRQ delivery (Phase-4C, melonDS ground truth) ------------------
 * Ground truth: tools/add_irqtrace.py hooks (SetIRQ in NDS.cpp, TriggerIRQ
 * in ARM.cpp) capture every IF raise ("setirq") and every taken IRQ
 * ("trigger") with the ARM9 PC-trace index at that moment.  A 10M-PC
 * capture gives 624 raises and exactly 3 deliveries; the delivery condition
 * is ime && (ie & if) && !(CPSR I bit), measured true at precisely those 3.
 *
 * Clock: melonDS's ARM9Trace::Count at a hook = the index of the NEXT PC it
 * will log; this runtime's `steps` at the dispatch of trace-index k equals
 * k+1 (the counter is bumped at the top of the dispatch iteration).  So a
 * melonDS event with a9idx=N is reproduced at the loop top where
 * steps == N+1, i.e. condition `steps > N`.  Both the IF raises and the
 * exception entry are scripted from the measured log AND gated on the live
 * architectural state (ime / ie & if / CPSR.I) at check time — a delivery
 * can never happen unless the real state says it should.
 *
 * Exception entry HLE (melonDS ARM::TriggerIRQ + FreeBIOS interrupt_handler,
 * freebios/src/bios_common.s:1186 — the BIOS vector/epilogue sits at
 * 0xFFFF0018..0xFFFF01DC, untranslated and outside the trace window, so it
 * is HLE'd, not executed):
 *   1. SPSR = current CPSR; CPSR control = 0xD2 (IRQ mode, I+F masked);
 *      r13/r14 swapped with the IRQ bank (seeded with the measured
 *      sp_irq = 0x027E3F7C, constant at all 3 triggers — the BIOS sets it
 *      during boot, which this runtime skips).
 *   2. push {r0-r3, r12, lr = resume+4} on the IRQ stack (the BIOS frame).
 *   3. lr = the BIOS-return sentinel; pc = the user vector read with the
 *      BIOS formula ((dtcm setting & ~0xFF) + 0x4000 - 4 -> dtcm[0x3FFC]),
 *      which reads 0x01FF8000.
 * The SDK stub then runs as normal translated ITCM code: it pushes the
 * sentinel (stmfd sp!, {lr}), reads IME/IE/IF, acks IF, calls the user
 * handler, and pops the sentinel into pc.  A pc >= 0xFFFF0000 dispatched
 * while irq_active IS the BIOS epilogue (ldmia sp!, {r0-r3,r12,lr}; subs
 * pc, lr, #4): pop the frame, restore CPSR from SPSR (mode+flags) and the
 * banked r13/r14, resume at lr-4.  Nothing in the BIOS is traced, matching
 * melonDS's out-of-window capture.
 *
 * Known model gap (unchanged by the banking model below): the stub's
 * task-switch tail (0x01FF80B8.., taken only when [0x0206C6A0] != 0) hands
 * the interrupted context to the SDK scheduler; the tail itself is translated
 * guest code and now runs against the real mode banks. */
#define BIOS_IRQ_LO 0xFFFF0000u
#define BIOS_IRQ_RET (BIOS_IRQ_LO + 0x1F0u)  /* sentinel popped by the stub's popeq {pc} */
static int irq_active;
static int irq_raise_n, irq_trig_n;
static int arm7_ipcstat_done;
/* IRQ_SCRIPT_TABLES_BEGIN (generated by tools/gen_irq_script.py; do not edit) */
struct IrqEvt { uint32_t step; uint32_t mask; };
static const struct IrqEvt irq_raise_log[] = {
    {3635491u, 0x00040000u},
    {3645254u, 0x00080000u},
    {3645515u, 0x00080000u},
    {3797989u, 0x00080000u},
    {3798122u, 0x00080000u},
    {3798215u, 0x00080000u},
    {3807570u, 0x00080000u},
    {3807727u, 0x00080000u},
    {3807820u, 0x00080000u},
    {3816845u, 0x00080000u},
    {3816978u, 0x00080000u},
    {3817071u, 0x00080000u},
    {3820188u, 0x00080000u},
    {3820281u, 0x00080000u},
    {4039510u, 0x00080000u},
    {4045700u, 0x00080000u},
    {4051890u, 0x00080000u},
    {4058080u, 0x00080000u},
    {4064270u, 0x00080000u},
    {4070460u, 0x00080000u},
    {4076740u, 0x00080000u},
    {4082930u, 0x00080000u},
    {4089120u, 0x00080000u},
    {4095310u, 0x00080000u},
    {4101500u, 0x00080000u},
    {4107690u, 0x00080000u},
    {4113880u, 0x00080000u},
    {4120070u, 0x00080000u},
    {4126260u, 0x00080000u},
    {4132450u, 0x00080000u},
    {4138640u, 0x00080000u},
    {4144830u, 0x00080000u},
    {4151020u, 0x00080000u},
    {4157210u, 0x00080000u},
    {4163400u, 0x00080000u},
    {4169590u, 0x00080000u},
    {4175780u, 0x00080000u},
    {4182000u, 0x00080000u},
    {4188190u, 0x00080000u},
    {4194380u, 0x00080000u},
    {4200570u, 0x00080000u},
    {4206760u, 0x00080000u},
    {4212950u, 0x00080000u},
    {4219140u, 0x00080000u},
    {4225330u, 0x00080000u},
    {4231520u, 0x00080000u},
    {4237710u, 0x00080000u},
    {4243900u, 0x00080000u},
    {4250090u, 0x00080000u},
    {4256280u, 0x00080000u},
    {4262470u, 0x00080000u},
    {4268660u, 0x00080000u},
    {4274850u, 0x00080000u},
    {4281040u, 0x00080000u},
    {4287230u, 0x00080000u},
    {4293420u, 0x00080000u},
    {4299610u, 0x00080000u},
    {4305800u, 0x00080000u},
    {4311990u, 0x00080000u},
    {4318180u, 0x00080000u},
    {4324370u, 0x00080000u},
    {4330560u, 0x00080000u},
    {4336750u, 0x00080000u},
    {4342940u, 0x00080000u},
    {4349130u, 0x00080000u},
    {4355320u, 0x00080000u},
    {4361510u, 0x00080000u},
    {4367700u, 0x00080000u},
    {4373890u, 0x00080000u},
    {4380080u, 0x00080000u},
    {4386270u, 0x00080000u},
    {4392470u, 0x00080000u},
    {4398660u, 0x00080000u},
    {4404850u, 0x00080000u},
    {4411040u, 0x00080000u},
    {4417230u, 0x00080000u},
    {4423420u, 0x00080000u},
    {4429610u, 0x00080000u},
    {4435800u, 0x00080000u},
    {4441990u, 0x00080000u},
    {4448180u, 0x00080000u},
    {4454370u, 0x00080000u},
    {4460560u, 0x00080000u},
    {4466750u, 0x00080000u},
    {4472940u, 0x00080000u},
    {4479130u, 0x00080000u},
    {4485335u, 0x00080000u},
    {4491525u, 0x00080000u},
    {4497715u, 0x00080000u},
    {4503905u, 0x00080000u},
    {4510095u, 0x00080000u},
    {4516285u, 0x00080000u},
    {4522475u, 0x00080000u},
    {4528665u, 0x00080000u},
    {4534855u, 0x00080000u},
    {4541045u, 0x00080000u},
    {4547235u, 0x00080000u},
    {4553425u, 0x00080000u},
    {4559650u, 0x00080000u},
    {4565840u, 0x00080000u},
    {4572030u, 0x00080000u},
    {4578220u, 0x00080000u},
    {4584410u, 0x00080000u},
    {4590600u, 0x00080000u},
    {4596790u, 0x00080000u},
    {4602980u, 0x00080000u},
    {4609170u, 0x00080000u},
    {4615360u, 0x00080000u},
    {4621550u, 0x00080000u},
    {4627740u, 0x00080000u},
    {4633930u, 0x00080000u},
    {4640120u, 0x00080000u},
    {4646310u, 0x00080000u},
    {4652500u, 0x00080000u},
    {4658690u, 0x00080000u},
    {4664880u, 0x00080000u},
    {4671070u, 0x00080000u},
    {4677260u, 0x00080000u},
    {4683450u, 0x00080000u},
    {4689640u, 0x00080000u},
    {4695830u, 0x00080000u},
    {4702020u, 0x00080000u},
    {4708210u, 0x00080000u},
    {4714400u, 0x00080000u},
    {4720590u, 0x00080000u},
    {4726780u, 0x00080000u},
    {4732970u, 0x00080000u},
    {4739160u, 0x00080000u},
    {4745350u, 0x00080000u},
    {4751540u, 0x00080000u},
    {4757730u, 0x00080000u},
    {4763920u, 0x00080000u},
    {4770110u, 0x00080000u},
    {4776300u, 0x00080000u},
    {4782490u, 0x00080000u},
    {4788680u, 0x00080000u},
    {4794870u, 0x00080000u},
    {4801060u, 0x00080000u},
    {4807250u, 0x00080000u},
    {4813440u, 0x00080000u},
    {4819630u, 0x00080000u},
    {4825820u, 0x00080000u},
    {4832010u, 0x00080000u},
    {4838200u, 0x00080000u},
    {4844390u, 0x00080000u},
    {4850580u, 0x00080000u},
    {4856770u, 0x00080000u},
    {4862995u, 0x00080000u},
    {4869185u, 0x00080000u},
    {4875375u, 0x00080000u},
    {4881565u, 0x00080000u},
    {4887755u, 0x00080000u},
    {4893945u, 0x00080000u},
    {4900135u, 0x00080000u},
    {4906325u, 0x00080000u},
    {4912515u, 0x00080000u},
    {4918705u, 0x00080000u},
    {4924895u, 0x00080000u},
    {4931085u, 0x00080000u},
    {4937275u, 0x00080000u},
    {4943465u, 0x00080000u},
    {4949655u, 0x00080000u},
    {4955845u, 0x00080000u},
    {4962035u, 0x00080000u},
    {4968225u, 0x00080000u},
    {4974415u, 0x00080000u},
    {4980605u, 0x00080000u},
    {4986795u, 0x00080000u},
    {4992985u, 0x00080000u},
    {4999175u, 0x00080000u},
    {5005365u, 0x00080000u},
    {5011555u, 0x00080000u},
    {5017745u, 0x00080000u},
    {5023935u, 0x00080000u},
    {5030125u, 0x00080000u},
    {5036365u, 0x00080000u},
    {5042555u, 0x00080000u},
    {5048745u, 0x00080000u},
    {5054935u, 0x00080000u},
    {5061125u, 0x00080000u},
    {5067315u, 0x00080000u},
    {5073505u, 0x00080000u},
    {5079695u, 0x00080000u},
    {5085885u, 0x00080000u},
    {5092075u, 0x00080000u},
    {5098265u, 0x00080000u},
    {5104455u, 0x00080000u},
    {5110645u, 0x00080000u},
    {5116835u, 0x00080000u},
    {5123025u, 0x00080000u},
    {5129215u, 0x00080000u},
    {5135465u, 0x00080000u},
    {5141655u, 0x00080000u},
    {5147845u, 0x00080000u},
    {5154035u, 0x00080000u},
    {5160225u, 0x00080000u},
    {5166425u, 0x00080000u},
    {5172615u, 0x00080000u},
    {5178805u, 0x00080000u},
    {5184995u, 0x00080000u},
    {5191185u, 0x00080000u},
    {5197375u, 0x00080000u},
    {5203565u, 0x00080000u},
    {5209755u, 0x00080000u},
    {5215945u, 0x00080000u},
    {5222135u, 0x00080000u},
    {5228325u, 0x00080000u},
    {5234550u, 0x00080000u},
    {5240780u, 0x00080000u},
    {5246970u, 0x00080000u},
    {5253160u, 0x00080000u},
    {5259350u, 0x00080000u},
    {5265540u, 0x00080000u},
    {5271730u, 0x00080000u},
    {5277920u, 0x00080000u},
    {5284110u, 0x00080000u},
    {5290300u, 0x00080000u},
    {5296490u, 0x00080000u},
    {5302680u, 0x00080000u},
    {5308870u, 0x00080000u},
    {5315060u, 0x00080000u},
    {5321250u, 0x00080000u},
    {5327440u, 0x00080000u},
    {5333655u, 0x00080000u},
    {5339845u, 0x00080000u},
    {5346035u, 0x00080000u},
    {5352225u, 0x00080000u},
    {5358415u, 0x00080000u},
    {5364605u, 0x00080000u},
    {5370795u, 0x00080000u},
    {5376985u, 0x00080000u},
    {5383175u, 0x00080000u},
    {5389365u, 0x00080000u},
    {5395555u, 0x00080000u},
    {5401745u, 0x00080000u},
    {5407935u, 0x00080000u},
    {5414125u, 0x00080000u},
    {5420315u, 0x00080000u},
    {5426505u, 0x00080000u},
    {5432705u, 0x00080000u},
    {5438895u, 0x00080000u},
    {5445085u, 0x00080000u},
    {5451275u, 0x00080000u},
    {5457465u, 0x00080000u},
    {5463655u, 0x00080000u},
    {5469845u, 0x00080000u},
    {5476035u, 0x00080000u},
    {5482225u, 0x00080000u},
    {5488415u, 0x00080000u},
    {5494605u, 0x00080000u},
    {5500795u, 0x00080000u},
    {5506985u, 0x00080000u},
    {5513175u, 0x00080000u},
    {5519365u, 0x00080000u},
    {5525555u, 0x00080000u},
    {5531745u, 0x00080000u},
    {5537935u, 0x00080000u},
    {5544155u, 0x00080000u},
    {5550345u, 0x00080000u},
    {5556535u, 0x00080000u},
    {5562725u, 0x00080000u},
    {5568915u, 0x00080000u},
    {5575105u, 0x00080000u},
    {5581295u, 0x00080000u},
    {5587485u, 0x00080000u},
    {5593675u, 0x00080000u},
    {5599865u, 0x00080000u},
    {5606055u, 0x00080000u},
    {5612245u, 0x00080000u},
    {5618435u, 0x00080000u},
    {5624625u, 0x00080000u},
    {5630815u, 0x00080000u},
    {5637005u, 0x00080000u},
    {5643195u, 0x00080000u},
    {5649385u, 0x00080000u},
    {5655575u, 0x00080000u},
    {5661765u, 0x00080000u},
    {5667955u, 0x00080000u},
    {5674145u, 0x00080000u},
    {5680335u, 0x00080000u},
    {5686525u, 0x00080000u},
    {5692715u, 0x00080000u},
    {5698905u, 0x00080000u},
    {5705095u, 0x00080000u},
    {5711285u, 0x00080000u},
    {5717475u, 0x00080000u},
    {5723665u, 0x00080000u},
    {5729855u, 0x00080000u},
    {5736045u, 0x00080000u},
    {5742235u, 0x00080000u},
    {5748425u, 0x00080000u},
    {5754615u, 0x00080000u},
    {5760805u, 0x00080000u},
    {5766995u, 0x00080000u},
    {5773185u, 0x00080000u},
    {5779375u, 0x00080000u},
    {5785565u, 0x00080000u},
    {5791755u, 0x00080000u},
    {5797945u, 0x00080000u},
    {5804135u, 0x00080000u},
    {5810325u, 0x00080000u},
    {5816515u, 0x00080000u},
    {5822705u, 0x00080000u},
    {5828895u, 0x00080000u},
    {5835085u, 0x00080000u},
    {5841275u, 0x00080000u},
    {5847490u, 0x00080000u},
    {5853680u, 0x00080000u},
    {5859870u, 0x00080000u},
    {5866060u, 0x00080000u},
    {5872250u, 0x00080000u},
    {5878440u, 0x00080000u},
    {5884630u, 0x00080000u},
    {5890820u, 0x00080000u},
    {5897010u, 0x00080000u},
    {5903200u, 0x00080000u},
    {5909390u, 0x00080000u},
    {5915580u, 0x00080000u},
    {5921790u, 0x00080000u},
    {5927980u, 0x00080000u},
    {5934170u, 0x00080000u},
    {5940360u, 0x00080000u},
    {5946550u, 0x00080000u},
    {5952740u, 0x00080000u},
    {5958930u, 0x00080000u},
    {5965120u, 0x00080000u},
    {5971310u, 0x00080000u},
    {5977500u, 0x00080000u},
    {5983690u, 0x00080000u},
    {5989880u, 0x00080000u},
    {5996070u, 0x00080000u},
    {6002260u, 0x00080000u},
    {6008450u, 0x00080000u},
    {6014640u, 0x00080000u},
    {6020830u, 0x00080000u},
    {6027020u, 0x00080000u},
    {6033210u, 0x00080000u},
    {6039400u, 0x00080000u},
    {6045590u, 0x00080000u},
    {6051780u, 0x00080000u},
    {6057970u, 0x00080000u},
    {6064160u, 0x00080000u},
    {6070350u, 0x00080000u},
    {6076540u, 0x00080000u},
    {6082730u, 0x00080000u},
    {6088920u, 0x00080000u},
    {6095110u, 0x00080000u},
    {6101300u, 0x00080000u},
    {6107490u, 0x00080000u},
    {6113680u, 0x00080000u},
    {6119885u, 0x00080000u},
    {6126075u, 0x00080000u},
    {6132265u, 0x00080000u},
    {6138455u, 0x00080000u},
    {6144645u, 0x00080000u},
    {6150835u, 0x00080000u},
    {6157025u, 0x00080000u},
    {6163215u, 0x00080000u},
    {6169405u, 0x00080000u},
    {6175595u, 0x00080000u},
    {6181785u, 0x00080000u},
    {6187975u, 0x00080000u},
    {6194165u, 0x00080000u},
    {6200355u, 0x00080000u},
    {6206545u, 0x00080000u},
    {6212735u, 0x00080000u},
    {6218960u, 0x00080000u},
    {6220579u, 0x00000008u},
    {6225211u, 0x00080000u},
    {6231401u, 0x00080000u},
    {6237591u, 0x00080000u},
    {6243781u, 0x00080000u},
    {6249971u, 0x00080000u},
    {6256161u, 0x00080000u},
    {6262351u, 0x00080000u},
    {6268541u, 0x00080000u},
    {6274731u, 0x00080000u},
    {6280921u, 0x00080000u},
    {6287111u, 0x00080000u},
    {6293301u, 0x00080000u},
    {6299491u, 0x00080000u},
    {6305681u, 0x00080000u},
    {6311871u, 0x00080000u},
    {6318101u, 0x00080000u},
    {6324291u, 0x00080000u},
    {6330481u, 0x00080000u},
    {6336671u, 0x00080000u},
    {6342861u, 0x00080000u},
    {6349051u, 0x00080000u},
    {6355241u, 0x00080000u},
    {6361431u, 0x00080000u},
    {6367621u, 0x00080000u},
    {6373811u, 0x00080000u},
    {6380001u, 0x00080000u},
    {6386191u, 0x00080000u},
    {6392381u, 0x00080000u},
    {6398571u, 0x00080000u},
    {6404761u, 0x00080000u},
    {6410951u, 0x00080000u},
    {6417216u, 0x00080000u},
    {6423406u, 0x00080000u},
    {6429596u, 0x00080000u},
    {6435786u, 0x00080000u},
    {6441976u, 0x00080000u},
    {6448166u, 0x00080000u},
    {6454356u, 0x00080000u},
    {6460546u, 0x00080000u},
    {6466736u, 0x00080000u},
    {6472926u, 0x00080000u},
    {6479116u, 0x00080000u},
    {6485306u, 0x00080000u},
    {6491496u, 0x00080000u},
    {6497686u, 0x00080000u},
    {6503876u, 0x00080000u},
    {6510066u, 0x00080000u},
    {6516296u, 0x00080000u},
    {6522486u, 0x00080000u},
    {6528696u, 0x00080000u},
    {6534886u, 0x00080000u},
    {6541076u, 0x00080000u},
    {6547266u, 0x00080000u},
    {6553456u, 0x00080000u},
    {6559646u, 0x00080000u},
    {6565836u, 0x00080000u},
    {6572026u, 0x00080000u},
    {6578216u, 0x00080000u},
    {6584406u, 0x00080000u},
    {6590596u, 0x00080000u},
    {6596786u, 0x00080000u},
    {6603001u, 0x00080000u},
    {6609191u, 0x00080000u},
    {6615416u, 0x00080000u},
    {6621606u, 0x00080000u},
    {6627796u, 0x00080000u},
    {6633986u, 0x00080000u},
    {6640176u, 0x00080000u},
    {6646366u, 0x00080000u},
    {6652556u, 0x00080000u},
    {6658746u, 0x00080000u},
    {6664936u, 0x00080000u},
    {6671126u, 0x00080000u},
    {6677316u, 0x00080000u},
    {6683506u, 0x00080000u},
    {6689696u, 0x00080000u},
    {6695886u, 0x00080000u},
    {6702076u, 0x00080000u},
    {6708266u, 0x00080000u},
    {6714456u, 0x00080000u},
    {6720646u, 0x00080000u},
    {6726836u, 0x00080000u},
    {6733026u, 0x00080000u},
    {6739216u, 0x00080000u},
    {6745406u, 0x00080000u},
    {6751596u, 0x00080000u},
    {6757786u, 0x00080000u},
    {6763976u, 0x00080000u},
    {6770166u, 0x00080000u},
    {6776356u, 0x00080000u},
    {6782546u, 0x00080000u},
    {6788736u, 0x00080000u},
    {6794926u, 0x00080000u},
    {6801116u, 0x00080000u},
    {6807306u, 0x00080000u},
    {6813496u, 0x00080000u},
    {6819686u, 0x00080000u},
    {6825876u, 0x00080000u},
    {6832066u, 0x00080000u},
    {6838256u, 0x00080000u},
    {6844446u, 0x00080000u},
    {6850636u, 0x00080000u},
    {6856826u, 0x00080000u},
    {6863016u, 0x00080000u},
    {6869206u, 0x00080000u},
    {6875396u, 0x00080000u},
    {6881586u, 0x00080000u},
    {6887776u, 0x00080000u},
    {6893966u, 0x00080000u},
    {6900156u, 0x00080000u},
    {6906371u, 0x00080000u},
    {6912561u, 0x00080000u},
    {6918751u, 0x00080000u},
    {6924941u, 0x00080000u},
    {6931131u, 0x00080000u},
    {6937321u, 0x00080000u},
    {6943511u, 0x00080000u},
    {6949701u, 0x00080000u},
    {6955891u, 0x00080000u},
    {6962081u, 0x00080000u},
    {6968271u, 0x00080000u},
    {6974461u, 0x00080000u},
    {6980651u, 0x00080000u},
    {6986841u, 0x00080000u},
    {6993031u, 0x00080000u},
    {6999221u, 0x00080000u},
    {7005411u, 0x00080000u},
    {7011601u, 0x00080000u},
    {7017791u, 0x00080000u},
    {7023981u, 0x00080000u},
    {7030171u, 0x00080000u},
    {7036361u, 0x00080000u},
    {7042551u, 0x00080000u},
    {7048741u, 0x00080000u},
    {7054931u, 0x00080000u},
    {7061121u, 0x00080000u},
    {7067311u, 0x00080000u},
    {7073501u, 0x00080000u},
    {7079691u, 0x00080000u},
    {7085881u, 0x00080000u},
    {7092071u, 0x00080000u},
    {7098261u, 0x00080000u},
    {7104451u, 0x00080000u},
    {7110641u, 0x00080000u},
    {7116831u, 0x00080000u},
    {7123021u, 0x00080000u},
    {7129211u, 0x00080000u},
    {7135401u, 0x00080000u},
    {7141591u, 0x00080000u},
    {7147781u, 0x00080000u},
    {7153971u, 0x00080000u},
    {7160161u, 0x00080000u},
    {7166351u, 0x00080000u},
    {7172541u, 0x00080000u},
    {7178731u, 0x00080000u},
    {7184921u, 0x00080000u},
    {7191111u, 0x00080000u},
    {7197301u, 0x00080000u},
    {7203491u, 0x00080000u},
    {7209711u, 0x00080000u},
    {7215901u, 0x00080000u},
    {7222091u, 0x00080000u},
    {7228281u, 0x00080000u},
    {7234471u, 0x00080000u},
    {7240676u, 0x00080000u},
    {7246866u, 0x00080000u},
    {7253056u, 0x00080000u},
    {7259246u, 0x00080000u},
    {7265436u, 0x00080000u},
    {7271626u, 0x00080000u},
    {7277816u, 0x00080000u},
    {7284026u, 0x00080000u},
    {7290216u, 0x00080000u},
    {7296406u, 0x00080000u},
    {7302596u, 0x00080000u},
    {7308786u, 0x00080000u},
    {7314976u, 0x00080000u},
    {7321166u, 0x00080000u},
    {7327356u, 0x00080000u},
    {7333546u, 0x00080000u},
    {7339736u, 0x00080000u},
    {7345941u, 0x00080000u},
    {7352131u, 0x00080000u},
    {7358321u, 0x00080000u},
    {7364511u, 0x00080000u},
    {7370701u, 0x00080000u},
    {7376891u, 0x00080000u},
    {7383081u, 0x00080000u},
    {7389271u, 0x00080000u},
    {7395461u, 0x00080000u},
    {7401671u, 0x00080000u},
    {7407861u, 0x00080000u},
    {7414051u, 0x00080000u},
    {7420241u, 0x00080000u},
    {7426431u, 0x00080000u},
    {7432621u, 0x00080000u},
    {7438811u, 0x00080000u},
    {7445001u, 0x00080000u},
    {7451191u, 0x00080000u},
    {7457381u, 0x00080000u},
    {7463571u, 0x00080000u},
    {7469761u, 0x00080000u},
    {7475951u, 0x00080000u},
    {7482141u, 0x00080000u},
    {7488331u, 0x00080000u},
    {7494521u, 0x00080000u},
    {7500756u, 0x00080000u},
    {7506946u, 0x00080000u},
    {7513136u, 0x00080000u},
    {7519326u, 0x00080000u},
    {7525516u, 0x00080000u},
    {7531706u, 0x00080000u},
    {7537896u, 0x00080000u},
    {7544086u, 0x00080000u},
    {7550276u, 0x00080000u},
    {7556486u, 0x00080000u},
    {7562676u, 0x00080000u},
    {7568866u, 0x00080000u},
    {7575056u, 0x00080000u},
    {7581246u, 0x00080000u},
    {7587461u, 0x00080000u},
    {7593651u, 0x00080000u},
    {7599896u, 0x00080000u},
    {7606086u, 0x00080000u},
    {7612276u, 0x00080000u},
    {7618466u, 0x00080000u},
    {7624656u, 0x00080000u},
    {7630846u, 0x00080000u},
    {7637036u, 0x00080000u},
    {7643226u, 0x00080000u},
    {7649416u, 0x00080000u},
    {7655606u, 0x00080000u},
    {7661796u, 0x00080000u},
    {7667986u, 0x00080000u},
    {7674176u, 0x00080000u},
    {7680366u, 0x00080000u},
    {7686556u, 0x00080000u},
    {7692746u, 0x00080000u},
    {7698976u, 0x00080000u},
    {7705166u, 0x00080000u},
    {7711356u, 0x00080000u},
    {7717546u, 0x00080000u},
    {7723736u, 0x00080000u},
    {7729926u, 0x00080000u},
    {7736116u, 0x00080000u},
    {7742306u, 0x00080000u},
    {7748496u, 0x00080000u},
    {7754686u, 0x00080000u},
    {7760876u, 0x00080000u},
    {7767066u, 0x00080000u},
    {7773256u, 0x00080000u},
    {7779446u, 0x00080000u},
    {7785638u, 0x00080000u},
    {7785963u, 0x00080000u},
    {7786056u, 0x00080000u},
    {9097800u, 0x00000008u},
};
#define IRQ_RAISE_N ((int)(sizeof irq_raise_log / sizeof irq_raise_log[0]))
static const uint32_t irq_trig_log[] = { 3635491u, 6220580u, 9097801u };
#define IRQ_TRIG_N ((int)(sizeof irq_trig_log / sizeof irq_trig_log[0]))

static void irq_tick(void) {
    while (irq_raise_n < IRQ_RAISE_N && steps > irq_raise_log[irq_raise_n].step) {
        iflags |= irq_raise_log[irq_raise_n].mask;
        irq_raise_n++;
    }
    /* ARM7 IPC-status halfword (Phase-4C): the ARM7 writes 1 to the shared
     * main-RAM word at 0x02FFFC40 (main-RAM offset 0x3FFC40 in melonDS's
     * 4 MiB-mirrored view; the native flat window has no mirror, so the
     * literal address is used — the ARM9's only readers load the same
     * literal, 0x02008DF0 / 0x02008EA8) while servicing the IPC FIFO
     * request.  The exact write moment is NOT yet captured; it is bracketed
     * by measurement: the ARM9's first read (call 0x02008DD8 at trace index
     * ~3632991, before the FIFO IRQ) saw 0 in BOTH melonDS and native, and
     * the second read (index 3636869) saw 1 in melonDS.  Scripted at the
     * FIFO-IRQ delivery boundary (steps > 3635491), inside the bracket. */
    if (!arm7_ipcstat_done && steps > 3635491u) {
        st16(0x02FFFC40u, 1u);
        arm7_ipcstat_done = 1;
        fprintf(stderr, "arm7 ipcstat step=%llu [0x02FFFC40]=1\n", (unsigned long long)steps);
    }
}
static void irq_enter(uint32_t *pc) {
    uint32_t vec = ld32(((cp15_dtcm_setting & ~0xFFu) + 0x4000u) - 4u);
    if (vec < ITCM_LO || vec >= ITCM_HI) {
        halt_reason("IRQ user vector is not the ITCM stub");
        fprintf(stderr, "irq enter step=%llu: user vector 0x%08X rejected\n",
                (unsigned long long)steps, vec);
        return;
    }
    uint32_t resume = *pc;
    /* Exception entry: SPSR_irq = the interrupted CPSR, then switch to IRQ
     * mode (I+F masked) which swaps in the IRQ bank of r13/r14.  The guest
     * sets that bank itself (crt0 at 0x02000834..0x02000840 computes
     * 0x027E3F7C), so no seed is applied here. */
    spsr_bank[MODE_IRQ] = (n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x10u | cpsr_ctl;
    cpsr_set_ctl(0xD2u);
    {
        uint32_t sp = r[13] - 24u;      /* BIOS frame: r0-r3, r12, lr */
        st32(sp, r[0]); st32(sp + 4u, r[1]); st32(sp + 8u, r[2]);
        st32(sp + 12u, r[3]); st32(sp + 16u, r[12]); st32(sp + 20u, resume + 4u);
        r[13] = sp;
    }
    r[14] = BIOS_IRQ_RET;
    irq_active = 1;
    cycles += 12u;                     /* BIOS entry: honest small charge */
    fprintf(stderr, "irq enter step=%llu resume=0x%08X vector=0x%08X sp_irq=0x%08X spsr=0x%08X ime=%u ie=0x%08X if=0x%08X\n",
            (unsigned long long)steps, resume, vec, r[13], spsr_bank[MODE_IRQ], ime, ie, iflags);
    *pc = vec;
}
static void bios_irq_return(uint32_t *pc) {
    uint32_t sp = r[13];
    r[0] = ld32(sp); r[1] = ld32(sp + 4u); r[2] = ld32(sp + 8u);
    r[3] = ld32(sp + 12u); r[12] = ld32(sp + 16u);
    uint32_t lr = ld32(sp + 20u);
    r[13] = sp + 24u;
    {
        uint32_t sr = spsr_bank[MODE_IRQ];
        n = (sr >> 31) & 1u; z = (sr >> 30) & 1u;
        c = (sr >> 29) & 1u; v = (sr >> 28) & 1u;
        cpsr_set_ctl(sr & 0xFFu);       /* back to the interrupted mode+bank */
    }
    irq_active = 0;
    *pc = lr - 4u;
    cycles += 4u;                      /* BIOS epilogue */
    fprintf(stderr, "irq return step=%llu pc=0x%08X cpsr_ctl=0x%08X mode=%d sp=0x%08X\n",
            (unsigned long long)steps, *pc, cpsr_ctl, mode_cur, r[13]);
}

/* Barrel shifter: value, kind (0 lsl / 1 lsr / 2 asr / 3 ror|rrx), amount,
 * carry-in.  Carry-out is published in `carry_out`. */
static uint32_t sh_op(uint32_t v, unsigned kind, unsigned amt, uint32_t carry) {
    switch (kind) {
    case 0: /* LSL */
        if (amt == 0) { carry_out = carry; return v; }
        if (amt < 32) { carry_out = (v >> (32 - amt)) & 1u; return v << amt; }
        if (amt == 32) { carry_out = v & 1u; return 0; }
        carry_out = 0; return 0;
    case 1: /* LSR */
        if (amt == 0) { carry_out = (v >> 31) & 1u; return 0; }
        if (amt < 32) { carry_out = (v >> (amt - 1)) & 1u; return v >> amt; }
        carry_out = (amt == 32) ? ((v >> 31) & 1u) : 0u;
        return 0;
    case 2: /* ASR */
        if (amt == 0) amt = 32;
        if (amt >= 32) { carry_out = (v >> 31) & 1u; return (v >> 31) ? 0xFFFFFFFFu : 0u; }
        carry_out = (v >> (amt - 1)) & 1u;
        return (uint32_t)(((int32_t)v) >> amt);
    default: /* ROR / RRX */
        if (amt == 0) { uint32_t res = (carry << 31) | (v >> 1); carry_out = v & 1u; return res; }
        amt &= 31;
        if (amt == 0) { carry_out = (v >> 31) & 1u; return v; }
        carry_out = (v >> (amt - 1)) & 1u;
        return (v >> amt) | (v << (32 - amt));
    }
}

/* Shifter with a REGISTER-specified amount (ARM `mov rd, rm, lsl rs`, Thumb
 * format-4 `lsls/lsrs/asrs/rors rd, rs`).  The architectural difference from
 * the immediate forms: an amount of 0 performs NO shift, leaving the result
 * AND the carry flag unchanged (ARM ARM A4.1.2).  `sh_op`'s immediate-0 path
 * means LSR #32 / RRX instead, so by-register shifts must come here — using
 * `sh_op` for them zeroed the destination whenever the shift register held 0.
 * Amounts above 255 are UNPREDICTABLE; the emitted code masks to 8 bits. */
static uint32_t sh_op_reg(uint32_t v, unsigned kind, unsigned amt, uint32_t carry) {
    amt &= 255u;
    if (amt == 0) { carry_out = carry; return v; }
    switch (kind) {
    case 0: /* LSL */
        if (amt < 32) { carry_out = (v >> (32 - amt)) & 1u; return v << amt; }
        if (amt == 32) { carry_out = v & 1u; return 0; }
        carry_out = 0; return 0;
    case 1: /* LSR */
        if (amt < 32) { carry_out = (v >> (amt - 1)) & 1u; return v >> amt; }
        carry_out = (amt == 32) ? ((v >> 31) & 1u) : 0u;
        return 0;
    case 2: /* ASR */
        if (amt >= 32) { carry_out = (v >> 31) & 1u; return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u; }
        carry_out = (v >> (amt - 1)) & 1u;
        return (uint32_t)(((int32_t)v) >> amt);
    default: { /* ROR */
        unsigned n = amt & 31u;
        if (n == 0) { carry_out = (v >> 31) & 1u; return v; }
        carry_out = (v >> (n - 1)) & 1u;
        return (v >> n) | (v << (32 - n));
    }
    }
}

static inline uint32_t clz32(uint32_t x) {
    uint32_t k = 0;
    if (x == 0) return 32;
    while ((x & 0x80000000u) == 0) { x <<= 1; k++; }
    return k;
}

/* Full per-execution guest-PC trace: one "0x%08x" line per dispatched
 * instruction (loops repeat), plus a distinct-PC bitmap for the summary. */
/* Checkpoint dump: NNK_DUMP_AT_PC=<hex> + NNK_CHECKPOINT_MEM=<path> dump the
 * guest window the FIRST time the dispatch loop reaches that PC.  Used to
 * capture memory state at a phase boundary (e.g. the auto-load walker entry
 * 0x020009FC, right after the in-place decompressor completes and before the
 * walker's bss sweep can overwrite it). */
static uint32_t ckpt_pc;
static int ckpt_done;
static void checkpoint_dump(uint32_t pc) {
    if (!ckpt_pc || ckpt_done || pc != ckpt_pc) return;
    ckpt_done = 1;
    const char *p = getenv("NNK_CHECKPOINT_MEM");
    if (!p || !p[0]) return;
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(gmem, 1, GSIZE, f); fclose(f);
             fprintf(stderr, "checkpoint dump at pc=0x%08X -> %s\n", pc, p); }
    else fprintf(stderr, "NNK_CHECKPOINT_MEM: cannot open %s\n", p);
}

/* NNK_DEBUG_AT_PC=<hex> [+ NNK_DEBUG_N=<count, default 64>] dumps all guest
 * registers + flags to stderr each time the dispatch loop reaches that PC —
 * a poor man's breakpoint for arbitrating emulator-vs-emulator divergences
 * (e.g. the LZ decompressor flag stream at 0x0200098C).
 * Phase-4F: the line also carries the step index, NNK_DEBUG_MEM="a:len[,a:len]"
 * + NNK_DEBUG_MEM_PATH=<file> appends a hexdump of each range on every hit
 * (read through the guest data path, so DTCM ranges show the guest's view),
 * and NNK_HALT_AT_PC=<hex> stops the run honestly at that PC. */
static uint32_t dbg_pc;
static uint32_t halt_at_pc;
static int dbg_hits;
static int dbg_limit = 64;
static void debug_mem_dump(void) {
    const char *spec = getenv("NNK_DEBUG_MEM");
    const char *path = getenv("NNK_DEBUG_MEM_PATH");
    if (!spec || !spec[0] || !path || !path[0]) return;
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "hit %d pc=0x%08X step=%llu\n", dbg_hits, dbg_pc, (unsigned long long)steps);
    const char *q = spec;
    int n = 0;
    while (*q && n < 4) {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        uint32_t a = (uint32_t)strtoul(q, (char **)&q, 0);
        while (*q == ' ' || *q == ':') q++;
        uint32_t len = (uint32_t)strtoul(q, (char **)&q, 0);
        if (!len) len = 0x100;
        fprintf(f, "range 0x%08X len 0x%X\n", a, len);
        for (uint32_t off = 0; off < len; off += 16) {
            fprintf(f, "0x%08X:", a + off);
            for (uint32_t i = 0; i < 16 && off + i < len; i++) {
                uint32_t v = ld16(a + off + i);
                fprintf(f, " %02X", (unsigned)(v & 0xFFu));
            }
            fputc('\n', f);
        }
        n++;
    }
    fclose(f);
}
static void debug_dump(uint32_t pc) {
    if (!dbg_pc || pc != dbg_pc || dbg_hits >= dbg_limit) return;
    dbg_hits++;
    fprintf(stderr, "dbg%d step=%llu pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r12=%08X r13=%08X r14=%08X nzcv=%d%d%d%d\n",
            dbg_hits, (unsigned long long)steps, pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[12], r[13], r[14],
            (int)n, (int)z, (int)c, (int)v);
    debug_mem_dump();
}

/* First-execution ITCM snapshot (Phase-4B): the loader's copy into ITCM is
 * what the pre-translated ITCM cases execute, so the runtime must prove the
 * bytes it holds are the bytes melonDS executed.  Logged unconditionally;
 * NNK_ITCM_DUMP=<path> writes the 32 KiB array at that moment for a
 * byte-compare against the melonDS MELONDS_ITCM_DUMP capture. */
static int itcm_dumped;
static void itcm_dump_once(uint32_t pc) {
    if (itcm_dumped || pc < ITCM_LO || pc >= ITCM_HI) return;
    itcm_dumped = 1;
    fprintf(stderr, "itcm: first dispatch pc=0x%08X itcm_size=0x%08X\n", pc, itcm_size);
    const char *p = getenv("NNK_ITCM_DUMP");
    if (!p || !p[0]) return;
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(itcm, 1, ITCM_PHYS, f); fclose(f);
             fprintf(stderr, "itcm: dumped to %s\n", p); }
    else fprintf(stderr, "itcm: cannot open %s\n", p);
}

static void trace_pc(uint32_t pc) {
    last_pc = pc;
    cycles++;                       /* coarse timing: ~1 cycle per instruction */
    if (halt_at_pc && pc == halt_at_pc) { halt_reason("halt-at-pc"); step_limit = steps; return; }
    if (nnk_display_tick()) {
#ifdef NNK_GX
        fprintf(stderr, "nnk: quit path at step=%llu gxd=%p frames=%u should_quit=%d\n",
                (unsigned long long)steps, (void *)gxd, gxd_frames,
                gxd ? gxd_should_quit(gxd) : -1);
#endif
        halt_reason("SDL window closed"); step_limit = steps;
    }
    checkpoint_dump(pc);
    debug_dump(pc);
    itcm_dump_once(pc);
    if (steps >= trace_from && traced < TRACE_LIMIT + trace_from) printf("0x%08x\n", pc);
    traced++;
    if (pc >= IMAGE_LOAD) {
        uint32_t idx = (pc - IMAGE_LOAD) >> 1;
        if (idx < IMAGE_LEN / 2u && (seen[idx >> 3] & (1u << (idx & 7))) == 0) {
            seen[idx >> 3] |= (uint8_t)(1u << (idx & 7));
            distinct_pcs++;
        }
    }
}

static void halt_reason(const char *msg) { halt_msg = msg; }

/* ---- SWI / BIOS HLE ------------------------------------------------------
 * Number convention verified against melonDS's FreeBIOS
 * (freebios/src/bios_common.s, swi_handler):
 *     ldrb r12, [lr, #-2]
 * reads bits 16..23 of the 24-bit SWI immediate (numbers >= 0x20 are
 * remapped to the invalid slot).  The low byte (GBA-style "svc 0x0000NN")
 * is logged as "lo=" but never dispatched on, so an encoding difference from
 * the reference BIOS stays visible in the log instead of silently diverging.
 * Every hit is logged with its argument registers BEFORE semantics run; an
 * unimplemented number HALTS, so no SWI ever looks successful without its
 * real effects.  Returns 1 = executed, 0 = halt.
 *
 * `thumb` selects where the number is read from the instruction: FreeBIOS
 * does `ldrb r12, [lr, #-2]`, which is bits 16..23 of an ARM comment field
 * (lr = svc + 4) but the whole `svc #imm8` of a Thumb instruction (lr = the
 * next halfword).  A Thumb SWI therefore dispatches on bits 7..0.
 *
 * Cycle accounting: melonDS executes the real FreeBIOS handler (exception
 * entry + ~25 handler instructions + return), so HLE must charge those
 * cycles or VCOUNT/DISPSTAT-based polling runs at the wrong wall-clock rate.
 * The constants below are calibrated against the melonDS ground-truth
 * poll-loop iteration counts (documented in IO_MAP.md). */
#ifndef SWI_OVERHEAD_CYCLES
#define SWI_OVERHEAD_CYCLES 40u
#endif
#ifndef WAITBYLOOP_CYCLES_PER_ITER
#define WAITBYLOOP_CYCLES_PER_ITER 3u
#endif
static int hle_swi(uint32_t imm24, uint32_t at, int thumb) {
    uint32_t hi = thumb ? (imm24 & 0xFFu) : ((imm24 >> 16) & 0xFFu);
    uint32_t lo = imm24 & 0xFFu;
    hle_hits++;
    cycles += SWI_OVERHEAD_CYCLES;
    fprintf(stderr,
            "hle swi imm24=0x%06X num=0x%02X lo=0x%02X st=%s at=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
            imm24, hi, lo, thumb ? "thumb" : "arm", at, r[0], r[1], r[2], r[3]);
    switch (hi) {
    case 0x03: { /* WaitByLoop (FreeBIOS bios_common.s swi_wait_by_loop):
                 *   0: subs r0, r0, #1 ; bgt 0b ; b swi_complete
                 * r0 iterations when r0 > 0 (leaves r0 = 0), otherwise a
                 * single iteration leaving r0 = start - 1; the flags are the
                 * ones the final `subs` leaves behind. */
        uint32_t start = r[0];
        int positive = (int32_t)start > 0;
        uint32_t iters = positive ? start : 1u;
        uint32_t fin = positive ? 0u : start - 1u;
        uint32_t sub_a = positive ? 1u : start;
        uint64_t diff = (uint64_t)sub_a - 1u;
        int64_t sdiff = (int64_t)(int32_t)sub_a - 1;
        r[0] = fin;
        z = (fin == 0); n = (fin >> 31) & 1;
        c = (diff >> 32) ? 0u : 1u;
        v = (sdiff > (int64_t)0x7FFFFFFF || sdiff < -(int64_t)0x80000000) ? 1u : 0u;
        cycles += (uint64_t)WAITBYLOOP_CYCLES_PER_ITER * iters;
        fprintf(stderr, "hle   WaitByLoop r0=0x%08X -> 0x%08X iters=%u\n", start, fin, iters);
        return 1;
    }
    case 0x09: { /* Div (FreeBIOS swi_divide): signed quotient, magnitude
                 * remainder, |quotient| */
        uint32_t a = r[0], b = r[1];
        if (b == 0) { halt_reason("SWI 09 Div: divisor 0 (FreeBIOS behaviour not modelled)"); return 0; }
        uint32_t an = (a & 0x80000000u) ? (0u - a) : a;
        uint32_t bn = (b & 0x80000000u) ? (0u - b) : b;
        uint32_t q = an / bn;
        r[0] = ((a ^ b) & 0x80000000u) ? (0u - q) : q;
        r[1] = an % bn;
        r[3] = q;
        fprintf(stderr, "hle   Div -> r0=0x%08X r1=0x%08X r3=0x%08X\n", r[0], r[1], r[3]);
        return 1;
    }
    case 0x0D: { /* Sqrt (FreeBIOS swi_sqrt): integer square root of r0 */
        uint32_t x = r[0], res = 0, bit = 1u << 30;
        while (bit > x) bit >>= 2;
        while (bit) {
            if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
            else res >>= 1;
            bit >>= 2;
        }
        r[0] = res;
        fprintf(stderr, "hle   Sqrt -> r0=0x%08X\n", r[0]);
        return 1;
    }
    case 0x0B: { /* CpuSet (FreeBIOS swi_cpu_set): r2 bits 0..20 = count,
                 * bit26 = word, bit24 = fill (fixed source) */
        uint32_t len = r[2] & 0x1FFFFFu;
        uint32_t src = r[0], dst = r[1];
        int word = (int)((r[2] >> 26) & 1u), fill = (int)((r[2] >> 24) & 1u);
        if (word) {
            src &= ~3u; dst &= ~3u;
            if (fill) { uint32_t v = ld32(src); while (len--) { st32(dst, v); dst += 4; } }
            else      { while (len--) { st32(dst, ld32(src)); src += 4; dst += 4; } }
        } else {
            src &= ~1u; dst &= ~1u;
            if (fill) { uint32_t v = ld16(src); while (len--) { st16(dst, (uint16_t)v); dst += 2; } }
            else      { while (len--) { st16(dst, (uint16_t)ld16(src)); src += 2; dst += 2; } }
        }
        fprintf(stderr, "hle   CpuSet len=%u word=%d fill=%d\n", r[2] & 0x1FFFFFu, word, fill);
        return 1;
    }
    case 0x0C: { /* CpuFastSet (FreeBIOS swi_cpu_fast_set): word only,
                 * bit24 = fill, count used as-is (no rounding to 8) */
        uint32_t len = r[2] & 0x1FFFFFu;
        uint32_t src = r[0] & ~3u, dst = r[1] & ~3u;
        if ((r[2] >> 24) & 1u) { uint32_t v = ld32(src); while (len--) { st32(dst, v); dst += 4; } }
        else { while (len--) { st32(dst, ld32(src)); src += 4; dst += 4; } }
        fprintf(stderr, "hle   CpuFastSet len=%u fill=%d\n", r[2] & 0x1FFFFFu, (int)((r[2] >> 24) & 1u));
        return 1;
    }
    default:
        halt_reason("SWI not implemented in the Phase-2A HLE (see HLE log)");
        return 0;
    }
}

/* Phase-4J: SMC runtime image-state tracking (generation-0 snapshot, per-band
 * generation counters and the runtime re-decoder).  Kept in tools/nnk_smc.inc
 * so the mechanism is reviewable as C instead of Rust string escaping; the
 * build copies it next to the generated TU. */
#include "nnk_smc.inc"

static void guest_init(void) { memset(r, 0, sizeof r); n = z = c = v = 0; cp15_update_tcm(); io_load_rom(); bios_boot_seed(); }
"##;

const RUNTIME_ALL_MAIN: &str = r##"int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "arm9_image.bin";
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open guest image %s\n", path); return 2; }
    size_t got = fread(gmem, 1, IMAGE_LEN, fp);
    fclose(fp);
    if (got != IMAGE_LEN) { fprintf(stderr, "short read: got %zu, want %u\n", got, IMAGE_LEN); return 2; }
    smc_arm();          /* Phase-4J: snapshot the compiled generation */
    guest_init();
    nnk_display_open();
    { const char *s = getenv("NNK_STEP_LIMIT");
      if (s && s[0]) step_limit = strtoull(s, NULL, 0); }
    { const char *s = getenv("NNK_TRACE_FROM");
      if (s && s[0]) trace_from = strtoull(s, NULL, 0); }
    { const char *s = getenv("NNK_MODE_TRACE");
      if (s && s[0]) mode_trace = atoi(s); }
    { const char *s = getenv("NNK_DUMP_AT_PC");
      if (s && s[0]) ckpt_pc = (uint32_t)strtoul(s, NULL, 0); }
    { const char *s = getenv("NNK_DEBUG_AT_PC");
      if (s && s[0]) dbg_pc = (uint32_t)strtoul(s, NULL, 0);
      const char *n2 = getenv("NNK_DEBUG_N");
      if (n2 && n2[0]) dbg_limit = (int)strtol(n2, NULL, 0); }
    { const char *s = getenv("NNK_HALT_AT_PC");
      if (s && s[0]) halt_at_pc = (uint32_t)strtoul(s, NULL, 0); }
    printf("nnk recompile-all ARM9 entry 0x%08X\n", ENTRY_CONST);
    printf("guest image %s: %zu bytes at 0x%08X\n", path, got, IMAGE_LOAD);
    run();
    fprintf(stderr, "halt state: r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r13=%08X r14=%08X nzcv=%d%d%d%d\n",
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[13], r[14], (int)n, (int)z, (int)c, (int)v);
    { const char *dump = getenv("NNK_DUMP_MEM");
      if (dump && dump[0]) {
          FILE *df = fopen(dump, "wb");
          if (df) { fwrite(gmem, 1, GSIZE, df); fclose(df);
                    fprintf(stderr, "dumped guest window (%u bytes) to %s\n", GSIZE, dump); }
          else fprintf(stderr, "NNK_DUMP_MEM: cannot open %s\n", dump); } }
    printf("done steps=%llu traced=%llu distinct_pcs=%llu hle_hits=%llu halt_pc=0x%08X halt=%s\n",
           (unsigned long long)steps, (unsigned long long)traced,
           (unsigned long long)distinct_pcs, (unsigned long long)hle_hits,
           last_pc, halt_msg ? halt_msg : "none");
    fprintf(stderr, "done steps=%llu traced=%llu distinct_pcs=%llu hle_hits=%llu halt_pc=0x%08X halt=%s\n",
            (unsigned long long)steps, (unsigned long long)traced,
            (unsigned long long)distinct_pcs, (unsigned long long)hle_hits,
            last_pc, halt_msg ? halt_msg : "none");
    nnk_display_close();
    return 0;
}
"##;
