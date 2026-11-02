//! Phase-5AT: the supported FUNCTION-MODE OVERRIDE file.
//!
//! WHY THIS EXISTS
//! ---------------
//! The recovered function set is seeded from a Ghidra table
//! (`work/phase3a/seeds_phase3a3.tsv`) whose last column is the instruction
//! length (`2` = Thumb).  Ghidra reports `-1` (= unknown) for a function it
//! could not size, and `load_seeds` has to default that to ARM.  When the
//! default is wrong the emitted body is the ARM reading of Thumb bytes: the
//! dispatch executes it, the bytes decode as something else entirely, and the
//! run halts on an "undefined instruction" that is really a decoder mismatch.
//!
//! Measured (Phase-5AS/5AT): `0x02062C44` is Thumb (`b510 1c04` = push
//! {r4,lr}; adds r4,r0,#0) and is called by ov17's Thumb function 0x020FEB94
//! with `bl 0x02062C44` at 0x020FEBB2 (LR = 0x020FEBB7, odd = Thumb caller).
//! The seed row carries insn_len = -1, so the image emitted the ARM `cdpne`
//! reading there and trapped at 0x02062C48 with `undefined instruction
//! 0xF94AF000`.  This is a MODE-TABLE bug: nothing about the branch is
//! ambiguous, no literal pointer to the function exists in any of the 22
//! images, and the ROM-derived pc->form map has a hole there.
//!
//! THE CONTRACT
//! ------------
//! `tools/nnk_mode_overrides.txt` is committed and audited:
//!
//! ```text
//! <address> <thumb|arm> <reason>
//! ```
//!
//! `nnktool recompile-all|decompile|recompile-cases --mode-overrides <file>`
//! normalises every seed *and* every function reached by a direct call to the
//! stated mode before it is decoded, so a regeneration produces the correct
//! body deterministically.  `tools/p5at_mode_splice.py` consumes the same file
//! for the frozen dispatch image, so the shipped artifact and a future
//! regeneration cannot disagree.  A row without a reason is rejected: an
//! override is a claim about the guest, and it has to be auditable.

use crate::recover::{FuncKey, Mode};
use std::fs;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModeOverride {
    pub addr: u32,
    pub mode: Mode,
    pub reason: String,
}

/// Parse the override file.  Blank lines and `#` comments are allowed.
pub fn parse(text: &str, path: &str) -> Result<Vec<ModeOverride>, String> {
    let mut out: Vec<ModeOverride> = Vec::new();
    for (i, line) in text.lines().enumerate() {
        let line = line.trim_end();
        if line.trim().is_empty() || line.trim_start().starts_with('#') {
            continue;
        }
        let mut it = line.split_whitespace();
        let a = it.next().unwrap_or("");
        let m = it.next().unwrap_or("");
        let reason = it.collect::<Vec<_>>().join(" ");
        let hex = a.trim_start_matches("0x").trim_start_matches("0X");
        let addr = u32::from_str_radix(hex, 16)
            .map_err(|_| format!("{}:{}: bad address {:?}", path, i + 1, a))?;
        if addr & 1 != 0 {
            return Err(format!(
                "{}:{}: address 0x{:08X} has bit0 set; name the entry, not the Thumb call target",
                path,
                i + 1,
                addr
            ));
        }
        let mode = match m {
            "arm" => Mode::Arm,
            "thumb" => Mode::Thumb,
            _ => return Err(format!("{}:{}: mode must be arm or thumb, got {:?}", path, i + 1, m)),
        };
        if reason.trim().is_empty() {
            return Err(format!(
                "{}:{}: 0x{:08X} {} has no reason; an override must be auditable",
                path,
                i + 1,
                addr,
                m
            ));
        }
        if out.iter().any(|o| o.addr == addr) {
            return Err(format!("{}:{}: duplicate override for 0x{:08X}", path, i + 1, addr));
        }
        out.push(ModeOverride { addr, mode, reason });
    }
    Ok(out)
}

pub fn load(path: &str) -> Result<Vec<ModeOverride>, String> {
    let text = fs::read_to_string(path)
        .map_err(|e| format!("cannot read mode-override file {}: {}", path, e))?;
    parse(&text, path)
}

/// Rewrite the mode of every seed the file names.  Returns how many seeds
/// actually changed mode (an override that matches nothing is not an error at
/// this layer: `apply_to_seeds` is also used on partial seed lists, and the
/// splice tool reports unmatched rows).
pub fn apply_to_seeds(seeds: &mut [FuncKey], ovs: &[ModeOverride]) -> usize {
    let mut n = 0usize;
    for s in seeds.iter_mut() {
        if let Some(o) = ovs.iter().find(|o| o.addr == (s.addr & !1)) {
            if s.mode != o.mode {
                s.mode = o.mode;
                n += 1;
            }
        }
    }
    n
}
