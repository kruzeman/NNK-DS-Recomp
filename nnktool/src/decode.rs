// ARMv5TE decoder for nnktool: one structured decode feeding disasm text,
// CFG classification and the C re-emitter.  Text formatting follows GNU
// binutils conventions (signed-decimal immediates, sl/fp/ip/sp/lr aliases)
// so `nnktool disasm` output can be diffed against arm-none-eabi-objdump
// after whitespace/comment normalisation (tools/cmp_objdump.py).

pub const COND: [&str; 16] = [
    "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv",
];

pub const REGS: [&str; 16] = [
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "sl", "fp", "ip", "sp", "lr", "pc",
];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ShiftKind {
    Lsl,
    Lsr,
    Asr,
    Ror,
}

#[derive(Clone, Copy, Debug)]
pub enum Operand {
    Imm(u32),
    Reg {
        rm: u8,
        kind: u8,
        amount: u8,
        by_reg: bool,
    },
}

#[derive(Clone, Copy, Debug)]
pub enum Ea {
    /// Immediate offset (signed); pre-indexed or post-indexed.  `up` keeps the
    /// encoding's U bit so a #-0 offset renders like binutils does.
    Imm { off: i32, pre: bool, wb: bool, up: bool },
    Reg {
        rm: u8,
        kind: u8,
        amount: u8,
        pre: bool,
        wb: bool,
        up: bool,
    },
}

#[derive(Clone, Copy, Debug)]
pub enum HwKind {
    Strh,
    Ldrh,
    Ldrsb,
    Ldrsh,
    Strd,
    Ldrd,
}

#[derive(Clone, Copy, Debug)]
pub enum MulKind {
    Mul,
    Mla,
    Umull,
    Umlal,
    Smull,
    Smlal,
}

#[derive(Clone, Debug)]
pub enum Kind {
    /// op: 0=and..7=rsc, 8..11 = tst/teq/cmp/cmn, 12=orr 13=mov 14=bic 15=mvn
    Dp {
        op: u8,
        s: bool,
        rd: u8,
        rn: u8,
        src: Operand,
    },
    Mul {
        kind: MulKind,
        s: bool,
        rd: u8,
        rn: u8,
        rm: u8,
        ra: u8,
    },
    Mrs { spsr: bool, rd: u8 },
    Msr { spsr: bool, mask: u8, src: Operand },
    Bx { rm: u8, link: bool },
    /// cond == 14 (AL) means unconditional.
    B { link: bool, target: u32, cond: u8 },
    BlxImm { target: u32 },
    Mem { load: bool, byte: bool, rd: u8, rn: u8, ea: Ea },
    Hw { kind: HwKind, rd: u8, rn: u8, ea: Ea },
    LdmStm {
        load: bool,
        rn: u8,
        wb: bool,
        list: u16,
        pu: u8,
        s: bool,
    },
    Swp { byte: bool, rd: u8, rn: u8, rm: u8 },
    Clz { rd: u8, rm: u8 },
    Mcr {
        load: bool,
        coproc: u8,
        opc1: u8,
        rd: u8,
        crn: u8,
        crm: u8,
        opc2: u8,
    },
    Coproc,
    Svc { imm: u32 },
    Hint,
    Undef,
}

#[derive(Clone, Debug)]
pub struct Insn {
    pub addr: u32,
    pub words: Vec<u32>, // 1 word (ARM / Thumb-2 pair) or 1 halfword
    pub text: String,
    pub kind: Kind,
}

/// Control-flow classification used by the CFG walker.
#[derive(Clone, Copy, Debug)]
pub enum Flow {
    Next,
    Branch(u32),
    BranchCond(u32),
    Call(u32, bool), // (target, callee_is_thumb)
    Return,
    Indirect(Option<(u32, bool)>),
    Halt,
}

// ------------------------------------------------------------ image access ---

static mut IMAGE: *const u8 = std::ptr::null();
static mut IMAGE_LEN: usize = 0;
static mut IMAGE_BASE: u32 = 0;

/// Install the guest image for literal/veneer resolution.  The caller keeps
/// the buffer alive for the lifetime of the process (a CLI one-shot).
pub fn set_image(data: &[u8], base: u32) {
    unsafe {
        IMAGE = data.as_ptr();
        IMAGE_LEN = data.len();
        IMAGE_BASE = base;
    }
}

pub fn lit_value(addr: u32) -> Option<u32> {
    unsafe {
        if IMAGE.is_null() {
            return None;
        }
        let off = addr.checked_sub(IMAGE_BASE)? as usize;
        if off + 4 > IMAGE_LEN {
            return None;
        }
        let p = std::slice::from_raw_parts(IMAGE.add(off), 4);
        Some(u32::from_le_bytes(p.try_into().ok()?))
    }
}

pub fn lit_value_at(data: &[u8], base: u32, addr: u32) -> Option<u32> {
    let off = addr.checked_sub(base)? as usize;
    let b = data.get(off..off + 4)?;
    Some(u32::from_le_bytes(b.try_into().ok()?))
}

// ------------------------------------------------------------- text helpers ---

fn ror32(v: u32, n: u32) -> u32 {
    let n = n & 31;
    if n == 0 {
        v
    } else {
        v.rotate_right(n)
    }
}

pub fn arm_imm(w: u32) -> u32 {
    ror32(w & 0xFF, ((w >> 8) & 15) * 2)
}

fn imm_text(v: u32) -> String {
    format!("#{}", v as i32)
}

/// Number of contiguous 1-runs in a 32-bit word (binutils decides between a
/// decimal immediate and the raw `#imm8, rot` form by this).
fn bit_runs(v: u32) -> u32 {
    let mut runs = 0u32;
    let mut prev = 0u32;
    for i in 0..32 {
        let b = (v >> i) & 1;
        if b == 1 && prev == 0 {
            runs += 1;
        }
        prev = b;
    }
    runs
}

/// Immediate operand rendering as GNU objdump does it.  binutils prints the
/// expanded value only when the word carries the *minimal-rotation* encoding
/// of that value (the one GNU as would emit); any other (imm8, rot) pairing —
/// e.g. a compiler-generated pool word — prints as the raw `#imm8, rot`.
fn imm_operand_text(w: u32) -> String {
    let imm8 = w & 0xFF;
    let rot = ((w >> 8) & 15) * 2;
    let v = ror32(imm8, rot);
    let mut canonical_rot = None;
    for r in (0..32u32).step_by(2) {
        let cand = if r == 0 { v } else { (v << r) | (v >> (32 - r)) } & 0xFF;
        if ror32(cand, r) == v {
            canonical_rot = Some(r);
            break;
        }
    }
    if canonical_rot == Some(rot) {
        imm_text(v)
    } else {
        format!("#0x{:x}, 0x{:x}", imm8, rot)
    }
}

fn shift_text(kind: u8, amount: u8, by_reg: bool) -> String {
    let name = ["lsl", "lsr", "asr", "ror"][(kind & 3) as usize];
    if by_reg {
        return format!(", {} r{}", name, amount);
    }
    match (kind & 3, amount) {
        (0, 0) => String::new(),
        (1, 0) | (2, 0) => format!(", {} #32", name),
        (3, 0) => ", rrx".to_string(),
        _ => format!(", {} #{}", name, amount),
    }
}

pub fn operand_text(o: &Operand) -> String {
    match o {
        Operand::Imm(v) => imm_text(*v),
        Operand::Reg { rm, kind, amount, by_reg } => {
            let mut s = REGS[*rm as usize].to_string();
            s.push_str(&shift_text(*kind, *amount, *by_reg));
            s
        }
    }
}

fn ea_text(base: &str, ea: &Ea) -> String {
    match ea {
        Ea::Imm { off, pre, wb, up } => {
            let w = if *wb { "!" } else { "" };
            if *pre {
                if *off == 0 {
                    if *up {
                        format!("[{}{}]", base, w)
                    } else {
                        // binutils renders U=0 with a zero offset as #-0x0.
                        format!("[{}, #-0x0]{}", base, w)
                    }
                } else if *off > 0 {
                    format!("[{}, #{}]{}", base, off, w)
                } else {
                    format!("[{}, #-{}]{}", base, -(*off as i64), w)
                }
            } else if *off >= 0 {
                format!("[{}], #{}", base, off)
            } else {
                format!("[{}], #-{}", base, -(*off as i64))
            }
        }
        Ea::Reg { rm, kind, amount, pre, wb, up } => {
            let shift = shift_text(*kind, *amount, false);
            let reg = if *up {
                format!("r{}{}", rm, shift)
            } else {
                format!("-r{}{}", rm, shift)
            };
            if *pre {
                format!("[{}, {}{}]{}", base, reg, if *wb { "!" } else { "" }, "")
            } else {
                format!("[{}], {}{}", base, reg, "")
            }
        }
    }
}

fn psr_mask_text(mask: u8) -> String {
    let mut s = String::new();
    if mask & 8 != 0 {
        s.push('f');
    }
    if mask & 4 != 0 {
        s.push('s');
    }
    if mask & 2 != 0 {
        s.push('x');
    }
    if mask & 1 != 0 {
        s.push('c');
    }
    if s.is_empty() {
        s.push('c');
    }
    s
}

const DP_NAMES: [&str; 16] = [
    "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr",
    "mov", "bic", "mvn",
];

fn dp_operand(w: u32) -> Operand {
    if w & (1 << 25) != 0 {
        Operand::Imm(arm_imm(w))
    } else {
        let by_reg = w & (1 << 4) != 0;
        Operand::Reg {
            rm: (w & 15) as u8,
            kind: ((w >> 5) & 3) as u8,
            amount: if by_reg {
                ((w >> 8) & 15) as u8
            } else {
                ((w >> 7) & 31) as u8
            },
            by_reg,
        }
    }
}

fn dp_text(cs: &str, op: u8, s: bool, rd: u8, rn: u8, src: &Operand, w: u32) -> String {
    let ss = if s { "s" } else { "" };
    let r = REGS[rd as usize];
    let n = REGS[rn as usize];
    // binutils orders the condition AFTER the s suffix (`mvnseq`), and prints
    // `mov rd, rm, <shift>` as the shift mnemonic itself.
    let o = match src {
        Operand::Imm(_) => imm_operand_text(w),
        Operand::Reg { rm, kind, amount, by_reg } => {
            if op == 13 && !*by_reg && (*kind != 0 || *amount != 0) {
                // `mov rd, rm, <shift>` prints as the shift mnemonic itself.
                return match (*kind, *amount) {
                    (3, 0) => format!("rrx{}{} {}, {}", ss, cs, r, REGS[*rm as usize]),
                    _ => format!(
                        "{}{}{} {}, {}, #{}",
                        ["lsl", "lsr", "asr", "ror"][(*kind & 3) as usize],
                        ss,
                        cs,
                        r,
                        REGS[*rm as usize],
                        amount
                    ),
                };
            }
            operand_text(src)
        }
    };
    match op {
        // CMP/CMN/TST/TEQ imply the S flag; binutils prints cond AFTER s.
        8 | 9 | 10 | 11 => {
            let name = DP_NAMES[op as usize];
            format!("{}{} {}, {}", name, cs, n, o)
        }
        13 => format!("mov{}{} {}, {}", ss, cs, r, o),
        15 => format!("mvn{}{} {}, {}", ss, cs, r, o),
        _ => format!("{}{}{} {}, {}, {}", DP_NAMES[op as usize], ss, cs, r, n, o),
    }
}

fn ldm_stm_text(cs: &str, load: bool, rn: u8, wb: bool, list: u16, pu: u8, s: bool) -> String {
    let regs: Vec<&str> = (0..16).filter(|i| list & (1 << i) != 0).map(|i| REGS[i]).collect();
    let joined = format!("{{{}}}", regs.join(", "));
    let base = REGS[rn as usize];
    if !s {
        if !load && pu == 2 && rn == 13 && wb {
            return format!("push{} {}", cs, joined);
        }
        if load && pu == 1 && rn == 13 && wb {
            return format!("pop{} {}", cs, joined);
        }
    }
    let name = match (load, pu) {
        (false, 0) => "stmda",
        (false, 1) => "stmia",
        (false, 2) => "stmdb",
        (false, 3) => "stmib",
        (true, 0) => "ldmda",
        (true, 1) => "ldmia",
        (true, 2) => "ldmdb",
        _ => "ldmib",
    };
    format!("{}{} {}{}, {}{}", name, cs, base, if wb { "!" } else { "" }, joined, if s { "^" } else { "" })
}

// ---------------------------------------------------------------- ARM decode ---

pub fn decode_arm(w: u32, addr: u32) -> Insn {
    let csuf = COND[(w >> 28) as usize];
    let cs = csuf.to_string();

    // ---- cond == 0xF: BLX imm + v5 hints / barriers ----
    if w >> 28 == 0xF {
        if w & 0xFE00_0000 == 0xFA00_0000 {
            let h = (w >> 24) & 1;
            let delta = ((w & 0x00FF_FFFF) << 8) as i32 >> 6;
            let target = addr.wrapping_add(8).wrapping_add(delta as u32).wrapping_add(h << 1);
            return Insn {
                addr,
                words: vec![w],
                text: format!("blx\t0x{:08x}", target),
                kind: Kind::BlxImm { target },
            };
        }
        if w & 0xFE70_0000 == 0xF450_0000 || w & 0xFE70_0000 == 0xF550_0000 {
            // PLD/PLI immediate and register forms (prefetch hints).
            let rn = ((w >> 16) & 15) as usize;
            let name = if w & (1 << 20) != 0 { "pli" } else { "pld" };
            let ea = if w & (1 << 22) != 0 {
                ea_text(&REGS[rn], &Ea::Imm { off: (w & 0xFFF) as i32, pre: true, wb: false, up: true })
            } else {
                ea_text(&REGS[rn], &Ea::Reg {
                    rm: (w & 15) as u8,
                    kind: ((w >> 5) & 3) as u8,
                    amount: ((w >> 7) & 31) as u8,
                    pre: true,
                    wb: false,
                    up: true,
                })
            };
            return Insn { addr, words: vec![w], text: format!("{}\t{}", name, ea), kind: Kind::Hint };
        }
        if w == 0xF101_0000 || w == 0xF102_0000 {
            let name = if w == 0xF101_0000 { "cpsie\ti" } else { "cpsid\ti" };
            return Insn { addr, words: vec![w], text: name.to_string(), kind: Kind::Hint };
        }
        if w == 0xF57F_F01F {
            return Insn { addr, words: vec![w], text: "clrex".into(), kind: Kind::Hint };
        }
        if w == 0xF57F_F04F || w == 0xF57F_F05F || w == 0xF57F_F06F {
            let name = if w == 0xF57F_F04F {
                "dsb\tst"
            } else if w == 0xF57F_F05F {
                "dmb\tst"
            } else {
                "isb\tst"
            };
            return Insn { addr, words: vec![w], text: name.into(), kind: Kind::Hint };
        }
        if w == 0xF320_F000 {
            return Insn { addr, words: vec![w], text: "nop".into(), kind: Kind::Hint };
        }
        return Insn { addr, words: vec![w], text: String::new(), kind: Kind::Undef };
    }

    // ---- BX / BLX register ----
    if w & 0x0FFF_FFF0 == 0x012F_FF10 || w & 0x0FFF_FFF0 == 0x012F_FF30 {
        // bits 5..4: 01 = BX, 11 = BLX (register forms share the encoding).
        let link = w & 0x30 == 0x30;
        let rm = (w & 15) as u8;
        let name = if link { "blx" } else { "bx" };
        return Insn {
            addr,
            words: vec![w],
            text: format!("{}{} {}", name, cs, REGS[rm as usize]),
            kind: Kind::Bx { rm, link },
        };
    }
    // ---- CLZ ----
    if w & 0x0FFF_0FF0 == 0x016F_0F10 {
        let rd = ((w >> 12) & 15) as u8;
        let rm = (w & 15) as u8;
        return Insn {
            addr,
            words: vec![w],
            text: format!("clz{} {}, {}", cs, REGS[rd as usize], REGS[rm as usize]),
            kind: Kind::Clz { rd, rm },
        };
    }
    // ---- MRS ----
    if w & 0x0FBF_0FFF == 0x010F_0000 {
        let spsr = w & (1 << 22) != 0;
        let rd = ((w >> 12) & 15) as u8;
        return Insn {
            addr,
            words: vec![w],
            text: format!("mrs{} {}, {}", cs, REGS[rd as usize], if spsr { "SPSR" } else { "CPSR" }),
            kind: Kind::Mrs { spsr, rd },
        };
    }
    // ---- MSR (register) ----
    if w & 0x0FB0_FFF0 == 0x0120_F000 {
        let spsr = w & (1 << 22) != 0;
        let mask = ((w >> 16) & 15) as u8;
        let rm = (w & 15) as u8;
        return Insn {
            addr,
            words: vec![w],
            text: format!(
                "msr{} {}{}, {}",
                cs,
                if spsr { "SPSR_" } else { "CPSR_" },
                psr_mask_text(mask),
                REGS[rm as usize]
            ),
            kind: Kind::Msr { spsr, mask, src: Operand::Reg { rm, kind: 0, amount: 0, by_reg: false } },
        };
    }
    // ---- MSR (immediate) ----
    if w & 0x0FB0_F000 == 0x0320_F000 {
        let spsr = w & (1 << 22) != 0;
        let mask = ((w >> 16) & 15) as u8;
        let imm = arm_imm(w);
        return Insn {
            addr,
            words: vec![w],
            text: format!(
                "msr{}{} {}{}, {}",
                cs,
                if spsr { "" } else { "" },
                if spsr { "SPSR_" } else { "CPSR_" },
                psr_mask_text(mask),
                imm_operand_text(w)
            ),
            kind: Kind::Msr { spsr, mask, src: Operand::Imm(imm) },
        };
    }
    // ---- MUL / MLA ----
    if w & 0x0FC0_00F0 == 0x0000_0090 {
        let s = w & (1 << 20) != 0;
        let a = w & (1 << 21) != 0;
        let rd = ((w >> 16) & 15) as u8;
        let rn = ((w >> 12) & 15) as u8;
        let rs = ((w >> 8) & 15) as u8;
        let rm = (w & 15) as u8;
        let name = format!("{}{}{}", if a { "mla" } else { "mul" }, cs, if s { "s" } else { "" });
        let text = if a {
            format!("{} {}, {}, {}, {}", name, REGS[rd as usize], REGS[rm as usize], REGS[rs as usize], REGS[rn as usize])
        } else {
            format!("{} {}, {}, {}", name, REGS[rd as usize], REGS[rm as usize], REGS[rs as usize])
        };
        return Insn {
            addr,
            words: vec![w],
            text,
            kind: Kind::Mul { kind: if a { MulKind::Mla } else { MulKind::Mul }, s, rd, rn, rm, ra: rs },
        };
    }
    // ---- UMULL / UMLAL / SMULL / SMLAL ----
    if w & 0x0F80_00F0 == 0x0080_0090 {
        let s = w & (1 << 20) != 0;
        let signed = w & (1 << 22) != 0;
        let acc = w & (1 << 21) != 0;
        let rdhi = ((w >> 16) & 15) as u8;
        let rdlo = ((w >> 12) & 15) as u8;
        let rs = ((w >> 8) & 15) as u8;
        let rm = (w & 15) as u8;
        let (mk, base) = match (signed, acc) {
            (false, false) => (MulKind::Umull, "umull"),
            (false, true) => (MulKind::Umlal, "umlal"),
            (true, false) => (MulKind::Smull, "smull"),
            _ => (MulKind::Smlal, "smlal"),
        };
        return Insn {
            addr,
            words: vec![w],
            text: format!(
                "{}{}{} {}, {}, {}, {}",
                base, cs, if s { "s" } else { "" },
                REGS[rdlo as usize], REGS[rdhi as usize], REGS[rm as usize], REGS[rs as usize]
            ),
            kind: Kind::Mul { kind: mk, s, rd: rdhi, rn: rdlo, rm, ra: rs },
        };
    }
    // ---- SWP ----
    if w & 0x0FB0_0FF0 == 0x0100_0090 {
        let byte = w & (1 << 22) != 0;
        let rn = ((w >> 16) & 15) as u8;
        let rd = ((w >> 12) & 15) as u8;
        let rm = (w & 15) as u8;
        return Insn {
            addr,
            words: vec![w],
            text: format!(
                "swp{}{} {}, {}, [{}]",
                cs,
                if byte { "b" } else { "" },
                REGS[rd as usize],
                REGS[rm as usize],
                REGS[rn as usize]
            ),
            kind: Kind::Swp { byte, rd, rn, rm },
        };
    }
    // ---- extra load/store: halfword, signed, double ----
    if w & 0x0E00_0090 == 0x0000_0090 && w & 0x0000_0060 != 0 {
        let h = ((w >> 5) & 3) as u8;
        let load = w & (1 << 20) != 0;
        let pre = w & (1 << 24) != 0;
        let up = w & (1 << 23) != 0;
        let imm_form = w & (1 << 22) != 0;
        let wb = w & (1 << 21) != 0;
        let rn = ((w >> 16) & 15) as u8;
        let rd = ((w >> 12) & 15) as u8;
        let ea = if imm_form {
            let off = (((w >> 4) & 0xF0) | (w & 15)) as i32;
            Ea::Imm { off: if up { off } else { -off }, pre, wb, up }
        } else {
            Ea::Reg { rm: (w & 15) as u8, kind: 0, amount: 0, pre, wb, up }
        };
        let (name, hk) = match (h, load) {
            (2, false) => ("strd", HwKind::Strd),
            (2, true) => ("ldrd", HwKind::Ldrd),
            (1, false) => ("strh", HwKind::Strh),
            (1, true) => ("ldrh", HwKind::Ldrh),
            (3, true) => ("ldrsh", HwKind::Ldrsh),
            _ => ("ldrsb", HwKind::Ldrsb),
        };
        let text = if matches!(hk, HwKind::Strd | HwKind::Ldrd) {
            // binutils prints the even register of the pair only.
            format!("{}{} {}, {}", name, cs, REGS[rd as usize], ea_text(&REGS[rn as usize], &ea))
        } else {
            format!("{}{} {}, {}", name, cs, REGS[rd as usize], ea_text(&REGS[rn as usize], &ea))
        };
        return Insn { addr, words: vec![w], text, kind: Kind::Hw { kind: hk, rd, rn, ea } };
    }
    // ---- single data transfer ----
    if w & 0x0C00_0000 == 0x0400_0000 {
        let load = w & (1 << 20) != 0;
        let pre = w & (1 << 24) != 0;
        let up = w & (1 << 23) != 0;
        let byte = w & (1 << 22) != 0;
        let wb = w & (1 << 21) != 0;
        let rn = ((w >> 16) & 15) as u8;
        let rd = ((w >> 12) & 15) as u8;
        let ea = if w & (1 << 25) != 0 {
            Ea::Reg {
                rm: (w & 15) as u8,
                kind: ((w >> 5) & 3) as u8,
                amount: ((w >> 7) & 31) as u8,
                pre,
                wb,
                up,
            }
        } else {
            let off = (w & 0xFFF) as i32;
            Ea::Imm { off: if up { off } else { -off }, pre, wb, up }
        };
        let name = match (load, byte) {
            (true, false) => "ldr",
            (true, true) => "ldrb",
            (false, false) => "str",
            (false, true) => "strb",
        };
        return Insn {
            addr,
            words: vec![w],
            text: format!("{}{} {}, {}", name, cs, REGS[rd as usize], ea_text(&REGS[rn as usize], &ea)),
            kind: Kind::Mem { load, byte, rd, rn, ea },
        };
    }
    // ---- block data transfer ----
    if w & 0x0E00_0000 == 0x0800_0000 {
        let load = w & (1 << 20) != 0;
        let rn = ((w >> 16) & 15) as u8;
        let wb = w & (1 << 21) != 0;
        let s = w & (1 << 22) != 0;
        let pu = ((w >> 23) & 3) as u8;
        let list = (w & 0xFFFF) as u16;
        return Insn {
            addr,
            words: vec![w],
            text: ldm_stm_text(&cs, load, rn, wb, list, pu, s),
            kind: Kind::LdmStm { load, rn, wb, list, pu, s },
        };
    }
    // ---- branch / branch-with-link ----
    if w & 0x0E00_0000 == 0x0A00_0000 {
        let link = w & (1 << 24) != 0;
        let delta = ((w & 0x00FF_FFFF) << 8) as i32 >> 6;
        let target = addr.wrapping_add(8).wrapping_add(delta as u32);
        return Insn {
            addr,
            words: vec![w],
            text: format!("{}{}\t0x{:08x}", if link { "bl" } else { "b" }, cs, target),
            kind: Kind::B { link, target, cond: (w >> 28) as u8 },
        };
    }
    // ---- SWI ----
    if w & 0x0F00_0000 == 0x0F00_0000 {
        return Insn {
            addr,
            words: vec![w],
            text: format!("svc{} 0x{:08x}", cs, w & 0x00FF_FFFF),
            kind: Kind::Svc { imm: w & 0x00FF_FFFF },
        };
    }
    // ---- MCR / MRC ----
    if w & 0x0F00_0010 == 0x0E00_0010 {
        let load = w & (1 << 20) != 0;
        let coproc = ((w >> 8) & 15) as u8;
        let opc1 = ((w >> 21) & 7) as u8;
        let crn = ((w >> 16) & 15) as u8;
        let rd = ((w >> 12) & 15) as u8;
        let crm = (w & 15) as u8;
        let opc2 = ((w >> 5) & 7) as u8;
        return Insn {
            addr,
            words: vec![w],
            text: format!(
                "{}{} {}, {}, r{}, cr{}, cr{}, {{{}}}",
                if load { "mrc" } else { "mcr" }, cs, coproc, opc1, rd, crn, crm, opc2
            ),
            kind: Kind::Mcr { load, coproc, opc1, rd, crn, crm, opc2 },
        };
    }
    // ---- CDP / LDC / STC ----
    if w & 0x0E00_0000 == 0x0C00_0000 || w & 0x0F00_0010 == 0x0E00_0000 {
        let text = format!(
            "cdp{} {}, {}, cr{}, cr{}, cr{}, {{{}}}",
            cs, (w >> 8) & 15, (w >> 20) & 15, (w >> 16) & 15, (w >> 12) & 15, w & 15, (w >> 5) & 7
        );
        return Insn { addr, words: vec![w], text, kind: Kind::Coproc };
    }
    // ---- data processing (catch-all) ----
    if w & 0x0C00_0000 == 0x0000_0000 {
        if w == 0xE1A0_0000 {
            return Insn { addr, words: vec![w], text: "nop".into(), kind: Kind::Hint };
        }
        let op = ((w >> 21) & 15) as u8;
        let s = w & (1 << 20) != 0;
        let rn = ((w >> 16) & 15) as u8;
        let rd = ((w >> 12) & 15) as u8;
        let src = dp_operand(w);
        let text = dp_text(&cs, op, s, rd, rn, &src, w);
        return Insn { addr, words: vec![w], text, kind: Kind::Dp { op, s, rd, rn, src } };
    }
    Insn { addr, words: vec![w], text: String::new(), kind: Kind::Undef }
}

// ----------------------------------------------------------- Thumb decode ----

pub fn thumb_fetch(data: &[u8], base: u32, addr: u32) -> Option<(u16, u16, usize)> {
    let off = addr.checked_sub(base)? as usize;
    let b = data.get(off..off + 2)?;
    let h = u16::from_le_bytes([b[0], b[1]]);
    if h & 0xF800 == 0xF000 {
        let b2 = data.get(off + 2..off + 4)?;
        let h2 = u16::from_le_bytes([b2[0], b2[1]]);
        Some((h, h2, 4))
    } else {
        Some((h, 0, 2))
    }
}

pub fn decode_thumb(h: u16, h2: u16, addr: u32, size: usize) -> Insn {
    let text;
    let mut kind;
    if h & 0xF800 == 0xF000 && size == 4 {
        // v5 / Thumb-2 BL, BLX: two halfwords, one32-bit slot
        let j1 = ((h2 >> 13) & 1) as u32;
        let j2 = ((h2 >> 11) & 1) as u32;
        let i1 = (!(j1 ^ (((h >> 10) & 1) as u32))) & 1;
        let i2 = (!(j2 ^ (((h >> 9) & 1) as u32))) & 1;
        let s = ((h >> 10) & 1) as u32;
        let imm10 = (h & 0x3FF) as u32;
        let imm11 = (h2 & 0x7FF) as u32;
        let mut off = (s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) | (imm11 << 1);
        if s != 0 {
            off |= 0xFE00_0000; // sign-extend the 25-bit offset
        }
        let target = addr.wrapping_add(4).wrapping_add(off);
        let blx = h2 & 0x1000 == 0;
        let target = if blx { target & !3 } else { target };
        text = format!("{} 0x{:08x}", if blx { "blx" } else { "bl" }, target);
        kind = Kind::B { link: true, target, cond: 14 };
        if blx {
            // Thumb BLX: call an ARM-state target.
            kind = Kind::BlxImm { target };
        }
    } else {
        let (t, k) = thumb_base(h, addr);
        text = t;
        kind = k;
    }
    Insn {
        addr,
        words: if size == 4 { vec![h as u32 | ((h2 as u32) << 16)] } else { vec![h as u32] },
        text,
        kind,
    }
}

fn thumb_base(h: u16, addr: u32) -> (String, Kind) {
    // format 1: shift by immediate (bits 15..11 < 0b011; 0b011 is format 2)
    if h & 0xE000 == 0x0000 && (h >> 11) < 3 {
        let op = ((h >> 11) & 3) as u8;
        let imm = ((h >> 6) & 31) as u8;
        let rm = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let names = ["lsls", "lsrs", "asrs"];
        let amt = if imm == 0 && op > 0 { 32 } else { imm as u32 };
        return (
            format!("{} r{}, r{}, #{}", names[op as usize], rd, rm, amt),
            Kind::Dp {
                op: [0, 1, 2][op as usize],
                s: true,
                rd,
                rn: rd,
                src: Operand::Reg { rm, kind: op, amount: imm, by_reg: false },
            },
        );
    }
    // format 2: add/sub register or 3-bit immediate
    if h & 0xFC00 == 0x1800 || h & 0xFC00 == 0x1C00 {
        let immf = h & 0x0400 != 0;
        let sub = h & 0x0200 != 0;
        let rn_imm = ((h >> 6) & 7) as u8;
        let rm = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let name = if sub { "sub" } else { "add" };
        if immf {
            return (
                format!("{}s r{}, r{}, #{}", name, rd, rm, rn_imm),
                Kind::Dp { op: if sub { 2 } else { 4 }, s: true, rd, rn: rm, src: Operand::Imm(rn_imm as u32) },
            );
        }
        return (
            format!("{}s r{}, r{}, r{}", name, rd, rm, rn_imm),
            Kind::Dp {
                op: if sub { 2 } else { 4 },
                s: true,
                rd,
                rn: rm,
                src: Operand::Reg { rm: rn_imm, kind: 0, amount: 0, by_reg: false },
            },
        );
    }
    // format 3: mov/cmp/add/sub with 8-bit immediate
    if h & 0xE000 == 0x2000 {
        let op = ((h >> 11) & 3) as u8;
        let rd = ((h >> 8) & 7) as u8;
        let imm = (h & 0xFF) as u32;
        let name = ["movs", "cmp", "adds", "subs"][op as usize];
        let (dpop, rn) = match op {
            0 => (13u8, 0u8),
            1 => (10u8, rd),
            2 => (4u8, rd),
            _ => (2u8, rd),
        };
        return (
            format!("{} r{}, #{}", name, rd, imm),
            Kind::Dp { op: dpop, s: true, rd, rn, src: Operand::Imm(imm) },
        );
    }
    // format 4: ALU operations
    if h & 0xFC00 == 0x4000 {
        let op = ((h >> 6) & 15) as u8;
        let rm = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let (name, dpop) = match op {
            0 => ("ands", 0u8),
            1 => ("eors", 1u8),
            2 => ("lsls", 0u8),
            3 => ("lsrs", 1u8),
            4 => ("asrs", 2u8),
            5 => ("adcs", 4u8),
            6 => ("sbcs", 6u8),
            7 => ("rors", 3u8),
            8 => ("tst", 8u8),
            9 => ("rsbs", 3u8),
            10 => ("cmp", 10u8),
            11 => ("cmn", 11u8),
            12 => ("orrs", 12u8),
            13 => ("muls", 13u8),
            14 => ("bics", 14u8),
            _ => ("mvns", 15u8),
        };
        if op == 13 {
            // binutils prints the two-operand MUL form when Rd == Rs.
            return (
                format!("muls r{}, r{}", rd, rm),
                Kind::Mul { kind: MulKind::Mul, s: true, rd, rn: 0, rm, ra: rd },
            );
        }
        if matches!(op, 2 | 3 | 4 | 7) {
            // register-specified shift of Rd
            let sk = match op {
                2 => 0,
                3 => 1,
                4 => 2,
                _ => 3,
            };
            return (
                format!("{} r{}, r{}", name, rd, rm),
                Kind::Dp {
                    op: dpop,
                    s: true,
                    rd,
                    rn: rd,
                    src: Operand::Reg { rm: rd, kind: sk, amount: rm, by_reg: true },
                },
            );
        }
        return (
            format!("{} r{}, r{}", name, rd, rm),
            Kind::Dp {
                op: dpop,
                s: true,
                rd,
                rn: rd,
                src: Operand::Reg { rm, kind: 0, amount: 0, by_reg: false },
            },
        );
    }
    // format 5: hi register operations / exchange (MOV r8, r8 is the NOP alias)
    if h == 0x46C0 {
        return ("nop".to_string(), Kind::Hint);
    }
    if h & 0xFC00 == 0x4400 {
        let op = (h >> 8) & 3;
        let rd = ((h & 7) | ((h >> 4) & 8)) as u8;
        let rm = ((h >> 3) & 15) as u8;
        if op < 3 {
            let name = ["add", "cmp", "mov"][op as usize];
            let dpop = [4u8, 10, 13][op as usize];
            return (
                format!("{} r{}, r{}", name, REGS[rd as usize], REGS[rm as usize]),
                Kind::Dp { op: dpop, s: true, rd, rn: rd, src: Operand::Reg { rm, kind: 0, amount: 0, by_reg: false } },
            );
        }
        let blx = h & 0x0080 != 0;
        return (
            format!("{} r{}", if blx { "blx" } else { "bx" }, REGS[rm as usize]),
            Kind::Bx { rm, link: blx },
        );
    }
    // format 6: pc-relative load
    if h & 0xF800 == 0x4800 {
        let rd = ((h >> 8) & 7) as u8;
        let imm = (h & 0xFF) as u32;
        let lit = (addr.wrapping_add(4) & !3).wrapping_add(imm << 2);
        return (
            format!("ldr r{}, [pc, #{}] @ 0x{:08x}", rd, imm * 4, lit),
            Kind::Mem {
                load: true,
                byte: false,
                rd,
                rn: 15,
                ea: Ea::Imm { off: (imm * 4) as i32, pre: true, wb: false, up: true },
            },
        );
    }
    // format 7: load/store with register offset (word/byte)
    // format 8: load/store with register offset (sign-extended / halfword)
    // Encoding: 0101 L B 0 Ro(3) Rb(3) Rd(3)  /  0101 H S 1 Ro Rb Rd
    if h & 0xF000 == 0x5000 {
        let ro = ((h >> 6) & 7) as u8;
        let rb = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let ea = Ea::Reg { rm: ro, kind: 0, amount: 0, pre: true, wb: false, up: true };
        let (name, kind) = if h & 0x0200 == 0 {
            // 0101 L B 0 Ro Rb Rd
            let l = h & 0x0800 != 0;
            let b = h & 0x0400 != 0;
            match (l, b) {
                (false, false) => ("str", Kind::Mem { load: false, byte: false, rd, rn: rb, ea }),
                (false, true) => ("strb", Kind::Mem { load: false, byte: true, rd, rn: rb, ea }),
                (true, false) => ("ldr", Kind::Mem { load: true, byte: false, rd, rn: rb, ea }),
                _ => ("ldrb", Kind::Mem { load: true, byte: true, rd, rn: rb, ea }),
            }
        } else {
            // 0101 H S 1 Ro Rb Rd
            let hbit = h & 0x0800 != 0;
            let sbit = h & 0x0400 != 0;
            match (hbit, sbit) {
                (false, false) => ("strh", Kind::Hw { kind: HwKind::Strh, rd, rn: rb, ea }),
                (false, true) => ("ldrsb", Kind::Hw { kind: HwKind::Ldrsb, rd, rn: rb, ea }),
                (true, false) => ("ldrh", Kind::Hw { kind: HwKind::Ldrh, rd, rn: rb, ea }),
                _ => ("ldrsh", Kind::Hw { kind: HwKind::Ldrsh, rd, rn: rb, ea }),
            }
        };
        return (format!("{} r{}, [r{}, r{}]", name, rd, rb, ro), kind);
    }
    // format 9: load/store word or byte with 5-bit immediate
    // Encoding: 011 B L imm5 Rb Rd  (bit12 = B, bit11 = L)
    if h & 0xE000 == 0x6000 {
        let imm5 = ((h >> 6) & 31) as i32;
        let imm = imm5 * 4;
        let b = h & 0x1000 != 0;
        let l = h & 0x0800 != 0;
        let rb = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let ea = Ea::Imm { off: if b { imm5 } else { imm }, pre: true, wb: false, up: true };
        let (name, kind) = match (l, b) {
            (false, false) => ("str", Kind::Mem { load: false, byte: false, rd, rn: rb, ea }),
            (false, true) => ("strb", Kind::Mem { load: false, byte: true, rd, rn: rb, ea }),
            (true, false) => ("ldr", Kind::Mem { load: true, byte: false, rd, rn: rb, ea }),
            _ => ("ldrb", Kind::Mem { load: true, byte: true, rd, rn: rb, ea }),
        };
        return (format!("{} r{}, [r{}, #{}]", name, rd, rb, if b { imm5 } else { imm }), kind);
    }
    // format 10: load/store halfword with 5-bit immediate
    if h & 0xF000 == 0x8000 {
        let imm = ((h >> 6) & 31) as i32 * 2;
        let l = h & 0x0800 != 0;
        let rb = ((h >> 3) & 7) as u8;
        let rd = (h & 7) as u8;
        let ea = Ea::Imm { off: imm, pre: true, wb: false, up: true };
        let (name, kind) = if l {
            ("ldrh", Kind::Hw { kind: HwKind::Ldrh, rd, rn: rb, ea })
        } else {
            ("strh", Kind::Hw { kind: HwKind::Strh, rd, rn: rb, ea })
        };
        return (format!("{} r{}, [r{}, #{}]", name, rd, rb, imm), kind);
    }
    // format 11: SP-relative load/store
    if h & 0xF000 == 0x9000 {
        let rd = ((h >> 8) & 7) as u8;
        let imm = (h & 0xFF) as u32 * 4;
        let load = h & 0x0800 != 0;
        return (
            format!("{} r{}, [sp, #{}]", if load { "ldr" } else { "str" }, rd, imm),
            Kind::Mem { load, byte: false, rd, rn: 13, ea: Ea::Imm { off: imm as i32, pre: true, wb: false, up: true } },
        );
    }
    // format 12: load address (ADD rd, pc/sp, #imm)
    if h & 0xF000 == 0xA000 {
        let rd = ((h >> 8) & 7) as u8;
        let imm = (h & 0xFF) as u32 * 4;
        let sp = h & 0x0800 != 0;
        return (
            format!("add r{}, {}, #{}", rd, if sp { "sp" } else { "pc" }, imm),
            Kind::Dp { op: 4, s: false, rd, rn: if sp { 13 } else { 15 }, src: Operand::Imm(imm) },
        );
    }
    // format 13: adjust stack pointer
    if h & 0xFF00 == 0xB000 {
        let imm = (h & 0x7F) as u32 * 4;
        let sub = h & 0x80 != 0;
        return (
            format!("{} sp, #{}", if sub { "sub" } else { "add" }, imm),
            Kind::Dp { op: if sub { 2 } else { 4 }, s: false, rd: 13, rn: 13, src: Operand::Imm(imm) },
        );
    }
    // format 14: push/pop (POP with the extra bit transfers pc)
    if h & 0xF600 == 0xB400 {
        let list = (h & 0xFF) as u16;
        let lr = h & 0x0100 != 0;
        let load = h & 0x0800 != 0;
        let extra = if load { 0x8000u16 } else { 0x4000 };
        let full = if lr { list | extra } else { list };
        return (
            format!("{} {}", if load { "pop" } else { "push" }, thumb_reglist(full)),
            Kind::LdmStm { load, rn: 13, wb: true, list: full, pu: if load { 1 } else { 2 }, s: false },
        );
    }
    // format 15: multiple load/store
    if h & 0xF000 == 0xC000 {
        let rn = ((h >> 8) & 7) as u8;
        let list = (h & 0xFF) as u16;
        let load = h & 0x0800 != 0;
        let wb = h & 0x0100 == 0;
        return (
            format!(
                "{} r{}{}, {}",
                if load { "ldmia" } else { "stmia" },
                rn,
                if wb { "!" } else { "" },
                thumb_reglist(list)
            ),
            Kind::LdmStm { load, rn, wb, list, pu: 1, s: false },
        );
    }
    // format 16/17: conditional branch / SWI / undefined
    if h & 0xF000 == 0xD000 {
        let cond = (h >> 8) & 15;
        if cond == 14 {
            return (format!("svc #{}", h & 0xFF), Kind::Svc { imm: (h & 0xFF) as u32 });
        }
        if cond == 15 {
            return ("udf #0".to_string(), Kind::Undef);
        }
        let off = (h as i8) as i32;
        let target = addr.wrapping_add(4).wrapping_add((off * 2) as u32);
        return (
            format!("b{}.n 0x{:08x}", COND[cond as usize], target),
            Kind::B { link: false, target, cond: cond as u8 },
        );
    }
    // format 18: unconditional branch
    if h & 0xF800 == 0xE000 {
        let delta = (((h & 0x7FF) << 5) as i16) >> 4;
        let target = addr.wrapping_add(4).wrapping_add(delta as u32);
        return (format!("b.n 0x{:08x}", target), Kind::B { link: false, target, cond: 14 });
    }
    ("und".to_string(), Kind::Undef)
}

fn thumb_reglist(list: u16) -> String {
    let mut parts: Vec<String> = (0..8).filter(|i| list & (1 << i) != 0).map(|i| format!("r{}", i)).collect();
    if list & 0x4000 != 0 {
        parts.push("lr".to_string());
    }
    if list & 0x8000 != 0 {
        parts.push("pc".to_string());
    }
    format!("{{{}}}", parts.join(", "))
}

// ------------------------------------------------------------------- flows ---

/// Resolve `LDR pc, [pc, #+/-imm]` — a statically readable veneer.
pub fn arm_ldr_pc_target(addr: u32, ea: &Ea) -> Option<(u32, bool)> {
    if let Ea::Imm { off, pre: true, wb: false, .. } = ea {
        let pc = addr.wrapping_add(8);
        let lit = if *off >= 0 {
            pc.wrapping_add(*off as u32)
        } else {
            pc.wrapping_sub(-(*off) as u32)
        };
        let target = lit_value(lit)?;
        Some((target, target & 1 != 0))
    } else {
        None
    }
}

/// Resolve `add/sub rM, pc, #imm` immediately preceding `bx rM`.
pub fn arm_bx_veneer(addr: u32, rm: usize) -> Option<(u32, bool)> {
    if addr < 4 {
        return None;
    }
    let prev = lit_value(addr - 4)?;
    if ((prev >> 12) & 15) as usize != rm {
        return None;
    }
    let op = prev & 0x0FEF_0000;
    if op != 0x028F_0000 && op != 0x024F_0000 || prev & (1 << 25) != 0 {
        return None;
    }
    let imm = arm_imm(prev);
    let pc = addr.wrapping_add(4);
    let target = if op == 0x024F_0000 {
        pc.wrapping_sub(imm)
    } else {
        pc.wrapping_add(imm)
    };
    Some((target, target & 1 != 0))
}

/// `LDR rM, [pc, #imm]` immediately preceding `bx rM`.
pub fn arm_bx_ldr_veneer(addr: u32, rm: usize) -> Option<(u32, bool)> {
    if addr < 4 {
        return None;
    }
    let prev = lit_value(addr - 4)?;
    let insn = decode_arm(prev, addr - 4);
    if let Kind::Mem { load: true, rd, rn: 15, ea, .. } = &insn.kind {
        if *rd as usize == rm {
            if let Some(lit) = arm_ldr_pc_target(addr - 4, ea) {
                return Some(lit);
            }
        }
    }
    None
}

pub fn arm_flow(addr: u32, insn: &Insn) -> Flow {
    match &insn.kind {
        Kind::B { link: true, target, .. } => Flow::Call(*target, false),
        Kind::B { link: false, target, cond } => {
            if *cond == 14 {
                Flow::Branch(*target)
            } else {
                Flow::BranchCond(*target)
            }
        }
        Kind::BlxImm { target } => Flow::Call(*target, true),
        Kind::Bx { rm, .. } => {
            if *rm == 14 {
                Flow::Return
            } else {
                let rm = *rm as usize;
                let resolved = arm_bx_veneer(addr, rm).or_else(|| arm_bx_ldr_veneer(addr, rm));
                Flow::Indirect(resolved)
            }
        }
        Kind::Mem { load: true, rd: 15, ea, .. } => Flow::Indirect(arm_ldr_pc_target(addr, ea)),
        Kind::Dp { op: 13, rd: 15, src, .. } => {
            // MOV pc, rM — a return when rM is lr, otherwise an indirect jump.
            if let Operand::Reg { rm, kind: 0, amount: 0, by_reg: false } = src {
                if *rm == 14 {
                    return Flow::Return;
                }
            }
            Flow::Indirect(None)
        }
        Kind::Dp { rd: 15, .. } => Flow::Indirect(None),
        Kind::LdmStm { load: true, list, .. } if list & 0x8000 != 0 => Flow::Indirect(None),
        Kind::Svc { .. } => Flow::Halt,
        Kind::Undef => Flow::Halt,
        _ => Flow::Next,
    }
}

pub fn thumb_flow(addr: u32, insn: &Insn) -> Flow {
    match &insn.kind {
        // Thumb BL stays in Thumb state; Thumb BLX switches to ARM state.
        Kind::B { link: true, target, .. } => Flow::Call(*target, true),
        Kind::BlxImm { target } => Flow::Call(*target, false),
        Kind::B { link: false, target, cond } => {
            if *cond == 14 {
                Flow::Branch(*target)
            } else {
                Flow::BranchCond(*target)
            }
        }
        Kind::Bx { rm, link } => {
            if *rm == 14 && !*link {
                Flow::Return
            } else {
                Flow::Indirect(None)
            }
        }
        Kind::Mem { load: true, rd: 15, .. } => Flow::Indirect(None),
        Kind::LdmStm { load: true, list, .. } if list & 0x8000 != 0 => Flow::Indirect(None),
        Kind::Dp { rd: 15, .. } => Flow::Indirect(None),
        Kind::Svc { .. } => Flow::Halt,
        Kind::Undef => Flow::Halt,
        _ => Flow::Next,
    }
}
