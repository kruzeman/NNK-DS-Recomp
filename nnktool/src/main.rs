// nnktool — Ni no Kuni (NDS, B2KJ) static-recompilation project CLI.
// No external crates (DQ8/dq8tool convention).
//
// Subcommands:
//   inspect [--full] <rom>   header + FNT/FAT + overlay tables
//   fnt     [--full] <rom>   file name table listing (default: top-level only)
//   extract <rom> <out_dir>  ARM9 / ARM7 / ARM9 overlay binaries
//   disasm <image> [--base A] [--start A] [--end A] [--mode arm|thumb] [--count N]
//   decompile <image> <out_dir> [--base A] [--entry A] [--max-insns N] [--max-funcs N]
//   recompile-entry <image> <out.c> [--base A] [--entry A] [--max-insns N]

mod decode;
mod recover;
mod recompile;

use decode::{decode_arm, decode_thumb, set_image, thumb_fetch};

use std::env;
use std::fs;
use std::path::Path;
use std::process::exit;

fn rd16(d: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([d[o], d[o + 1]])
}
fn rd32(d: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}
fn ascii(d: &[u8]) -> String {
    d.iter()
        .map(|&b| if b.is_ascii_graphic() || b == b' ' { b as char } else { '.' })
        .collect()
}

// ---------------------------------------------------------------- header ---

struct Header {
    title: String,
    gamecode: String,
    makercode: String,
    unit_code: u8,
    seed_select: u8,
    device_capacity: u8,
    region: u8,
    version: u8,
    autostart: u8,
    arm9_rom: u32,
    arm9_entry: u32,
    arm9_ram: u32,
    arm9_size: u32,
    arm7_rom: u32,
    arm7_entry: u32,
    arm7_ram: u32,
    arm7_size: u32,
    fnt_off: u32,
    fnt_size: u32,
    fat_off: u32,
    fat_size: u32,
    ovt9_off: u32,
    ovt9_size: u32,
    ovt7_off: u32,
    ovt7_size: u32,
    normal_cmd: u32,
    key1_cmd: u32,
    banner_off: u32,
    secure_crc: u16,
    secure_delay: u16,
    arm9_hook: u32,
    arm7_hook: u32,
    rom_size_or_rsa: u32,
    header_size: u32,
    header_crc: u16,
    debug_off: u32,
    debug_size: u32,
    debug_ram: u32,
}

impl Header {
    fn parse(d: &[u8]) -> Header {
        if d.len() < 0x200 {
            eprintln!("error: ROM smaller than 0x200 header");
            exit(1);
        }
        let end = d[0x000..0x00C].iter().rposition(|&b| b != 0).map(|i| i + 1).unwrap_or(0);
        Header {
            title: ascii(&d[0x000..0x00C][..end]),
            gamecode: ascii(&d[0x00C..0x010]),
            makercode: ascii(&d[0x010..0x012]),
            unit_code: d[0x012],
            seed_select: d[0x013],
            device_capacity: d[0x014],
            region: d[0x01D],
            version: d[0x01E],
            autostart: d[0x01F],
            arm9_rom: rd32(d, 0x020),
            arm9_entry: rd32(d, 0x024),
            arm9_ram: rd32(d, 0x028),
            arm9_size: rd32(d, 0x02C),
            arm7_rom: rd32(d, 0x030),
            arm7_entry: rd32(d, 0x034),
            arm7_ram: rd32(d, 0x038),
            arm7_size: rd32(d, 0x03C),
            fnt_off: rd32(d, 0x040),
            fnt_size: rd32(d, 0x044),
            fat_off: rd32(d, 0x048),
            fat_size: rd32(d, 0x04C),
            ovt9_off: rd32(d, 0x050),
            ovt9_size: rd32(d, 0x054),
            ovt7_off: rd32(d, 0x058),
            ovt7_size: rd32(d, 0x05C),
            normal_cmd: rd32(d, 0x060),
            key1_cmd: rd32(d, 0x064),
            banner_off: rd32(d, 0x068),
            secure_crc: rd16(d, 0x06C),
            secure_delay: rd16(d, 0x06E),
            arm9_hook: rd32(d, 0x070),
            arm7_hook: rd32(d, 0x074),
            rom_size_or_rsa: rd32(d, 0x080),
            header_size: rd32(d, 0x084),
            header_crc: rd16(d, 0x15E),
            debug_off: rd32(d, 0x160),
            debug_size: rd32(d, 0x164),
            debug_ram: rd32(d, 0x168),
        }
    }
}

// ------------------------------------------------------------------- FNT ---

struct Entry {
    name: String,
    is_dir: bool,
    id: u32, // file ID (files) or dir ID 0xF000+i (dirs)
}

struct Fnt {
    num_dirs: usize,
    entries_off: Vec<u32>,
    first_file_id: Vec<u16>,
    parent: Vec<i32>, // u16 field at +6 (0xF000+parent for non-root)
    entries: Vec<Vec<Entry>>,
    num_files: usize,
}

fn parse_fnt(fnt: &[u8]) -> Fnt {
    if fnt.len() < 8 {
        eprintln!("error: FNT too small");
        exit(1);
    }
    let num_dirs = rd32(fnt, 0) as usize / 8;
    let mut entries_off = Vec::with_capacity(num_dirs);
    let mut first_file_id = Vec::with_capacity(num_dirs);
    let mut parent = Vec::with_capacity(num_dirs);
    for i in 0..num_dirs {
        let b = i * 8;
        if b + 8 > fnt.len() {
            eprintln!("error: FNT main table truncated at dir {}", i);
            exit(1);
        }
        entries_off.push(rd32(fnt, b));
        first_file_id.push(rd16(fnt, b + 4));
        parent.push(rd16(fnt, b + 6) as i32);
    }
    let mut entries = Vec::with_capacity(num_dirs);
    let mut num_files = 0usize;
    for i in 0..num_dirs {
        let mut p = entries_off[i] as usize;
        let mut v = Vec::new();
        let mut file_idx = 0u32;
        loop {
            if p >= fnt.len() {
                eprintln!("error: FNT dir {} runs past end of table", i);
                exit(1);
            }
            let ctrl = fnt[p];
            if ctrl == 0 {
                p += 1;
                break;
            }
            let is_dir = ctrl & 0x80 != 0;
            let len = (ctrl & 0x7F) as usize;
            if p + 1 + len > fnt.len() {
                eprintln!("error: FNT dir {} name runs past end of table", i);
                exit(1);
            }
            let name = ascii(&fnt[p + 1..p + 1 + len]);
            p += 1 + len;
            if is_dir {
                if p + 2 > fnt.len() {
                    eprintln!("error: FNT dir {} folder id truncated", i);
                    exit(1);
                }
                let did = rd16(fnt, p);
                p += 2;
                v.push(Entry { name, is_dir: true, id: did as u32 });
            } else {
                let fid = first_file_id[i] as u32 + file_idx;
                file_idx += 1;
                num_files += 1;
                v.push(Entry { name, is_dir: false, id: fid });
            }
        }
        entries.push(v);
    }
    Fnt { num_dirs, entries_off, first_file_id, parent, entries, num_files }
}

fn walk_fnt(fnt: &Fnt, dir: usize, path: &mut String, out: &mut Vec<String>) {
    for e in &fnt.entries[dir] {
        let old = path.len();
        if !path.is_empty() {
            path.push('/');
        }
        path.push_str(&e.name);
        if e.is_dir {
            out.push(format!("{}/", path));
            walk_fnt(fnt, (e.id & 0xFFF) as usize, path, out);
        } else {
            out.push(format!("{}\t{}", e.id, path));
        }
        path.truncate(old);
    }
}

// ------------------------------------------------------------------- OVT ---

struct Overlay {
    id: u32,
    ram: u32,
    ram_size: u32,
    bss: u32,
    init_start: u32,
    init_end: u32,
    file_id: u32,
    compressed_size: u32, // low 24 bits of +0x1C
    flags: u32,           // high 8 bits of +0x1C (bit0 = compressed, bit1 = anonymous)
}

fn parse_ovt(t: &[u8]) -> Vec<Overlay> {
    let n = t.len() / 32;
    let mut v = Vec::with_capacity(n);
    for i in 0..n {
        let b = i * 32;
        let raw = rd32(t, b + 0x1C);
        v.push(Overlay {
            id: rd32(t, b),
            ram: rd32(t, b + 4),
            ram_size: rd32(t, b + 8),
            bss: rd32(t, b + 0x0C),
            init_start: rd32(t, b + 0x10),
            init_end: rd32(t, b + 0x14),
            file_id: rd32(t, b + 0x18),
            compressed_size: raw & 0x00FF_FFFF,
            flags: raw >> 24,
        });
    }
    v
}

// ------------------------------------------------------------ entry point ---

fn die(msg: &str) -> ! {
    eprintln!("error: {}", msg);
    exit(1);
}

fn load(rom: &str) -> Vec<u8> {
    fs::read(Path::new(rom)).unwrap_or_else(|e| die(&format!("cannot read {}: {}", rom, e)))
}

fn usage() -> ! {
    eprintln!(
        "usage:\n  nnktool inspect [--full] <rom>\n  nnktool fnt [--full] <rom>\n  nnktool extract <rom> <out_dir>\
         \n  nnktool disasm <image> [--base 0xADDR] [--start 0xADDR] [--end 0xADDR] [--mode arm|thumb] [--count N]\
         \n  nnktool decompile <image> <out_dir> [--base 0xADDR] [--entry 0xADDR] [--seeds <ghidra.tsv>] [--max-insns N] [--max-funcs N]\
         \n  nnktool recompile-entry <image> <out.c> [--base 0xADDR] [--entry 0xADDR] [--max-insns N]\
         \n  nnktool recompile-all <image> <out.c> [--base 0xADDR] [--entry 0xADDR] [--seeds <ghidra.tsv>] [--max-insns N] [--max-funcs N]"
    );
    exit(2);
}

/// Value of `--key <value>` in args, if present.
fn opt(args: &[String], key: &str) -> Option<String> {
    args.iter().position(|a| a == key).and_then(|i| args.get(i + 1)).cloned()
}

/// Split argv into positionals, `--key value` options and the --full flag.
fn split_args(args: &[String]) -> (Vec<String>, Vec<String>, bool) {
    const VALUE_FLAGS: [&str; 10] = [
        "--base", "--start", "--end", "--mode", "--count", "--entry", "--entry-mode",
        "--max-insns", "--max-funcs", "--seeds",
    ];
    let mut pos: Vec<String> = Vec::new();
    let mut flat: Vec<String> = Vec::new(); // key, value, key, value... for opt()
    let mut full = false;
    let mut it = args.iter(); // args[0] is the subcommand and stays a positional
    while let Some(a) = it.next() {
        if a == "--full" {
            full = true;
        } else if VALUE_FLAGS.contains(&a.as_str()) {
            flat.push(a.clone());
            if let Some(v) = it.next() {
                flat.push(v.clone());
            }
        } else if let Some((k, v)) = a.split_once('=') {
            if k.starts_with("--") {
                flat.push(k.to_string());
                flat.push(v.to_string());
            } else {
                pos.push(a.clone());
            }
        } else if a.starts_with("--") {
            flat.push(a.clone());
        } else {
            pos.push(a.clone());
        }
    }
    (pos, flat, full)
}

fn parse_addr(s: &str) -> u32 {
    let t = s.trim();
    let v = if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u32::from_str_radix(h, 16)
    } else {
        t.parse::<u32>()
    };
    v.unwrap_or_else(|_| die(&format!("bad address/number: {}", s)))
}

const ARM9_BASE: u32 = 0x0200_0000;
const ARM9_ENTRY: u32 = 0x0200_0800;

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let cmd = args[0].as_str();
    let (pos_vec, flags, full) = split_args(&args);
    let pos: Vec<&String> = pos_vec.iter().collect();
    let _ = &flags;

    match cmd {
        "inspect" => {
            if pos.len() != 2 {
                usage();
            }
            let rom = load(pos[1]);
            inspect(&rom, full);
        }
        "fnt" => {
            if pos.len() != 2 {
                usage();
            }
            let rom = load(pos[1]);
            let h = Header::parse(&rom);
            let fnt = parse_fnt(&rom[h.fnt_off as usize..(h.fnt_off + h.fnt_size) as usize]);
            let mut lines = Vec::new();
            let mut path = String::new();
            walk_fnt(&fnt, 0, &mut path, &mut lines);
            if full {
                for l in &lines {
                    println!("{}", l);
                }
            } else {
                for l in &lines {
                    if !l.contains('\t') {
                        println!("{}", l);
                    } else if !l.contains('/') {
                        println!("{}", l);
                    }
                }
            }
        }
        "extract" => {
            if pos.len() != 3 {
                usage();
            }
            let rom = load(pos[1]);
            extract(&rom, Path::new(pos[2]));
        }
        "disasm" => {
            if pos.len() != 2 {
                usage();
            }
            let image = load(pos[1]);
            let base = opt(&args, "--base").map(|s| parse_addr(&s)).unwrap_or(ARM9_BASE);
            let mode = opt(&args, "--mode").unwrap_or_else(|| "arm".into());
            let count = opt(&args, "--count").map(|s| parse_addr(&s) as usize).unwrap_or(64);
            let start = opt(&args, "--start").map(|s| parse_addr(&s)).unwrap_or(base);
            let end = opt(&args, "--end").map(|s| parse_addr(&s));
            cmd_disasm(&image, base, start, end, &mode, count);
        }
        "decompile" => {
            if pos.len() != 3 {
                usage();
            }
            let image = load(pos[1]);
            let base = opt(&args, "--base").map(|s| parse_addr(&s)).unwrap_or(ARM9_BASE);
            let entry = opt(&args, "--entry").map(|s| parse_addr(&s)).unwrap_or(ARM9_ENTRY);
            let entry_thumb = opt(&args, "--entry-mode").map(|s| s == "thumb").unwrap_or(false);
            let max_insns = opt(&args, "--max-insns").map(|s| parse_addr(&s) as usize).unwrap_or(20_000);
            let max_funcs = opt(&args, "--max-funcs").map(|s| parse_addr(&s) as usize).unwrap_or(20_000);
            let seeds = opt(&args, "--seeds");
            cmd_decompile(&image, base, entry, entry_thumb, Path::new(pos[2]), max_insns, max_funcs, seeds);
        }
        "recompile-entry" => {
            if pos.len() != 3 {
                usage();
            }
            let image = load(pos[1]);
            let base = opt(&args, "--base").map(|s| parse_addr(&s)).unwrap_or(ARM9_BASE);
            let entry = opt(&args, "--entry").map(|s| parse_addr(&s)).unwrap_or(ARM9_ENTRY);
            let max_insns = opt(&args, "--max-insns").map(|s| parse_addr(&s) as usize).unwrap_or(20_000);
            cmd_recompile_entry(&image, base, entry, Path::new(pos[2]), max_insns);
        }
        "recompile-all" => {
            if pos.len() != 3 {
                usage();
            }
            let image = load(pos[1]);
            let base = opt(&args, "--base").map(|s| parse_addr(&s)).unwrap_or(ARM9_BASE);
            let entry = opt(&args, "--entry").map(|s| parse_addr(&s)).unwrap_or(ARM9_ENTRY);
            let max_insns = opt(&args, "--max-insns").map(|s| parse_addr(&s) as usize).unwrap_or(20_000);
            let max_funcs = opt(&args, "--max-funcs").map(|s| parse_addr(&s) as usize).unwrap_or(50_000);
            let seeds = opt(&args, "--seeds");
            cmd_recompile_all(&image, base, entry, seeds, Path::new(pos[2]), max_insns, max_funcs);
        }
        _ => usage(),
    }
}

// ----------------------------------------------------------------- disasm ---

fn cmd_disasm(image: &[u8], base: u32, start: u32, end: Option<u32>, mode: &str, count: usize) {
    set_image(image, base);
    let text_end = base.wrapping_add(image.len() as u32);
    let stop = end.unwrap_or_else(|| text_end.min(start.wrapping_add((count as u32) * 4)));
    if stop <= start {
        die("empty disassembly range");
    }
    let mut addr = start;
    let mut n = 0usize;
    while addr < stop && addr < text_end {
        match mode {
            "arm" => {
                if addr as usize + 4 > base as usize + image.len() {
                    break;
                }
                let off = (addr - base) as usize;
                let w = u32::from_le_bytes(image[off..off + 4].try_into().unwrap());
                let insn = decode_arm(w, addr);
                println!("  {:08x}:\t{:08x} \t{}", addr, w, insn.text);
                addr = addr.wrapping_add(4);
                n += 1;
            }
            "thumb" => {
                let Some((h, h2, size)) = thumb_fetch(image, base, addr) else {
                    break;
                };
                let insn = decode_thumb(h, h2, addr, size);
                // binutils prints a Thumb BL/BLX pair as one line whose word
                // column is the first halfword and whose text starts with the
                // second: `f7ff fffe \tbl 0x...`.
                if size == 4 {
                    println!("  {:08x}:\t{:04x} {:04x} \t{}", addr, h, h2, insn.text);
                } else {
                    println!("  {:08x}:\t{:<9} \t{}", addr, format!("{:04x}", h), insn.text);
                }
                addr = addr.wrapping_add(size as u32);
                n += 1;
            }
            _ => die("--mode must be arm or thumb"),
        }
    }
    eprintln!("# {} instructions 0x{:08X}..0x{:08X} ({})", n, start, addr, mode);
}

// -------------------------------------------------------------- decompile ---

fn cmd_decompile(
    image: &[u8],
    base: u32,
    entry: u32,
    entry_thumb: bool,
    out_dir: &Path,
    max_insns: usize,
    max_funcs: usize,
    seeds_file: Option<String>,
) {
    set_image(image, base);
    let text_end = base.wrapping_add(image.len() as u32);
    let entry_mode = if entry_thumb { recover::Mode::Thumb } else { recover::Mode::Arm };
    let key = recover::FuncKey { addr: entry & !1, mode: entry_mode };
    let mut seed_list: Vec<recover::FuncKey> = vec![key];
    let mut from_tsv = 0usize;
    if let Some(p) = &seeds_file {
        for k in load_seeds(p) {
            from_tsv += 1;
            if !seed_list.contains(&k) {
                seed_list.push(k);
            }
        }
    }
    let rec = recover::recover_seeds(image, base, text_end, &seed_list, max_insns, max_funcs);
    let total = rec.funcs.len();
    let arm = rec.funcs.keys().filter(|k| k.mode == recover::Mode::Arm).count();
    let thumb = total - arm;
    let bytes = recover::coverage_bytes(&rec.funcs);
    let jt: usize = rec.funcs.values().map(|f| f.jump_tables).sum();
    let unresolved: usize = rec
        .funcs
        .values()
        .map(|f| {
            f.indirects
                .iter()
                .filter(|(_, d)| d.starts_with("indirect branch (unresolved)"))
                .count()
        })
        .sum();
    let missing = recover::missing_seeds(&seed_list, &rec.funcs);

    fs::create_dir_all(out_dir).unwrap_or_else(|e| die(&format!("mkdir {}: {}", out_dir.display(), e)));

    // function table
    let mut tsv = String::from("entry\tmode\tinsns\tbytes\tblocks\tcalls\tindirects\tundef\n");
    let mut listing = String::new();
    listing.push_str(&format!(
        "# nnktool decompile — {} recovery from entry 0x{:08X}{}\n# image 0x{:08X} + 0x{:X} bytes; recovered {} functions ({} arm, {} thumb); {} jump tables expanded\n\n",
        if from_tsv > 0 { "seeded" } else { "reachability-guided" },
        entry,
        if from_tsv > 0 {
            format!(" + {} seeds from {}", from_tsv, seeds_file.as_deref().unwrap_or("?"))
        } else {
            String::new()
        },
        base,
        image.len(),
        total,
        arm,
        thumb,
        jt
    ));
    for (k, f) in &rec.funcs {
        tsv.push_str(&format!(
            "0x{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
            k.addr,
            k.mode.name(),
            f.lines.len(),
            f.bytes(),
            f.blocks.len(),
            f.calls.len(),
            f.indirects.len(),
            u8::from(f.hit_undef)
        ));
        listing.push_str(&format!(
            "== sub_{:08X}_{} — {} insns, {} bytes, {} blocks, {} calls ==\n",
            k.addr,
            k.mode.name(),
            f.lines.len(),
            f.bytes(),
            f.blocks.len(),
            f.calls.len()
        ));
        for (addr, (words, text)) in &f.lines {
            let enc: Vec<String> = words.iter().map(|w| format!("{:08x}", w)).collect();
            listing.push_str(&format!("  {:08x}: {:<10} {}\n", addr, enc.join(" "), text));
        }
        for (addr, why) in &f.indirects {
            listing.push_str(&format!("  ; indirect 0x{:08X}: {}\n", addr, why));
        }
        for c in &f.calls {
            listing.push_str(&format!("  ; calls 0x{:08X}_{}\n", c.addr, c.mode.name()));
        }
        listing.push('\n');
    }

    let tsv_path = out_dir.join("arm9_functions.tsv");
    fs::write(&tsv_path, &tsv).unwrap_or_else(|e| die(&format!("write {}: {}", tsv_path.display(), e)));
    let list_path = out_dir.join("arm9_decompile.txt");
    fs::write(&list_path, &listing).unwrap_or_else(|e| die(&format!("write {}: {}", list_path.display(), e)));

    println!("recovered {} functions from entry 0x{:08X} ({} arm, {} thumb)", total, entry, arm, thumb);
    if from_tsv > 0 {
        println!(
            "seeds: {} from {} + entry -> {} unique; seeds with an empty body: {}",
            from_tsv,
            seeds_file.as_deref().unwrap_or("?"),
            seed_list.len(),
            missing.len()
        );
        if !missing.is_empty() && missing.len() <= 12 {
            println!(
                "  empty seeds: {}",
                missing
                    .iter()
                    .map(|k| format!("0x{:08X}_{}", k.addr, k.mode.name()))
                    .collect::<Vec<_>>()
                    .join(", ")
            );
        }
    }
    println!("jump tables expanded: {}; unresolved indirect sites: {}", jt, unresolved);
    println!("instruction bytes in recovered bodies: {} of 0x{:X} image bytes ({:.2}%)",
        bytes, image.len(), 100.0 * bytes as f64 / image.len() as f64);
    println!("truncated: {}", rec.truncated);
    println!("wrote {} and {}", tsv_path.display(), list_path.display());
}

/// Seed keys from Ghidra's exported function table (columns:
/// `entry, name, body_bytes, is_thunk, cc, insn_len`).  `insn_len == 2`
/// marks a Thumb entry; everything else is ARM.
fn load_seeds(path: &str) -> Vec<recover::FuncKey> {
    let text = fs::read_to_string(path)
        .unwrap_or_else(|e| die(&format!("cannot read seed file {}: {}", path, e)));
    let mut seen: Vec<recover::FuncKey> = Vec::new();
    for (i, line) in text.lines().enumerate() {
        let line = line.trim_end();
        if line.is_empty() {
            continue;
        }
        let mut it = line.split('\t');
        let entry = it.next().unwrap_or("");
        if i == 0 && entry.eq_ignore_ascii_case("entry") {
            continue;
        }
        let _name = it.next();
        let _body = it.next();
        let _thunk = it.next();
        let _cc = it.next();
        let insn_len = it.next().unwrap_or("4").trim().to_string();
        let hex = entry.trim().trim_start_matches("0x");
        let addr = u32::from_str_radix(hex, 16)
            .unwrap_or_else(|_| die(&format!("bad seed entry address {:?} in {}", entry, path)));
        let mode = if insn_len == "2" { recover::Mode::Thumb } else { recover::Mode::Arm };
        let k = recover::FuncKey { addr: addr & !1, mode };
        if !seen.contains(&k) {
            seen.push(k);
        }
    }
    seen
}

// ------------------------------------------------------- recompile-all ---

fn cmd_recompile_all(
    image: &[u8],
    base: u32,
    entry: u32,
    seeds_file: Option<String>,
    out_c: &Path,
    max_insns: usize,
    max_funcs: usize,
) {
    set_image(image, base);
    let mut seed_list: Vec<recover::FuncKey> = vec![recover::FuncKey {
        addr: entry & !1,
        mode: recover::Mode::Arm,
    }];
    let mut from_tsv = 0usize;
    if let Some(p) = &seeds_file {
        for k in load_seeds(p) {
            from_tsv += 1;
            if !seed_list.contains(&k) {
                seed_list.push(k);
            }
        }
    }
    let stats = recompile::recompile_all(
        image,
        base,
        &seed_list,
        entry,
        out_c,
        max_insns,
        max_funcs,
    )
    .unwrap_or_else(|e| die(&e));
    let img_out = out_c.with_extension("image.bin");
    fs::write(&img_out, image).unwrap_or_else(|e| die(&format!("write {}: {}", img_out.display(), e)));

    println!(
        "recompile-all: {} seeds ({} from {}) -> {} functions ({} arm, {} thumb), {} cases, {} bytes C",
        stats.seeds, from_tsv, seeds_file.as_deref().unwrap_or("-"),
        stats.funcs, stats.funcs_arm, stats.funcs_thumb, stats.insns, stats.bytes
    );
    println!(
        "cases: {} direct calls, {} returns, {} runtime-indirect dispatches, {} duplicate addresses skipped",
        stats.calls, stats.returns, stats.indirects, stats.dup_cases
    );
    println!(
        "jump tables expanded: {}; unresolved indirect sites: {}; thumb instructions NOT translated: {}",
        stats.jump_tables, stats.unresolved_sites, stats.thumb_insns
    );
    if stats.swi_sites == 0 {
        println!("SWI sites in the recovered set: 0 (the HLE dispatcher is present but reports no SWI instruction)");
    } else {
        println!(
            "SWI sites in the recovered set: {} -> {}",
            stats.swi_sites,
            stats
                .swis
                .iter()
                .map(|(i, a)| format!("svc 0x{:06X} @0x{:08X}", i, a))
                .collect::<Vec<_>>()
                .join(", ")
        );
    }
    println!("wrote {} and {}", out_c.display(), img_out.display());
    println!(
        "build: cc -O2 -o arm9_all {} && ./arm9_all {}",
        out_c.display(),
        img_out.display()
    );
}

// -------------------------------------------------------- recompile-entry ---

fn cmd_recompile_entry(image: &[u8], base: u32, entry: u32, out_c: &Path, max_insns: usize) {
    set_image(image, base);
    let stats = recompile::recompile_entry(image, base, entry, out_c, max_insns)
        .unwrap_or_else(|e| die(&e));
    // Also drop the guest image next to the C so the compiled binary can load it.
    let img_out = out_c.with_extension("image.bin");
    fs::write(&img_out, image).unwrap_or_else(|e| die(&format!("write {}: {}", img_out.display(), e)));
    println!(
        "generated {} ({} entry insns, {} call shims, {} indirect sites, {} halt sites, {} bytes C)",
        out_c.display(), stats.insns, stats.calls_shimmed, stats.indirects, stats.halts, stats.bytes
    );
    println!("wrote guest image copy {}", img_out.display());
    println!("build: cc -O2 -o entry_recompiled {} && ./entry_recompiled {}", out_c.display(), img_out.display());
}

fn inspect(rom: &[u8], full: bool) {
    let h = Header::parse(rom);
    let fnt = parse_fnt(&rom[h.fnt_off as usize..(h.fnt_off + h.fnt_size) as usize]);
    let fat_n = (h.fat_size / 8) as usize;
    let ovt9 = parse_ovt(&rom[h.ovt9_off as usize..(h.ovt9_off + h.ovt9_size) as usize]);
    let ovt7 = parse_ovt(&rom[h.ovt7_off as usize..(h.ovt7_off + h.ovt7_size) as usize]);

    println!("== NDS header ==");
    println!("title              {}", h.title);
    println!("gamecode           {}", h.gamecode);
    println!("makercode          {}", h.makercode);
    println!("unit_code          0x{:02X}", h.unit_code);
    println!("encryption_seed    0x{:02X}", h.seed_select);
    println!("device_capacity    2^{} bytes (0x{:02X})", h.device_capacity + 20, h.device_capacity);
    println!("region             0x{:02X}", h.region);
    println!("version            0x{:02X}", h.version);
    println!("autostart          0x{:02X}", h.autostart);
    println!("rom_size_or_rsa    0x{:08X}", h.rom_size_or_rsa);
    println!("header_size        0x{:08X}", h.header_size);
    println!("header_crc         0x{:04X}", h.header_crc);
    println!("icon_banner_off    0x{:08X}", h.banner_off);
    println!("normal_cmd         0x{:08X}", h.normal_cmd);
    println!("key1_cmd           0x{:08X}", h.key1_cmd);
    println!("secure_crc         0x{:04X}", h.secure_crc);
    println!("secure_delay       0x{:04X}", h.secure_delay);
    println!("arm9_hookram       0x{:08X}", h.arm9_hook);
    println!("arm7_hookram       0x{:08X}", h.arm7_hook);
    if h.debug_off != 0 {
        println!("debug_rom          0x{:08X} +0x{:08X} @0x{:08X}", h.debug_off, h.debug_size, h.debug_ram);
    }
    println!();
    println!("== ARM9 ==");
    println!("arm9.rom_offset    0x{:08X}", h.arm9_rom);
    println!("arm9.entry         0x{:08X}", h.arm9_entry);
    println!("arm9.ram           0x{:08X}", h.arm9_ram);
    println!("arm9.size          0x{:08X} ({} bytes)", h.arm9_size, h.arm9_size);
    println!();
    println!("== ARM7 ==");
    println!("arm7.rom_offset    0x{:08X}", h.arm7_rom);
    println!("arm7.entry         0x{:08X}", h.arm7_entry);
    println!("arm7.ram           0x{:08X}", h.arm7_ram);
    println!("arm7.size          0x{:08X} ({} bytes)", h.arm7_size, h.arm7_size);
    println!();
    println!("== FNT ==");
    println!("fnt.offset         0x{:08X}", h.fnt_off);
    println!("fnt.size           0x{:08X} ({} bytes)", h.fnt_size, h.fnt_size);
    println!("fnt.dirs           {}", fnt.num_dirs);
    println!("fnt.files          {}", fnt.num_files);
    println!();
    println!("== FAT ==");
    println!("fat.offset         0x{:08X}", h.fat_off);
    println!("fat.size           0x{:08X} ({} bytes)", h.fat_size, h.fat_size);
    println!("fat.entries        {}", fat_n);
    let referenced: Vec<bool> = {
        let mut r = vec![false; fat_n];
        for d in 0..fnt.num_dirs {
            for e in &fnt.entries[d] {
                if !e.is_dir && (e.id as usize) < fat_n {
                    r[e.id as usize] = true;
                }
            }
        }
        r
    };
    let unref: Vec<usize> = referenced.iter().enumerate().filter(|(_, &x)| !x).map(|(i, _)| i).collect();
    println!("fat.unreferenced   {} ids: {:?}", unref.len(), unref);
    println!();
    println!("== overlay table (ARM9) ==");
    println!("ovt9.offset        0x{:08X}", h.ovt9_off);
    println!("ovt9.size          0x{:08X} ({} bytes)", h.ovt9_size, h.ovt9_size);
    println!("ovt9.entries       {}", ovt9.len());
    println!("== overlay table (ARM7) ==");
    println!("ovt7.offset        0x{:08X}", h.ovt7_off);
    println!("ovt7.size          0x{:08X} ({} bytes)", h.ovt7_size, h.ovt7_size);
    println!("ovt7.entries       {}", ovt7.len());
    println!();
    println!("== ARM9 overlays ==");
    println!("{:>4} {:>10} {:>10} {:>8} {:>10} {:>10} {:>6} {:>5} {:>4} {:>6}",
        "id", "ram", "ramsize", "bss", "initstart", "initend", "file", "csize", "flag", "comp");
    for o in &ovt9 {
        println!("{:>4} 0x{:08X} 0x{:08X} 0x{:06X} 0x{:08X} 0x{:08X} {:>6} {:>5} 0x{:02X} {:>6}",
            o.id, o.ram, o.ram_size, o.bss, o.init_start, o.init_end,
            o.file_id, o.compressed_size, o.flags, o.flags & 1);
    }
    if !ovt7.is_empty() {
        println!();
        println!("== ARM7 overlays ==");
        for o in &ovt7 {
            println!("{:>4} 0x{:08X} 0x{:08X} 0x{:06X} {:>6} 0x{:02X}",
                o.id, o.ram, o.ram_size, o.bss, o.file_id, o.flags);
        }
    }
    println!();
    println!("== top-level FNT entries ==");
    for e in &fnt.entries[0] {
        if e.is_dir {
            println!("DIR  {:<24} 0x{:04X}", e.name, e.id);
        } else {
            println!("FILE {:<24} id={}", e.name, e.id);
        }
    }
    println!("root: {} entries ({} dirs, {} files)",
        fnt.entries[0].len(),
        fnt.entries[0].iter().filter(|e| e.is_dir).count(),
        fnt.entries[0].iter().filter(|e| !e.is_dir).count());
    if full {
        println!();
        println!("== full tree ==");
        let mut lines = Vec::new();
        let mut path = String::new();
        walk_fnt(&fnt, 0, &mut path, &mut lines);
        for l in &lines {
            println!("{}", l);
        }
    }
}

fn extract(rom: &[u8], out: &Path) {
    let h = Header::parse(rom);
    fs::create_dir_all(out).unwrap_or_else(|e| die(&format!("mkdir {}: {}", out.display(), e)));

    let arm9 = &rom[h.arm9_rom as usize..(h.arm9_rom + h.arm9_size) as usize];
    let arm7 = &rom[h.arm7_rom as usize..(h.arm7_rom + h.arm7_size) as usize];
    let p = out.join("arm9.bin");
    fs::write(&p, arm9).unwrap_or_else(|e| die(&format!("write {}: {}", p.display(), e)));
    println!("wrote {} (0x{:X} bytes) from ROM 0x{:08X} -> load 0x{:08X}",
        p.display(), arm9.len(), h.arm9_rom, h.arm9_ram);
    let p = out.join("arm7.bin");
    fs::write(&p, arm7).unwrap_or_else(|e| die(&format!("write {}: {}", p.display(), e)));
    println!("wrote {} (0x{:X} bytes) from ROM 0x{:08X} -> load 0x{:08X}",
        p.display(), arm7.len(), h.arm7_rom, h.arm7_ram);

    let ovt9 = parse_ovt(&rom[h.ovt9_off as usize..(h.ovt9_off + h.ovt9_size) as usize]);
    let ovt_dir = out.join("ovt9");
    fs::create_dir_all(&ovt_dir).unwrap_or_else(|e| die(&format!("mkdir {}: {}", ovt_dir.display(), e)));
    let mut manifest = String::from("# id\tfile_id\tram\tram_size\tbss\tflags\trom_offset\trom_end\tbytes\n");
    for o in &ovt9 {
        let (s, e2) = fat_range(rom, &h, o.file_id as usize);
        let data = &rom[s..e2];
        let name = format!("ov9_{:03}_f{:05}.bin", o.id, o.file_id);
        let p = ovt_dir.join(&name);
        fs::write(&p, data).unwrap_or_else(|er| die(&format!("write {}: {}", p.display(), er)));
        manifest.push_str(&format!("{}\t{}\t0x{:08X}\t0x{:08X}\t0x{:X}\t0x{:02X}\t0x{:08X}\t0x{:08X}\t{}\n",
            o.id, o.file_id, o.ram, o.ram_size, o.bss, o.flags, s, e2, data.len()));
        println!("wrote {} (0x{:X} bytes) file#{} -> load 0x{:08X}{}",
            p.display(), data.len(), o.file_id, o.ram,
            if o.flags & 1 != 0 { "  [OVT says compressed]" } else { "" });
    }
    let p = ovt_dir.join("ovt9.tsv");
    fs::write(&p, &manifest).unwrap_or_else(|er| die(&format!("write {}: {}", p.display(), er)));
    println!("wrote {} ({} overlays)", p.display(), ovt9.len());
}

fn fat_range(rom: &[u8], h: &Header, file_id: usize) -> (usize, usize) {
    let base = h.fat_off as usize + file_id * 8;
    let s = rd32(rom, base) as usize;
    let e = rd32(rom, base + 4) as usize;
    if e < s || e > rom.len() {
        die(&format!("FAT entry {} out of range: 0x{:X}..0x{:X}", file_id, s, e));
    }
    (s, e)
}
