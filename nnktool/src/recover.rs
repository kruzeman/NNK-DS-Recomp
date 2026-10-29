// Function recovery for nnktool: CFG walk from an entry point following
// direct branches/calls, statically resolved veneers (LDR pc / ADD+BX) and
// ARM<->Thumb interworking.  Mirrors the DQ8 dq8tool `trace_function`
// architecture (DQ8/src/main.rs:965) but is shared by `decompile`,
// `disasm` samples and `recompile-entry`.

use crate::decode::*;
use std::collections::{BTreeMap, BTreeSet};

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Mode {
    Arm,
    Thumb,
}

impl Mode {
    pub fn name(&self) -> &'static str {
        match self {
            Mode::Arm => "arm",
            Mode::Thumb => "thumb",
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct FuncKey {
    pub addr: u32,
    pub mode: Mode,
}

#[derive(Clone)]
pub struct Func {
    pub key: FuncKey,
    /// addr -> (encoded words, disassembly text)
    pub lines: BTreeMap<u32, (Vec<u32>, String)>,
    /// Basic-block start addresses.
    pub blocks: Vec<u32>,
    /// Direct callees recovered from this function.
    pub calls: BTreeSet<FuncKey>,
    /// Unresolved indirect control-flow sites: (addr, description).
    pub indirects: Vec<(u32, String)>,
    /// Jump tables statically expanded at this function (site count).
    pub jump_tables: usize,
    /// True when the walk stopped on an undefined instruction.
    pub hit_undef: bool,
}

impl Func {
    pub fn bytes(&self) -> usize {
        self.lines.values().map(|(w, _)| line_bytes(self.key.mode, w)).sum()
    }
}

/// Encoded bytes of one recovered line: ARM words are 4 bytes; a Thumb line
/// is a halfword, except a BL/BLX pair which occupies one 32-bit slot.
fn line_bytes(mode: Mode, words: &[u32]) -> usize {
    match mode {
        Mode::Arm => words.len() * 4,
        Mode::Thumb => {
            if words[0] > 0xFFFF {
                4
            } else {
                2
            }
        }
    }
}

/// A statically expanded jump table site.
pub struct JumpTable {
    pub desc: String,
    /// Addresses to walk as new block starts.
    pub slots: Vec<u32>,
    /// True when the indirect site itself is unconditional (cond == 0xE).
    pub cond_al: bool,
}

fn lit_at(addr: u32) -> Option<u32> {
    lit_value(addr)
}

/// Bound on a jump table: the `cmp rM, #N` that guards the dispatch, else a
/// conservative default scan length.
fn table_bound(lines: &BTreeMap<u32, (Vec<u32>, String)>, addr: u32, idx: u8) -> usize {
    let mut seen = 0usize;
    for (&a, (words, _)) in lines.range(0..addr).rev() {
        if seen >= 24 {
            break;
        }
        seen += 1;
        let insn = decode_arm(words[0], a);
        match &insn.kind {
            Kind::Dp { op: 10 | 11, rn, src: Operand::Imm(v), .. } if *rn == idx && *v < 4096 => {
                return (*v as usize) + 1;
            }
            // a redefinition of the index register ends the search
            Kind::Dp { op: 13, rd, .. } if *rd == idx => break,
            Kind::Mem { load: true, rd, .. } if *rd == idx => break,
            _ => {}
        }
    }
    16
}

/// Value of `rn` immediately before `addr`, for the few PC-relative /
/// literal forms a jump-table base register is built from.
fn reg_def_value(
    lines: &BTreeMap<u32, (Vec<u32>, String)>,
    addr: u32,
    rn: u8,
) -> Option<u32> {
    let mut seen = 0usize;
    for (&a, (words, _)) in lines.range(0..addr).rev() {
        if seen >= 24 {
            break;
        }
        seen += 1;
        let insn = decode_arm(words[0], a);
        match &insn.kind {
            Kind::Dp { op: 4, rd, rn: 15, src: Operand::Imm(v), .. } if *rd == rn => {
                return Some(a.wrapping_add(8).wrapping_add(*v));
            }
            Kind::Dp { op: 2, rd, rn: 15, src: Operand::Imm(v), .. } if *rd == rn => {
                return Some(a.wrapping_add(8).wrapping_sub(*v));
            }
            Kind::Dp { op: 13, rd, src: Operand::Imm(v), .. } if *rd == rn => return Some(*v),
            Kind::Mem { load: true, rd, rn: 15, ea: Ea::Imm { off, pre: true, wb: false, .. }, .. }
                if *rd == rn =>
            {
                let pc = a.wrapping_add(8);
                let lit = if *off >= 0 {
                    pc.wrapping_add(*off as u32)
                } else {
                    pc.wrapping_sub(-(*off) as u32)
                };
                return lit_at(lit);
            }
            Kind::Dp { rd, .. } | Kind::Mem { load: true, rd, .. } if *rd == rn => return None,
            _ => {}
        }
    }
    None
}

/// Statically expand a jump-table indirect site inside `lines`.
///
/// Two ARM shapes are recognised (both are what GCC emits for a switch):
///   * `add pc, pc, rM, lsl #2`  — control enters the table at `addr+8`; each
///     slot is an ARM `b` instruction, so the slots themselves are code;
///   * `ldr pc, [rN, rM, lsl #2]` — each slot word is the absolute target.
/// Anything else stays an unresolved indirect site (recorded with its
/// address by the caller).
pub fn expand_jump_table(
    _data: &[u8],
    base: u32,
    text_end: u32,
    addr: u32,
    insn: &Insn,
    lines: &BTreeMap<u32, (Vec<u32>, String)>,
) -> Option<JumpTable> {
    let w = lines.get(&addr)?.0[0];
    let cond_al = (w >> 28) == 0xE;
    match &insn.kind {
        Kind::Dp {
            op: 4,
            rd: 15,
            rn: 15,
            src: Operand::Reg { rm, kind: 0, amount: 2, by_reg: false },
            ..
        } => {
            let slot0 = addr.wrapping_add(8);
            let bound = table_bound(lines, addr, *rm);
            let mut slots = Vec::new();
            for i in 0..bound {
                let sa = slot0.wrapping_add(4 * i as u32);
                if sa < base || sa.wrapping_add(4) > text_end {
                    break;
                }
                let Some(sw) = lit_at(sa) else { break };
                let s = decode_arm(sw, sa);
                match s.kind {
                    Kind::B { link: false, target, cond: 14 }
                        if target >= base && target < text_end =>
                    {
                        slots.push(sa);
                    }
                    _ => break,
                }
            }
            if slots.is_empty() {
                return None;
            }
            Some(JumpTable {
                desc: format!(
                    "add pc,pc,r{},lsl #2 at 0x{:08X} -> {} ARM branch slots from 0x{:08X} (bound {})",
                    rm,
                    addr,
                    slots.len(),
                    slot0,
                    bound
                ),
                slots,
                cond_al,
            })
        }
        Kind::Mem {
            load: true,
            rd: 15,
            rn,
            ea:
                Ea::Reg {
                    rm: idx,
                    kind: 0,
                    amount: 2,
                    pre: true,
                    wb: false,
                    up: true,
                },
            ..
        } => {
            let tbl = reg_def_value(lines, addr, *rn)?;
            if tbl < base || tbl >= text_end {
                return None;
            }
            let bound = table_bound(lines, addr, *idx);
            let mut slots = Vec::new();
            for i in 0..bound {
                let ea = tbl.wrapping_add(4 * i as u32);
                let Some(t) = lit_at(ea) else { break };
                // only ARM-state, in-image code addresses are walkable here
                if t & 1 != 0 || t < base || t >= text_end || t & 3 != 0 {
                    break;
                }
                slots.push(t);
            }
            if slots.is_empty() {
                return None;
            }
            Some(JumpTable {
                desc: format!(
                    "ldr pc,[r{},r{},lsl #2] at 0x{:08X} -> table 0x{:08X}, {} absolute targets (bound {})",
                    rn,
                    idx,
                    addr,
                    tbl,
                    slots.len(),
                    bound
                ),
                slots,
                cond_al,
            })
        }
        Kind::Bx { rm, .. } => {
            // `blx/bx rT` where rT was just loaded by an indexed table read
            // `ldr rT, [rN, rI, lsl #2]` whose base rN comes from a
            // PC-relative literal (observed at 0x02007FCC: dispatch through
            // the CP15-helper pointer table at 0x02068460).  The site itself
            // is not a table instruction, so scan back for the index load.
            let rm = *rm as usize;
            let mut site: Option<(u32, u8, u8)> = None;
            let mut seen = 0usize;
            for (&a, (words, _)) in lines.range(..addr).rev() {
                if seen >= 8 {
                    break;
                }
                seen += 1;
                let insn = decode_arm(words[0], a);
                match &insn.kind {
                    Kind::Mem {
                        load: true,
                        rd,
                        rn,
                        ea:
                            Ea::Reg {
                                rm: idx,
                                kind: 0,
                                amount: 2,
                                pre: true,
                                wb: false,
                                up: true,
                            },
                        ..
                    } if *rd as usize == rm => {
                        site = Some((a, *rn, *idx));
                        break;
                    }
                    Kind::Dp { rd, .. } | Kind::Mem { load: true, rd, .. }
                        if *rd as usize == rm =>
                    {
                        break;
                    }
                    Kind::Bx { .. } | Kind::B { .. } | Kind::BlxImm { .. } => break,
                    _ => {}
                }
            }
            let (la, rn, idx) = site?;
            let tbl = reg_def_value(lines, la, rn)?;
            if tbl < base || tbl >= text_end {
                return None;
            }
            let bound = table_bound(lines, la, idx);
            let mut slots = Vec::new();
            for i in 0..bound {
                let ea = tbl.wrapping_add(4 * i as u32);
                let Some(t) = lit_at(ea) else { break };
                if t & 1 != 0 || t < base || t >= text_end || t & 3 != 0 {
                    break;
                }
                slots.push(t);
            }
            if slots.is_empty() {
                return None;
            }
            Some(JumpTable {
                desc: format!(
                    "blx/bx r{} via indexed table at 0x{:08X} -> table 0x{:08X}, {} targets (bound {})",
                    rm,
                    addr,
                    tbl,
                    slots.len(),
                    bound
                ),
                slots,
                cond_al,
            })
        }
        _ => None,
    }
}

/// Walk one function's CFG.  `limit` caps instructions per function.
pub fn trace_function(data: &[u8], base: u32, text_end: u32, key: FuncKey, limit: usize) -> Func {
    set_image(data, base);
    let mut f = Func {
        key,
        lines: BTreeMap::new(),
        blocks: Vec::new(),
        calls: BTreeSet::new(),
        indirects: Vec::new(),
        jump_tables: 0,
        hit_undef: false,
    };
    let mut work: Vec<u32> = vec![key.addr];
    let mut visited: BTreeSet<u32> = BTreeSet::new();
    let mut count = 0usize;

    while let Some(start) = work.pop() {
        let mut addr = start;
        f.blocks.push(start);
        loop {
            let width = match key.mode {
                Mode::Arm => 4,
                Mode::Thumb => 2,
            };
            if addr < base
                || addr >= text_end
                || addr as usize + width > base as usize + data.len()
                || !visited.insert(addr)
                || count >= limit
            {
                break;
            }
            count += 1;
            let off = (addr - base) as usize;
            match key.mode {
                Mode::Arm => {
                    let w = u32::from_le_bytes(data[off..off + 4].try_into().unwrap());
                    let insn = decode_arm(w, addr);
                    let text = insn.text.clone();
                    let flow = arm_flow(addr, &insn);
                    f.lines.insert(addr, (vec![w], text));
                    match flow {
                        Flow::Next => addr = addr.wrapping_add(4),
                        Flow::BranchCond(t) => {
                            if t >= base && t < text_end {
                                work.push(t);
                            } else {
                                f.indirects
                                    .push((addr, format!("conditional branch out of image -> 0x{t:08X}")));
                            }
                            addr = addr.wrapping_add(4);
                        }
                        Flow::Branch(t) => {
                            if t >= base && t < text_end {
                                work.push(t);
                            } else {
                                f.indirects
                                    .push((addr, format!("branch out of image -> 0x{t:08X}")));
                            }
                            break;
                        }
                        Flow::Call(t, thumb) => {
                            let k = FuncKey {
                                addr: t & !1,
                                mode: if thumb { Mode::Thumb } else { Mode::Arm },
                            };
                            f.calls.insert(k);
                            addr = addr.wrapping_add(4);
                        }
                        Flow::Return => break,
                        Flow::Indirect(resolved) => {
                            if let Some((t, thumb)) = resolved {
                                let k = FuncKey {
                                    addr: t & !1,
                                    mode: if thumb { Mode::Thumb } else { Mode::Arm },
                                };
                                f.calls.insert(k);
                                f.indirects.push((
                                    addr,
                                    format!(
                                        "veneer -> {} 0x{:08X}",
                                        k.mode.name(),
                                        k.addr
                                    ),
                                ));
                            } else if let Some(exp) = expand_jump_table(data, base, text_end, addr, &insn, &f.lines)
                            {
                                f.jump_tables += 1;
                                f.indirects
                                    .push((addr, format!("jump table resolved: {}", exp.desc)));
                                for s in &exp.slots {
                                    work.push(*s);
                                }
                            } else {
                                f.indirects.push((addr, "indirect branch (unresolved)".into()));
                            }
                            // Fall-through recovery for the indirect site:
                            //  * a link-indirect (`blx rM`) is an indirect CALL —
                            //    control ALWAYS returns to addr+4, so the return
                            //    path must be recovered even when the target is
                            //    resolved (observed: the CP15 dispatch `blx r2`
                            //    at 0x02007FCC left its `pop {r3, pc}` return at
                            //    0x02007FD0 unrecovered, halting the run);
                            //  * a conditional (cond != AL) indirect may fall
                            //    through (e.g. `popne {..., pc}`).
                            // Unconditional plain `bx rM` has no fall-through.
                            let is_call = matches!(insn.kind, Kind::Bx { link: true, .. });
                            if (is_call || (w >> 28) != 0xE) && addr.wrapping_add(4) < text_end {
                                work.push(addr.wrapping_add(4));
                            }
                            break;
                        }
                        Flow::Halt => {
                            if matches!(insn.kind, Kind::Undef) {
                                f.hit_undef = true;
                            }
                            break;
                        }
                    }
                }
                Mode::Thumb => {
                    let Some((h, h2, size)) = thumb_fetch(data, base, addr) else {
                        break;
                    };
                    let insn = decode_thumb(h, h2, addr, size);
                    let text = insn.text.clone();
                    let words: Vec<u32> = if size == 4 {
                        vec![h as u32 | ((h2 as u32) << 16)]
                    } else {
                        vec![h as u32]
                    };
                    let flow = thumb_flow(addr, &insn);
                    f.lines.insert(addr, (words, text));
                    match flow {
                        Flow::Next => addr = addr.wrapping_add(size as u32),
                        Flow::BranchCond(t) => {
                            if t >= base && t < text_end {
                                work.push(t);
                            }
                            addr = addr.wrapping_add(size as u32);
                        }
                        Flow::Branch(t) => {
                            if t >= base && t < text_end {
                                work.push(t);
                            }
                            break;
                        }
                        Flow::Call(t, callee_thumb) => {
                            // Flow::Call flag is "callee is in Thumb state".
                            let mode = if callee_thumb { Mode::Thumb } else { Mode::Arm };
                            f.calls.insert(FuncKey { addr: t & !1, mode });
                            addr = addr.wrapping_add(size as u32);
                        }
                        Flow::Return => break,
                        Flow::Indirect(_) => {
                            f.indirects.push((addr, "indirect branch (unresolved)".into()));
                            // Thumb `blx rM` (0x4780 form: bit7 of the
                            // halfword set) is an indirect CALL — control
                            // returns to addr+size, so recover that path too.
                            if (h & 0x00F0) == 0x0080 && addr.wrapping_add(size as u32) < text_end {
                                work.push(addr.wrapping_add(size as u32));
                            }
                            break;
                        }
                        Flow::Halt => {
                            if matches!(insn.kind, Kind::Undef) {
                                f.hit_undef = true;
                            }
                            break;
                        }
                    }
                }
            }
        }
    }
    f
}

pub struct Recovered {
    pub funcs: BTreeMap<FuncKey, Func>,
    pub truncated: bool,
}

/// Reachability-guided recovery of every function reachable from `entry`.
pub fn recover(
    data: &[u8],
    base: u32,
    text_end: u32,
    entry: FuncKey,
    limit_per_fn: usize,
    max_funcs: usize,
) -> Recovered {
    recover_seeds(data, base, text_end, &[entry], limit_per_fn, max_funcs)
}

/// Recovery seeded from an explicit entry list (the ARM9 entry plus every
/// Ghidra function entry), then extended by direct calls from each seed.
/// Jumps tables and statically resolvable veneers are expanded as during a
/// single-root walk; every other indirect site is recorded with its address.
pub fn recover_seeds(
    data: &[u8],
    base: u32,
    text_end: u32,
    seeds: &[FuncKey],
    limit_per_fn: usize,
    max_funcs: usize,
) -> Recovered {
    let mut funcs: BTreeMap<FuncKey, Func> = BTreeMap::new();
    let mut pending: Vec<FuncKey> = seeds.to_vec();
    let mut truncated = false;
    while let Some(key) = pending.pop() {
        if funcs.contains_key(&key) {
            continue;
        }
        if funcs.len() >= max_funcs {
            truncated = true;
            break;
        }
        let f = trace_function(data, base, text_end, key, limit_per_fn);
        if f.lines.is_empty() {
            continue;
        }
        for c in &f.calls {
            if !funcs.contains_key(c) && !pending.contains(c) {
                pending.push(*c);
            }
        }
        funcs.insert(key, f);
    }
    Recovered { funcs, truncated }
}

/// Seeds that produced no instructions (their address did not decode): a seed
/// key never makes it into `funcs` when its body comes out empty.
pub fn missing_seeds(seeds: &[FuncKey], funcs: &BTreeMap<FuncKey, Func>) -> Vec<FuncKey> {
    seeds.iter().filter(|k| !funcs.contains_key(k)).copied().collect()
}

/// Total distinct byte coverage of the recovered function bodies.
pub fn coverage_bytes(funcs: &BTreeMap<FuncKey, Func>) -> usize {
    let mut covered: BTreeSet<u32> = BTreeSet::new();
    for f in funcs.values() {
        let step = match f.key.mode {
            Mode::Arm => 4u32,
            Mode::Thumb => 2,
        };
        for (addr, (words, _)) in &f.lines {
            let units = (line_bytes(f.key.mode, words) as u32) / step;
            for i in 0..units {
                covered.insert(addr + i * step);
            }
        }
    }
    // count distinct 4-byte units so a Thumb halfword contributes its
    // containing word (the reported number stays "instruction bytes")
    let words: BTreeSet<u32> = covered.iter().map(|a| a & !3).collect();
    words.len() * 4
}
