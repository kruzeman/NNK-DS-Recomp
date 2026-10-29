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

/// Destination of a write: R15 writes update the dispatch `pc`, never the
/// register file slot (the dispatch loop's `pc` is authoritative).
fn dst_c(rd: u8) -> String {
    if rd == 15 {
        "pc".to_string()
    } else {
        format!("r[{}]", rd)
    }
}

/// (C expression, does-it-shift) for a shifter operand.
fn operand_c(o: &Operand, pc: u32) -> (String, bool) {
    match o {
        Operand::Imm(v) => (format!("0x{:08X}u", v), false),
        Operand::Reg { rm, kind, amount, by_reg } => {
            let src = reg_c(*rm, pc);
            let shifted = *by_reg || *kind != 0 || *amount != 0;
            let amt = if *by_reg {
                format!("r[{}] & 255u", amount)
            } else {
                format!("{}u", amount)
            };
            (format!("sh_op({}, {}u, {}, c)", src, kind, amt), shifted)
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
fn ea_emit(out: &mut Vec<String>, ea: &Ea, rn: u8, pc: u32) -> String {
    let base = reg_c(rn, pc);
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
            let src = reg_c(*rm, pc);
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

/// Emit C statements for one ARM instruction.
///
/// `global == false` is the Phase-1A single-function mode (direct calls are
/// logged shims, returns leave the dispatch function, SWI halts).  `global ==
/// true` is the `recompile-all` mode: one program-wide PC-dispatch image, so
/// direct calls/returns become real pc updates, and SWIs become HLE calls
/// that return to the next instruction.
fn emit_arm(w: u32, addr: u32, out: &mut Vec<String>, global: bool) -> EmitOutcome {
    let insn = decode_arm(w, addr);
    let c = cond_of(w);
    let guarded = c != 14;
    let ce = cond_expr(c).to_string();
    let mut body: Vec<String> = Vec::new();
    let mut flow = Flow::Next;
    let mut pc_written = false;
    let mut shimmed = false;
    let mut halts = false;

    match &insn.kind {
        Kind::Dp { op, s, rd, rn, src } => {
            let (opnd, shifted) = operand_c(src, addr);
            let rn_v = reg_c(*rn, addr);
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
                        body.push(format!("{} = _res;", dst_c(*rd)));
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
                        body.push(format!("{} = _res;", dst_c(*rd)));
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
                        3 | 11 => (
                            "(_op - _rn)".to_string(),
                            "(uint64_t)_op - (uint64_t)_rn".to_string(),
                            "(int64_t)(int32_t)_op - (int64_t)(int32_t)_rn".to_string(),
                        ),
                        4 => (
                            "(_rn + _op)".to_string(),
                            "(uint64_t)_rn + (uint64_t)_op".to_string(),
                            "(int64_t)(uint32_t)_rn + (int64_t)(uint32_t)_op".to_string(),
                        ),
                        5 => (
                            "(_rn + _op + c)".to_string(),
                            "(uint64_t)_rn + (uint64_t)_op + c".to_string(),
                            "(int64_t)(uint32_t)_rn + (int64_t)(uint32_t)_op + (int64_t)c".to_string(),
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
                        body.push(format!("{} = _res;", dst_c(*rd)));
                    }
                    if *s {
                        body.push("z = (_res == 0); n = (_res >> 31) & 1;".into());
                        match op {
                            2 | 3 | 6 | 7 => body.push("c = (_uw >> 32) ? 0u : 1u;".into()),
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
            let (opnd, _) = operand_c(src, addr);
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
            let src = reg_c(*rm, addr);
            pc_written = true;
            if *link {
                // BLX rm: the return address is the next instruction.
                body.push(format!("r[14] = 0x{:08X}u;", addr.wrapping_add(4)));
            }
            if *rm == 14 && !*link {
                if global {
                    flow = Flow::Indirect(None);
                    body.push("{ uint32_t _t = r[14];".into());
                    body.push(
                        "if (_t & 1u) { halt_reason(\"return to Thumb target\"); return; }".into(),
                    );
                    body.push("pc = _t & ~1u; }".into());
                } else {
                    flow = Flow::Return;
                    body.push("pc = r[14];".into());
                }
            } else {
                flow = Flow::Indirect(None);
                body.push(format!("{{ uint32_t _t = {};", src));
                body.push("if (_t & 1u) { halt_reason(\"interworking to Thumb target\"); return; }".into());
                body.push("pc = _t & ~1u; }".into());
            }
        }
        Kind::Clz { rd, rm } => {
            body.push(format!("r[{}] = clz32({});", rd, reg_c(*rm, addr)));
        }
        Kind::Mem { load, byte, rd, rn, ea } => {
            body.push("{".into());
            let addr_expr = ea_emit(&mut body, ea, *rn, addr);
            if *load {
                if *rd == 15 {
                    body.push(format!("uint32_t _t = {};", addr_expr));
                    body.push(
                        "if (_t & 1u) { halt_reason(\"loaded Thumb pointer into pc\"); return; }".into(),
                    );
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
                    reg_c(*rd, addr)
                ));
            }
            body.push("}".into());
        }
        Kind::Hw { kind, rd, rn, ea } => {
            body.push("{".into());
            let addr_expr = ea_emit(&mut body, ea, *rn, addr);
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
                        body.push(format!("uint32_t _t = ld32(_a + {}u);", idx * 4));
                        body.push("if (_t & 1u) { halt_reason(\"LDM pc with Thumb bit\"); return; }".into());
                        body.push("pc = _t & ~1u;".into());
                        pc_written = true;
                        flow = Flow::Indirect(None);
                    } else {
                        body.push(format!("r[{}] = ld32(_a + {}u);", rr, idx * 4));
                    }
                } else {
                    body.push(format!("st32(_a + {}u, {});", idx * 4, reg_c(*rr, addr)));
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
                // returns to the next instruction (Flow::Next).
                body.push(format!("if (!hle_swi(0x{:06X}u, 0x{:08X}u)) return;", imm, addr));
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
            body.push(format!("halt_reason(\"undefined instruction 0x{:08X}\"); return;", w));
            flow = Flow::Halt;
            pc_written = true;
        }
        Kind::B { link: true, target, .. } => {
            if global {
                // real call: link the return address and dispatch to the
                // callee inside the shared PC-dispatch loop.
                flow = Flow::Indirect(None);
                pc_written = true;
                body.push(format!("r[14] = 0x{:08X}u;", addr.wrapping_add(4)));
                body.push(format!("pc = 0x{:08X}u;", target));
            } else {
                shimmed = true;
                flow = Flow::Call(*target, false);
                body.push(format!("r[14] = 0x{:08X}u;", addr.wrapping_add(4)));
                body.push(format!("call_shim(0x{:08X}u, 0x{:08X}u);", target, addr));
                pc_written = true;
            }
        }
        Kind::BlxImm { target } => {
            if global {
                // the callee is Thumb and Thumb translation is out of scope:
                // stop honestly instead of running ARM code at a Thumb target.
                halts = true;
                flow = Flow::Halt;
                pc_written = true;
                body.push(format!(
                    "halt_reason(\"call to Thumb callee 0x{:08X} from 0x{:08X}\"); return;",
                    target, addr
                ));
            } else {
                shimmed = true;
                flow = Flow::Call(*target, true);
                body.push(format!("r[14] = 0x{:08X}u;", addr.wrapping_add(4)));
                body.push(format!(
                    "call_shim(0x{:08X}u, 0x{:08X}u); /* blx: callee is Thumb */",
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
/// function with one `switch (pc)` covering every recovered ARM instruction,
/// real direct calls/returns (link register + pc update), runtime dispatch for
/// register-indirect control flow, and SWI/BIOS HLE that returns to the next
/// instruction.  Thumb functions in the recovered set are not translated and
/// are reported (their addresses fall through to the untranslated-PC halt).
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
            stats.thumb_insns += f.lines.len();
        } else {
            stats.funcs_arm += 1;
        }
    }

    let mut cases: BTreeMap<u32, (String, CaseMeta)> = BTreeMap::new();
    for (k, f) in &rec.funcs {
        if k.mode == Mode::Thumb {
            continue; // Thumb translation is out of scope (reported above)
        }
        for (&addr, (words, text)) in &f.lines {
            if cases.contains_key(&addr) {
                stats.dup_cases += 1;
                continue;
            }
            let w = words[0];
            let next = addr.wrapping_add((words.len() * 4) as u32);
            let mut scratch: Vec<String> = Vec::new();
            let oc = emit_arm(w, addr, &mut scratch, true);
            let insn = decode_arm(w, addr);
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
            cases.insert(addr, (case_string(addr, next, text, &oc), meta));
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
        "static void run(void) {\n    uint32_t pc = ENTRY_CONST;\n    for (;;) {\n        if (++steps > STEP_LIMIT) { halt_reason(\"step budget exhausted\"); return; }\n        switch (pc) {\n",
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

static inline uint32_t clz32(uint32_t x) {
    uint32_t k = 0;
    if (x == 0) return 32;
    while ((x & 0x80000000u) == 0) { x <<= 1; k++; }
    return k;
}

/* Full per-execution guest-PC trace: one "0x%08x" line per dispatched
 * instruction (loops repeat), plus a distinct-PC bitmap for the summary. */
static void trace_pc(uint32_t pc) {
    last_pc = pc;
    if (traced < TRACE_LIMIT) printf("0x%08x\n", pc);
    traced++;
    if (pc >= IMAGE_LOAD) {
        uint32_t idx = (pc - IMAGE_LOAD) >> 2;
        if (idx < IMAGE_LEN / 4u && (seen[idx >> 3] & (1u << (idx & 7))) == 0) {
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
 * real effects.  Returns 1 = executed, 0 = halt. */
static int hle_swi(uint32_t imm24, uint32_t at) {
    uint32_t hi = (imm24 >> 16) & 0xFFu;
    uint32_t lo = imm24 & 0xFFu;
    hle_hits++;
    fprintf(stderr,
            "hle swi imm24=0x%06X num=0x%02X lo=0x%02X at=0x%08X r0=0x%08X r1=0x%08X r2=0x%08X r3=0x%08X\n",
            imm24, hi, lo, at, r[0], r[1], r[2], r[3]);
    switch (hi) {
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

static void guest_init(void) { memset(r, 0, sizeof r); n = z = c = v = 0; }
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
