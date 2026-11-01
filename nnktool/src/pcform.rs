// Phase-5AP: a ROM-derived `pc -> form` (Thumb/ARM) map.
//
// WHY: three consecutive barriers (class 48/49/50) were point fixes for the
// same question -- in which PROCESSOR STATE does a pc the recompiler did not
// reach through a known branch execute?  The live re-decoder answered it with
// guesses (a stale sticky T bit, a register that happened to carry the target,
// the predecessor's stride), and each guess was wrong at least once.  This
// module computes the answer from the ROM image itself: run the existing CFG
// recovery (recover.rs -- the same descent the recompiler uses) over every
// image the guest loads, and record for each byte range which state the
// hardware would execute it in.
//
// The map is a set of half-open runs [lo,hi) tagged thumb|arm.  "Authoritative"
// means: if a pc falls inside a run, that run's form is the processor state;
// the runtime keeps its own inference only as a cross-check that must LOG a
// disagreement instead of silently winning.
//
// The descent follows the hardware's interworking rules (all of them live in
// decode.rs / recover.rs, not here):
//   * `bx rM` / `blx rM` take bit0 of the register   (recover: Flow::Indirect
//     + the register-tagged veneer resolution),
//   * `blx <imm>` is ARM                             (decode: Flow::Call(..,false)),
//   * `pop {..,pc}` / `ldm ..,{..,pc}` interwork on the popped bit0,
//   * ARM `mov pc,lr` continues in ARM,
//   * jump tables (`ldr pc,[pc,#imm]`, `add pc,pc,rN,lsl#2`) are expanded by
//     recover::expand_jump_table.
//
// COVERAGE: besides the declared entries (image base / entry / ctor array),
// every 32-bit pointer in every image's bytes that targets another image's
// code is followed as a seed (bit0 selects the state, exactly like blx rM).
// That is the route the guest really uses for overlay exports and vtables, and
// it keeps the map ROM-only -- no melonDS trace is consulted.
use crate::recover::{recover_seeds, FuncKey, Mode};
use std::fs;
use std::path::Path;

pub struct Image {
    pub name: String,
    pub base: u32,
    pub data: Vec<u8>,
}

impl Image {
    pub fn hi(&self) -> u32 {
        self.base.wrapping_add(self.data.len() as u32)
    }
}

/// One [lo,hi) run of code in one processor state.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Run {
    pub lo: u32,
    pub hi: u32,
    pub thumb: bool,
}

pub struct Stats {
    pub runs: usize,
    pub thumb_bytes: u64,
    pub arm_bytes: u64,
    pub seeds: usize,
    pub rounds: usize,
    pub overlaps: Vec<(u32, u32, bool)>,
    pub clipped_bytes: u64,
}

fn covered(runs: &[Run], pc: u32) -> bool {
    let mut lo = 0usize;
    let mut hi = runs.len();
    while lo < hi {
        let mid = (lo + hi) / 2;
        if pc < runs[mid].lo {
            hi = mid;
        } else if pc >= runs[mid].hi {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    false
}

fn sort_runs(runs: &mut Vec<Run>) {
    runs.sort_by_key(|r| (r.lo, r.hi));
}

/// Subtract everything already covered by `map` (sorted, disjoint) from `r`.
fn subtract(map: &[Run], r: Run) -> Vec<Run> {
    let mut out = Vec::new();
    let mut lo = r.lo;
    // first map run that could intersect
    let mut i = map.partition_point(|m| m.hi <= r.lo);
    while i < map.len() && map[i].lo < r.hi && lo < r.hi {
        if map[i].lo > lo {
            out.push(Run { lo, hi: map[i].lo.min(r.hi), thumb: r.thumb });
        }
        lo = lo.max(map[i].hi);
        i += 1;
    }
    if lo < r.hi {
        out.push(Run { lo, hi: r.hi, thumb: r.thumb });
    }
    out
}

/// Insert new runs into the map, clipping them so the map stays disjoint.
/// Returns the number of bytes that were already claimed by an earlier claim
/// (i.e. ambiguous decodings that the map resolves by first-claim-wins).
fn insert_clipped(map: &mut Vec<Run>, fresh: Vec<Run>) -> u64 {
    let mut clipped = 0u64;
    let mut runs = fresh;
    sort_runs(&mut runs);
    for r in runs {
        let pieces = subtract(map, r);
        let kept: u64 = pieces.iter().map(|p| (p.hi - p.lo) as u64).sum();
        clipped += (r.hi - r.lo) as u64 - kept;
        for p in pieces {
            map.push(p);
        }
        // keep the map merged so the next subtract sees disjoint runs
        *map = merge_into(map.clone());
    }
    clipped
}

fn merge_into(runs: Vec<Run>) -> Vec<Run> {
    let mut v = runs;
    sort_runs(&mut v);
    let mut out: Vec<Run> = Vec::with_capacity(v.len());
    for r in v {
        match out.last_mut() {
            Some(last) if r.lo <= last.hi && last.thumb == r.thumb => {
                if r.hi > last.hi {
                    last.hi = r.hi;
                }
            }
            _ => out.push(r),
        }
    }
    out
}

/// Regions of one recovered function: maximal runs of same-state instructions
/// with no gap between them.
fn func_runs(mode: Mode, lines: &std::collections::BTreeMap<u32, (Vec<u32>, String)>) -> Vec<Run> {
    let mut out = Vec::new();
    let mut cur: Option<Run> = None;
    for (&addr, (words, _)) in lines.iter() {
        let size = match mode {
            Mode::Arm => 4u32,
            Mode::Thumb => {
                if words.first().copied().unwrap_or(0) > 0xFFFF {
                    4
                } else {
                    2
                }
            }
        };
        let end = addr.wrapping_add(size);
        match cur {
            Some(mut r) if r.hi == addr => {
                r.hi = end;
                cur = Some(r);
            }
            Some(r) => {
                out.push(r);
                cur = Some(Run { lo: addr, hi: end, thumb: mode == Mode::Thumb });
            }
            None => {
                cur = Some(Run { lo: addr, hi: end, thumb: mode == Mode::Thumb });
            }
        }
    }
    if let Some(r) = cur {
        out.push(r);
    }
    out
}

/// Merge runs of the same state; report any overlap that disagrees.
fn merge(runs_in: Vec<Run>, overlaps: &mut Vec<(u32, u32, bool)>) -> Vec<Run> {
    let mut runs = runs_in;
    sort_runs(&mut runs);
    let mut out: Vec<Run> = Vec::with_capacity(runs.len());
    for r in runs {
        let mut push_r: Option<Run> = Some(r);
        if let Some(last) = out.last_mut() {
            if r.lo < last.hi {
                if last.thumb == r.thumb {
                    if r.hi > last.hi {
                        last.hi = r.hi;
                    }
                    push_r = None;
                } else {
                    // A real disagreement: the same address is code in both
                    // states.  Keep the first, report the fact.
                    let lo = r.lo;
                    let hi = if r.hi < last.hi { r.hi } else { last.hi };
                    overlaps.push((lo, hi, r.thumb));
                    if r.hi > last.hi {
                        // keep the remainder as its own run
                        push_r = Some(Run { lo: last.hi, hi: r.hi, thumb: r.thumb });
                    } else {
                        push_r = None;
                    }
                }
            } else if r.lo == last.hi && last.thumb == r.thumb {
                last.hi = r.hi;
                push_r = None;
            }
        }
        if let Some(x) = push_r {
            out.push(x);
        }
    }
    out
}

/// Build the map.  `seeds` are (image index, entry).  Returns merged runs.
pub fn build(imgs: &[Image], seeds_in: &[(usize, FuncKey)], verbose: bool) -> (Vec<Run>, Stats) {
    use std::collections::BTreeSet;
    let mut map: Vec<Run> = Vec::new();
    let mut pending: Vec<(usize, FuncKey)> = seeds_in.to_vec();
    let mut seen: BTreeSet<(usize, u32, u8)> = BTreeSet::new();
    let mut overlaps: Vec<(u32, u32, bool)> = Vec::new();
    let mut round = 0usize;
    let mut total_seeds = 0usize;
    let mut clipped_bytes = 0u64;
    loop {
        round += 1;
        let mut fresh: Vec<Run> = Vec::new();
        for (i, img) in imgs.iter().enumerate() {
            let mut ks: Vec<FuncKey> = Vec::new();
            for (j, k) in pending.iter() {
                if *j == i && seen.insert((*j, k.addr, k.mode as u8)) {
                    ks.push(*k);
                }
            }
            if ks.is_empty() {
                continue;
            }
            total_seeds += ks.len();
            let r = recover_seeds(&img.data, img.base, img.hi(), &ks, 40_000, 60_000);
            let mut n = 0usize;
            for (key, f) in r.funcs.iter() {
                if f.lines.is_empty() {
                    continue;
                }
                n += f.lines.len();
                fresh.extend(func_runs(key.mode, &f.lines));
            }
            if verbose {
                eprintln!(
                    "pcform: round {} image {}: {} seeds, {} funcs, {} insns",
                    round,
                    img.name,
                    ks.len(),
                    r.funcs.len(),
                    n
                );
            }
        }
        if fresh.is_empty() {
            break;
        }
        // Any fresh run that collides with a form already claimed is an
        // ambiguity; report it and let the earlier claim win (first-claim-wins
        // keeps the map a function, and the running conflict detector below
        // will show whether the lost form was ever executed).
        for r in fresh.iter() {
            for m in map.iter() {
                if r.lo < m.hi && m.lo < r.hi && m.thumb != r.thumb {
                    let lo = r.lo.max(m.lo);
                    let hi = r.hi.min(m.hi);
                    overlaps.push((lo, hi, r.thumb));
                }
            }
        }
        clipped_bytes += insert_clipped(&mut map, fresh);
        // ---- pointer scan: every 32-bit word that targets code we do not
        // cover yet becomes a seed (bit0 = Thumb), exactly the blx rule.
        let mut added: Vec<(usize, FuncKey)> = Vec::new();
        for img in imgs.iter() {
            let mut off = 0usize;
            while off + 4 <= img.data.len() {
                let w = u32::from_le_bytes([
                    img.data[off],
                    img.data[off + 1],
                    img.data[off + 2],
                    img.data[off + 3],
                ]);
                let t = w & !1u32;
                if t != 0 && !covered(&map, t) {
                    for (j, target) in imgs.iter().enumerate() {
                        if t >= target.base && t < target.hi() {
                            added.push((
                                j,
                                FuncKey {
                                    addr: t,
                                    mode: if w & 1 != 0 { Mode::Thumb } else { Mode::Arm },
                                },
                            ));
                            break;
                        }
                    }
                }
                off += 4;
            }
        }
        added.retain(|(i, k)| !seen.contains(&(*i, k.addr, k.mode as u8)));
        added.sort_by_key(|(i, k)| (*i, k.addr, k.mode as u8));
        added.dedup_by_key(|(i, k)| (*i, k.addr, k.mode as u8));
        if added.is_empty() || round >= 8 {
            break;
        }
        pending = added;
    }
    map = merge_into(map);
    let mut thumb_bytes = 0u64;
    let mut arm_bytes = 0u64;
    for r in &map {
        if r.thumb {
            thumb_bytes += (r.hi - r.lo) as u64;
        } else {
            arm_bytes += (r.hi - r.lo) as u64;
        }
    }
    (
        map,
        Stats {
            runs: 0,
            thumb_bytes,
            arm_bytes,
            seeds: total_seeds,
            rounds: round,
            overlaps,
            clipped_bytes,
        },
    )
}

fn emit_table(fh: &mut String, name: &str, runs: &[Run], thumb: bool) {
    let sel: Vec<&Run> = runs.iter().filter(|r| r.thumb == thumb).collect();
    fh.push_str(&format!(
        "static const uint32_t {}[{}][2] = {{\n",
        name,
        if sel.is_empty() { 1 } else { sel.len() }
    ));
    if sel.is_empty() {
        fh.push_str("    { 0u, 0u },\n");
    } else {
        for r in sel {
            fh.push_str(&format!("    {{ 0x{:08X}u, 0x{:08X}u }},\n", r.lo, r.hi));
        }
    }
    fh.push_str("};\n");
}

/// Write the C header the runtime includes (`nnk_pcform_map.h`).
pub fn write_header(path: &Path, runs: &[Run], stats: &Stats, source: &str) -> std::io::Result<()> {
    let mut s = String::new();
    s.push_str("/* GENERATED FILE -- do not edit, do not commit.\n");
    s.push_str(" * Phase-5AP ROM-derived pc -> processor-state map (Thumb/ARM).\n");
    s.push_str(&format!(" * source: {}\n", source));
    s.push_str(&format!(
        " * {} runs ({} Thumb bytes, {} ARM bytes)\n */\n",
        runs.len(),
        stats.thumb_bytes,
        stats.arm_bytes
    ));
    s.push_str("#ifndef NNK_PCFORM_MAP_H\n#define NNK_PCFORM_MAP_H\n\n");
    emit_table(&mut s, "nnk_pcform_thumb", runs, true);
    emit_table(&mut s, "nnk_pcform_arm", runs, false);
    let nt = runs.iter().filter(|r| r.thumb).count();
    let na = runs.len() - nt;
    s.push_str(&format!("#define NNK_PCFORM_THUMB_RUNS {}\n", nt));
    s.push_str(&format!("#define NNK_PCFORM_ARM_RUNS {}\n", na));
    s.push_str(
        "/* 1 = Thumb, 0 = ARM, -1 = the map does not cover this pc (a HOLE:\n\
         \x20* the caller must report it, never silently guess). */\n\
         static inline int nnk_pcform_lookup(uint32_t pc)\n{\n\
         \x20   {\n\
         \x20       int lo = 0, hi = NNK_PCFORM_THUMB_RUNS - 1;\n\
         \x20       while (lo <= hi) {\n\
         \x20           int mid = (lo + hi) >> 1;\n\
         \x20           if (pc < nnk_pcform_thumb[mid][0]) hi = mid - 1;\n\
         \x20           else if (pc >= nnk_pcform_thumb[mid][1]) lo = mid + 1;\n\
         \x20           else return 1;\n\
         \x20       }\n\
         \x20   }\n\
         \x20   {\n\
         \x20       int lo = 0, hi = NNK_PCFORM_ARM_RUNS - 1;\n\
         \x20       while (lo <= hi) {\n\
         \x20           int mid = (lo + hi) >> 1;\n\
         \x20           if (pc < nnk_pcform_arm[mid][0]) hi = mid - 1;\n\
         \x20           else if (pc >= nnk_pcform_arm[mid][1]) lo = mid + 1;\n\
         \x20           else return 0;\n\
         \x20       }\n\
         \x20   }\n\
         \x20   return -1;\n\
         }\n\n#endif\n",
    );
    fs::write(path, s)
}

/// Load an `img <name> <base> <path>` / `seed <name> <addr> [arm|thumb]`
/// manifest.  Named so the caller can report which image a run belongs to.
pub fn load_manifest(
    path: &Path,
) -> (Vec<Image>, Vec<(usize, FuncKey)>) {
    let text = fs::read_to_string(path).unwrap_or_else(|e| {
        eprintln!("pcform: cannot read manifest {}: {}", path.display(), e);
        std::process::exit(1);
    });
    let mut imgs: Vec<Image> = Vec::new();
    let mut seeds: Vec<(usize, FuncKey)> = Vec::new();
    for (lineno, line) in text.lines().enumerate() {
        let line = line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let f: Vec<&str> = line.split_whitespace().collect();
        match f[0] {
            "img" if f.len() == 4 => {
                let base = parse_num(f[2], lineno);
                let data = fs::read(f[3]).unwrap_or_else(|e| {
                    eprintln!("pcform: cannot read image {}: {}", f[3], e);
                    std::process::exit(1);
                });
                imgs.push(Image { name: f[1].to_string(), base, data });
            }
            "seed" if f.len() >= 3 => {
                let name = f[1];
                let idx = imgs.iter().position(|i| i.name == name).unwrap_or_else(|| {
                    eprintln!("pcform: seed for unknown image {}", name);
                    std::process::exit(1);
                });
                let mut addr = parse_num(f[2], lineno);
                let thumb = match f.get(3).copied() {
                    Some("thumb") => true,
                    Some("arm") => false,
                    Some(other) => {
                        eprintln!("pcform: bad seed mode {}", other);
                        std::process::exit(1);
                    }
                    None => {
                        let t = addr & 1 != 0;
                        addr &= !1;
                        t
                    }
                };
                seeds.push((
                    idx,
                    FuncKey { addr: addr & !1, mode: if thumb { Mode::Thumb } else { Mode::Arm } },
                ));
            }
            _ => {
                eprintln!("pcform: bad manifest line {}: {}", lineno + 1, line);
                std::process::exit(1);
            }
        }
    }
    (imgs, seeds)
}

fn parse_num(s: &str, lineno: usize) -> u32 {
    let t = s.trim();
    let v = if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u32::from_str_radix(h, 16)
    } else {
        t.parse::<u32>()
    };
    v.unwrap_or_else(|_| {
        eprintln!("pcform: bad number {:?} on manifest line {}", s, lineno + 1);
        std::process::exit(1);
    })
}
