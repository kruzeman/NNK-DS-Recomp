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
            let _ = spsr;
            body.push(format!(
                "r[{}] = (n << 31) | (z << 30) | (c << 29) | (v << 28) | 0x10u | cpsr_ctl;",
                rd
            ));
        }
        Kind::Msr { spsr, mask, src } => {
            let _ = spsr;
            let (opnd, _) = operand_c(src, addr, thumb);
            body.push(format!("{{ uint32_t _v = {};", opnd));
            if mask & 1 != 0 {
                body.push("cpsr_ctl = _v & 0xFFu;".into());
            }
            if mask & 8 != 0 {
                body.push(
                    "n = (_v >> 31) & 1; z = (_v >> 30) & 1; c = (_v >> 29) & 1; v = (_v >> 28) & 1;"
                        .into(),
                );
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
                    // dispatch key is the even address either way.
                    body.push(format!("uint32_t _t = {};", addr_expr));
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
            // Lowest transfer address per addressing mode:
            // DA: base-4n+4, IA: base, DB: base-4n, IB: base+4.
            let start = match pu {
                0 => format!("(_base - {}u + 4u)", cnt * 4),
                1 => "_base".to_string(),
                2 => format!("(_base - {}u)", cnt * 4),
                _ => "(_base + 4u)".to_string(),
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
                    } else {
                        body.push(format!("r[{}] = ld32(_a + {}u);", rr, idx * 4));
                    }
                } else {
                    body.push(format!("st32(_a + {}u, {});", idx * 4, if thumb { reg_c_t(*rr, addr) } else { reg_c(*rr, addr) }));
                }
                idx += 1;
            }
            if *wb && !regs.is_empty() {
                let sign = if *pu == 1 || *pu == 3 { "+" } else { "-" };
                body.push(format!("r[{}] = _base {} {}u;", rn, sign, cnt * 4));
            }
            let _ = s; // S-bit: single-bank register model, user bank == current
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
        Kind::Mcr { load, rd, .. } => {
            if *load {
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

    EmitOutcome { body, flow, guarded, cond: ce, pc_written, shimmed, halts }
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
        "static void run(void) {\n    uint32_t pc = ENTRY_CONST;\n    for (;;) {\n        if (++steps > STEP_LIMIT) { halt_reason(\"step budget exhausted\"); return; }\n        arm7_evt_tick();\n        switch (pc) {\n",
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
static uint32_t r[16];
static uint32_t n, z, c, v;      /* CPSR NZCV (single-bank model) */
static uint32_t cpsr_ctl = 0x13; /* control byte: mode/IRQ bits */
static uint32_t cp15_reg = 0;    /* CP15 reads return 0 in this model */
static uint32_t carry_out;       /* shifter carry-out of the last sh_op */
static uint64_t steps;
static uint64_t traced;
static uint64_t distinct_pcs;
static uint64_t hle_hits;
static uint32_t last_pc;

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
        if (fa >= GBASE && fa + 4u <= GBASE + GSIZE) {
            uint8_t *p = gmem + (fa - GBASE);
            uint32_t wv = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            wv |= (1u << 12);
            p[0] = (uint8_t)wv; p[1] = (uint8_t)(wv >> 8);
            p[2] = (uint8_t)(wv >> 16); p[3] = (uint8_t)(wv >> 24);
        }
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
        uint8_t *p = gmem + (fa - GBASE);
        p[0] = (uint8_t)wv; p[1] = (uint8_t)(wv >> 8);
        p[2] = (uint8_t)(wv >> 16); p[3] = (uint8_t)(wv >> 24);
        fprintf(stderr, "arm7 evt step=%llu val=0x%08X\n",
                (unsigned long long)steps, wv);
        arm7_evt_n++;
    }
}

/* Card (SPICNT/ROMCNT/CARD_COMMAND/ROM_DATA). */
static uint8_t card_cmd[8];
static int card_busy = 0, card_drq = 0;
static uint32_t card_addr = 0, card_remaining = 0;
static uint8_t *rom = NULL; static size_t rom_len = 0;
static uint32_t spicnt = 0, romcnt = 0;
static int rom_loaded = 0;

/* Timers, DMA, interrupts, display, misc. */
static uint32_t dispstat = 0, dispcnt = 0;
static uint32_t ime = 0, ie = 0, iflags = 0;
static uint32_t tm_reload[4], tm_ctrl[4], tm_val[4];
static uint32_t dma_sad[4], dma_dad[4], dma_cnt[4];
static uint32_t exmemcnt = 0, powerctl = 0; static uint8_t postflg = 1;
static uint32_t div_num_lo = 0, div_num_hi = 0, div_den_lo = 0, div_den_hi = 0;
static uint32_t div_ctrl = 0, div_quot = 0, div_rem = 0;
static uint32_t sqrt_in_lo = 0, sqrt_in_hi = 0, sqrt_ctrl = 0, sqrt_res = 0;
static uint32_t vramcnt[9]; static uint32_t wramcnt_map = 0;

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

static void io_load_rom(void) {
    const char *p = getenv("NNK_ROM_PATH");
    if (!p || !p[0]) { fprintf(stderr, "io: NNK_ROM_PATH unset; card reads return 0\\n"); return; }
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "io: cannot open ROM %s\\n", p); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { rom = malloc((size_t)n); rom_len = fread(rom, 1, (size_t)n, f); }
    fclose(f); rom_loaded = 1;
    fprintf(stderr, "io: card rom %zu bytes from %s\\n", rom_len, p);
}

static uint32_t io_read8(uint32_t a);
static void io_write8(uint32_t a, uint32_t v);
static uint32_t io_read16(uint32_t a);
static void io_write16(uint32_t a, uint32_t v);
static uint32_t io_read32(uint32_t a);
static void io_write32(uint32_t a, uint32_t v);
static void halt_reason(const char *msg);

static inline uint32_t ld32(uint32_t a) {
    if (a >= IO_LO && a < IO_HI) return io_read32(a);
    if (a < GBASE || (uint64_t)a + 4u > (uint64_t)GBASE + GSIZE) return 0;
    const uint8_t *p = gmem + (a - GBASE);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void st32(uint32_t a, uint32_t v) {
    if (a >= IO_LO && a < IO_HI) { io_write32(a, v); return; }
    if (a < GBASE || (uint64_t)a + 4u > (uint64_t)GBASE + GSIZE) return;
    uint8_t *p = gmem + (a - GBASE);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t ld16(uint32_t a) {
    if (a >= IO_LO && a < IO_HI) return io_read16(a);
    if (a < GBASE || (uint64_t)a + 2u > (uint64_t)GBASE + GSIZE) return 0;
    const uint8_t *p = gmem + (a - GBASE);
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline void st16(uint32_t a, uint16_t v) {
    if (a >= IO_LO && a < IO_HI) { io_write16(a, v); return; }
    if (a < GBASE || (uint64_t)a + 2u > (uint64_t)GBASE + GSIZE) return;
    uint8_t *p = gmem + (a - GBASE);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline uint32_t ld8(uint32_t a) {
    if (a >= IO_LO && a < IO_HI) return io_read8(a);
    if (a < GBASE || (uint64_t)a + 1u > (uint64_t)GBASE + GSIZE) return 0;
    return gmem[a - GBASE];
}
static inline void st8(uint32_t a, uint32_t v) {
    if (a >= IO_LO && a < IO_HI) { io_write8(a, v); return; }
    if (a < GBASE || (uint64_t)a + 1u > (uint64_t)GBASE + GSIZE) return;
    gmem[a - GBASE] = (uint8_t)v;
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
    uint32_t timing = (cnt >> 24) & 3u;
    if (timing != 0u) return;                /* vblank/hblank DMA: trigger on demand elsewhere */
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
static void io_card_start(uint32_t val) {
    uint32_t code = (val >> 24) & 7u;
    uint32_t datasize = (code == 7u) ? 4u : (code > 0u ? (0x100u << code) : 0u);
    uint32_t addr = 0;
    if (card_cmd[0] == 0xB7)
        addr = ((uint32_t)card_cmd[1] << 24) | ((uint32_t)card_cmd[2] << 16) |
               ((uint32_t)card_cmd[3] << 8)  | (uint32_t)card_cmd[4];
    else
        fprintf(stderr, "io: card cmd 0x%02X (only 0xB7 modelled)\n", card_cmd[0]);
    card_busy = 1; card_drq = datasize > 0; card_addr = addr; card_remaining = datasize;
    fprintf(stderr, "io: card start cmd=0x%02X addr=0x%08X size=0x%X\n", card_cmd[0], addr, datasize);
}
static uint32_t io_read32(uint32_t a) {
    switch (a) {
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
    case 0x040001A4: return (romcnt & ~0x80000000u) | (card_busy ? 0x80000000u : 0u) | (card_drq ? 0x00800000u : 0u);
    case 0x04100000: if (arm9_recv_n > 0) { uint32_t v = arm9_recv_fifo[0]; for (int k=1;k<arm9_recv_n;k++) arm9_recv_fifo[k-1]=arm9_recv_fifo[k]; arm9_recv_n--; return v; } ipc_fifo_cnt |= 0x4000u; return 0;
    case 0x04100010: {
        if (!card_busy || card_remaining == 0) return 0;
        uint32_t word = 0;
        if (rom && (size_t)card_addr + 4u <= rom_len)
            word = (uint32_t)rom[card_addr] | ((uint32_t)rom[card_addr+1] << 8) |
                   ((uint32_t)rom[card_addr+2] << 16) | ((uint32_t)rom[card_addr+3] << 24);
        card_addr += 4; card_remaining -= 4;
        if (card_remaining == 0) { card_busy = 0; card_drq = 0; }
        return word;
    }
    default: return 0;
    }
}
static void io_write32(uint32_t a, uint32_t v) {
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
    case 0x04000204: exmemcnt = v; return;
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
    case 0x040001A4: if ((v & 0x80000000u) && (spicnt & 0x8000u) && !card_busy) { romcnt = v; io_card_start(v); } else romcnt = v; return;
    case 0x040001A8: card_cmd[0]=(uint8_t)v; card_cmd[1]=(uint8_t)(v>>8); card_cmd[2]=(uint8_t)(v>>16); card_cmd[3]=(uint8_t)(v>>24); return;
    case 0x040001AC: card_cmd[4]=(uint8_t)v; card_cmd[5]=(uint8_t)(v>>8); card_cmd[6]=(uint8_t)(v>>16); card_cmd[7]=(uint8_t)(v>>24); return;
    case 0x04000304: powerctl = v; return;
    default: return;
    }
}
static uint32_t io_read16(uint32_t a) {
    switch (a) {
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
static void io_write16(uint32_t a, uint32_t v) {
    switch (a) {
    case 0x04000004: dispstat = v & 0xFFF8u; return;
    case 0x04000180: { io_ipcsync_write(v); return; }
    case 0x04000184: ipc_fifo_cnt = v & 0x8F04u; return;
    case 0x040001A0: spicnt = v; return;
    case 0x04000100: case 0x04000104: case 0x04000108: case 0x0400010C: tm_reload[(a-0x04000100u)>>2] = v; return;
    case 0x04000102: case 0x04000106: case 0x0400010A: case 0x0400010E: tm_ctrl[(a-0x04000102u)>>2] = v; return;
    case 0x04000300: postflg = (uint8_t)v; return;
    default: io_write32(a & ~3u, (io_read32(a & ~3u) & ~(0xFFFFu << ((a&2)*8))) | ((v & 0xFFFFu) << ((a&2)*8))); return;
    }
}
static uint32_t io_read8(uint32_t a) {
    if (a == 0x04000300) return postflg;
    if (a >= 0x04000240u && a <= 0x04000248u) return vramcnt[a - 0x04000240u];
    if (a == 0x04000247) return wramcnt_map;
    return io_read16(a & ~1u) >> ((a & 1u) * 8);
}
static void io_write8(uint32_t a, uint32_t v) {
    if (a == 0x04000300) { postflg = (uint8_t)v; return; }
    if (a >= 0x04000240u && a <= 0x04000248u) { vramcnt[a - 0x04000240u] = v; return; }
    if (a == 0x04000247) { wramcnt_map = v; return; }
    io_write16(a & ~1u, v);
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
 * (e.g. the LZ decompressor flag stream at 0x0200098C). */
static uint32_t dbg_pc;
static int dbg_hits;
static int dbg_limit = 64;
static void debug_dump(uint32_t pc) {
    if (!dbg_pc || pc != dbg_pc || dbg_hits >= dbg_limit) return;
    dbg_hits++;
    fprintf(stderr, "dbg%d pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r12=%08X r13=%08X r14=%08X nzcv=%d%d%d%d\n",
            dbg_hits, pc, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[12], r[13], r[14],
            (int)n, (int)z, (int)c, (int)v);
}

static void trace_pc(uint32_t pc) {
    last_pc = pc;
    cycles++;                       /* coarse timing: ~1 cycle per instruction */
    checkpoint_dump(pc);
    debug_dump(pc);
    if (traced < TRACE_LIMIT) printf("0x%08x\n", pc);
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

static void guest_init(void) { memset(r, 0, sizeof r); n = z = c = v = 0; io_load_rom(); }
"##;

const RUNTIME_ALL_MAIN: &str = r##"int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "arm9_image.bin";
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "cannot open guest image %s\n", path); return 2; }
    size_t got = fread(gmem, 1, IMAGE_LEN, fp);
    fclose(fp);
    if (got != IMAGE_LEN) { fprintf(stderr, "short read: got %zu, want %u\n", got, IMAGE_LEN); return 2; }
    guest_init();
    { const char *s = getenv("NNK_STEP_LIMIT");
      if (s && s[0]) step_limit = strtoull(s, NULL, 0); }
    { const char *s = getenv("NNK_DUMP_AT_PC");
      if (s && s[0]) ckpt_pc = (uint32_t)strtoul(s, NULL, 0); }
    { const char *s = getenv("NNK_DEBUG_AT_PC");
      if (s && s[0]) dbg_pc = (uint32_t)strtoul(s, NULL, 0);
      const char *n2 = getenv("NNK_DEBUG_N");
      if (n2 && n2[0]) dbg_limit = (int)strtol(n2, NULL, 0); }
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
    return 0;
}
"##;
