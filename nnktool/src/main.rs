// nnktool — Ni no Kuni (NDS, B2KJ) static-recompilation project CLI.
// No external crates (DQ8/dq8tool convention).
//
// Subcommands:
//   inspect [--full] <rom>   header + FNT/FAT + overlay tables
//   fnt     [--full] <rom>   file name table listing (default: top-level only)
//   extract <rom> <out_dir>  ARM9 / ARM7 / ARM9 overlay binaries

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
        "usage:\n  nnktool inspect [--full] <rom>\n  nnktool fnt [--full] <rom>\n  nnktool extract <rom> <out_dir>"
    );
    exit(2);
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.is_empty() {
        usage();
    }
    let cmd = args[0].as_str();
    let flags: Vec<&String> = args.iter().filter(|a| a.starts_with("--")).collect();
    let pos: Vec<&String> = args.iter().filter(|a| !a.starts_with("--")).collect();
    let full = flags.iter().any(|f| *f == "--full");

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
        _ => usage(),
    }
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
