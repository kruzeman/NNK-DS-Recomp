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
    /// True when the walk stopped on an undefined instruction.
    pub hit_undef: bool,
}

impl Func {
    pub fn bytes(&self) -> usize {
        self.lines
            .values()
            .map(|(w, _)| w.len() * 4)
            .sum()
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
                                        if thumb { "thumb" } else { "arm" },
                                        k.addr
                                    ),
                                ));
                            } else {
                                f.indirects.push((addr, "indirect branch (unresolved)".into()));
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
    let mut funcs: BTreeMap<FuncKey, Func> = BTreeMap::new();
    let mut pending: Vec<FuncKey> = vec![entry];
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
            if !funcs.contains_key(c) {
                pending.push(*c);
            }
        }
        funcs.insert(key, f);
    }
    Recovered { funcs, truncated }
}

/// Total distinct byte coverage of the recovered function bodies.
pub fn coverage_bytes(funcs: &BTreeMap<FuncKey, Func>) -> usize {
    let mut covered: BTreeSet<u32> = BTreeSet::new();
    for f in funcs.values() {
        for (addr, (words, _)) in &f.lines {
            for i in 0..words.len() {
                covered.insert(addr + (i as u32) * 4);
            }
        }
    }
    covered.len() * 4
}
