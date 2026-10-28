# ROM_MAP.md — Ni no Kuni: Shikkoku no Madoushi (NDS, LEVEL-5, game code B2KJ)

Static map of `Ni No Kuni.nds` (512 MiB) for the static-recompilation project.
All numbers below are reproduced by `nnktool inspect` / the tools in `tools/`
(commands in §6). Nothing under `work/` is ever committed.

## 1. Identification

| field | value |
|---|---|
| title | `NINOKUNI` (header 0x000, 12 bytes, NUL-padded) |
| gamecode | `B2KJ` (0x00C) |
| makercode | `HF` (0x010) |
| unit / seed / capacity | 0x00 / 0x00 / 0x0C (2^32) |
| region / version / autostart | 0x00 / 0x00 / 0x00 |
| header size | 0x4000 (0x084), header CRC 0x3A18 (0x15E) |
| icon banner | 0x0021B600 (0x068) |
| normal / key1 card regs | 0x00416657 / 0x081808F8 |
| secure CRC / delay | 0x1DE6 / 0x0D7E |

## 2. Memory layout

### ARM9 / ARM7 images

| | ROM offset | stored size | RAM base | entry | notes |
|---|---|---|---|---|---|
| ARM9 | 0x00004000 | 0x49328 (299,816) | 0x02000000 | 0x02000800 | **code-compressed** (see §5): literal prefix = first 0x4000 bytes, RAM image after decompression = **0x6CB78 (445,304)** |
| ARM7 | 0x001AD200 | 0x26F28 (159,528) | 0x02380000 | 0x02380000 | stored raw (decompressed == stored) |

* `arm9_hookram` (0x06C) = 0x02000A74, `arm7_hookram` (0x070) = 0x02380158 — the ARM9 image's entry
  code sets up CPU **modes/stacks** (three `msr cpsr_*` writes at 0x0200081C/0x02000830/0x02000858) and
  uses I/O base `0x04000000` (`mov ip, #0x04000000` at 0x02000800).
* Entry state is **ARM** (entry address even); the first Thumb decode of the same bytes is nonsense
  (`stm r3!, {r0}` / `b ...`), so disassembly must start in ARM state.

### ARM9 overlay table (0x050: 0x0004D400, size 0x2A0 = 21 × 32; ARM7 overlay table: none)

All 21 overlays are stored code-compressed (OVT flag byte 0x01, `compressedSize` = stored size) and
decompress to **exactly** their OVT `ramSize`. All share 4 load arenas:

| arena (RAM base) | overlay ids |
|---|---|
| 0x02079F80 | 0–13 |
| 0x020C18E0 | 14–16 |
| 0x020D0A00 | 17–18 |
| 0x02159FE0 | 19–20 |

| id | ram | ramSize | bss | staticInit (start/end) | file id | stored | flags |
|---|---|---|---|---|---|---|---|
| 0 | 0x02079F80 | 0x026120 | 0xA0 | 0x0209DBC4/0x0209DBCC | 0 | 0x175CC | 0x01 |
| 1 | 0x02079F80 | 0x046D80 | 0 | 0x020BE75C/0x020BE760 | 1 | 0x2CB98 | 0x01 |
| 2 | 0x02079F80 | 0x02F7A0 | 0x6D40 | 0x020A69F4/0x020A69FC | 2 | 0x1DA34 | 0x01 |
| 3 | 0x02079F80 | 0x01CD00 | 0x80 | 0x02093F54/0x02093F5C | 3 | 0x10378 | 0x01 |
| 4 | 0x02079F80 | 0x021200 | 0x20 | 0x02097768/0x02097774 | 4 | 0x11E10 | 0x01 |
| 5 | 0x02079F80 | 0x01A100 | 0x20 | 0x02092CA0/0x02092CA4 | 5 | 0xF048 | 0x01 |
| 6 | 0x02079F80 | 0x004CA0 | 0 | 0x0207E730/0x0207E734 | 6 | 0x3000 | 0x01 |
| 7 | 0x02079F80 | 0x0478C0 | 0xA0 | 0x020BDA00/0x020BDA04 | 7 | 0x2A5D8 | 0x01 |
| 8 | 0x02079F80 | 0x020960 | 0x19A0 | 0x02097D38/0x02097D3C | 8 | 0x12F10 | 0x01 |
| 9 | 0x02079F80 | 0x0074E0 | 0 | 0x02081178/0x0208117C | 9 | 0x4368 | 0x01 |
| 10 | 0x02079F80 | 0x002FA0 | 0 | 0x0207CBC8/0x0207CBCC | 10 | 0x1D74 | 0x01 |
| 11 | 0x02079F80 | 0x0027C0 | 0 | 0x0207B9EC/0x0207B9F0 | 11 | 0x1910 | 0x01 |
| 12 | 0x02079F80 | 0x004600 | 0 | 0x0207DC74/0x0207DC78 | 12 | 0x2C94 | 0x01 |
| 13 | 0x02079F80 | 0x011000 | 0x480 | 0x0208AE30/0x0208AE34 | 13 | 0x7BD4 | 0x01 |
| 14 | 0x020C18E0 | 0x00B5C0 | 0x30E0 | 0x020CC4DC/0x020CC4F0 | 14 | 0x71F4 | 0x01 |
| 15 | 0x020C18E0 | 0x005E80 | 0x40 | 0x020C74A4/0x020C74A8 | 15 | 0x3BD8 | 0x01 |
| 16 | 0x020C18E0 | 0x00BBE0 | 0x3540 | 0x020CC758/0x020CC75C | 16 | 0x7760 | 0x01 |
| 17 | 0x020D0A00 | 0x072700 | 0x16EE0 | 0x0213A26C/0x0213A290 | 17 | 0x4BAC8 | 0x01 |
| 18 | 0x020D0A00 | 0x025E40 | 0x26C0 | 0x020F52A4/0x020F52A8 | 18 | 0x17DD0 | 0x01 |
| 19 | 0x02159FE0 | 0x0021E0 | 0x20 | 0x0215C184/0x0215C188 | 19 | 0x20E4 | 0x01 |
| 20 | 0x02159FE0 | 0x001FA0 | 0 | 0x0215BD70/0x0215BD74 | 20 | 0x162C | 0x01 |

### File-system tables

| table | offset | size | contents |
|---|---|---|---|
| FNT | 0x001D4200 | 0x0002E69A | 888 directory records (8 B each) + entry area |
| FAT | 0x00202A00 | 0x00018A30 | 12,614 × (start, end) u32 pairs |

## 3. FNT layout (verified, not assumed)

* Main table: `numDirs = rec0.entriesOff / 8 = 0x1BC0 / 8 = 888`; record = `u32 entriesOff,
  u16 firstFileID, u16 parentDirID (0xF000+parent; root's third field = 888 = numDirs)`.
* Entry area record: first byte is a `u8 control` — `bit7 = directory`, `low7 = name length` — then
  that many name bytes (no NUL); directories then carry an explicit `u16 dirID (0xF000+index)`,
  files carry **no id** (implicit `firstFileID + running file index`); a `0x00` byte terminates the
  directory. Verified against every one of the 888 dirs: entry runs are byte-contiguous, the last run
  ends exactly at `FNT + fntSize`, all 888 dirs are reachable from the root, and every record's
  parent field matches the tree walk.
* Counts: 12,593 FNT-named files (ids 21…12613) + 21 unreferenced FAT ids (0…20) = 12,614 FAT
  entries. FAT ids 0…20 are exactly the 21 ARM9 overlay file ids — `arm9`/`arm7` are *not* FAT
  files (they are referenced directly by the header at 0x020/0x030).

### Top-level FNT entries (root, dir 0xF000)

| entry | type | dir id | recursive files |
|---|---|---|---|
| `data` | DIR | 0xF001 | 12,592 |
| `dwc` | DIR | 0xF377 | 1 (Nintendo WFC lobby data) |

(root has **0 files**; 888 dirs total, 13 direct children under `data/`):

| data/… | recursive files | direct subdirs |
|---|---|---|
| Battle | 658 | 15 |
| Casino | 278 | 2 |
| credit | 48 | 0 |
| Event | 3,080 | 4 |
| font | 13 | 0 |
| map | 2,743 | 8 |
| mdl | 2,401 | 13 |
| movie | 32 | 0 |
| SkyBattle | 104 | 8 |
| sound | 1,513 | 3 |
| System | 42 | 14 |
| test | 2 | 0 |
| UI | 1,678 | 6 |

## 4. Compression-signature sniff (tools/compression_sniff.py)

First 4 bytes of all 12,614 FAT entries:

| signature | files | meaning |
|---|---|---|
| `NPCK` | 8,960 (71.0%) | LEVEL-5 pack container (.n3d 5,833 / .n2d 1,894 / .npd 1,229 …) |
| `KPCN` | 146 | byte-swapped `NPCK` (inside .bin) |
| `BCA0` | 1,085 | NSBCA animation variant wrapper |
| `TMAP` / `WMAP` | 303 / 16 | map tile / world map containers |
| `sadl` / `smdl` / `swdl` / `spdl` | 238 / 6 / 6 / 43 | Nintendo sound streams (.SAD/.SMD/.SWD/.SPD) |
| `LZ11` (`0x11` + plausible 24-bit size) | 141 | plain LZ11 (47 .nsbca, 44 .bin, 28 .ncoll, 16 .nsbtx, 4 .nsbma) |
| `LZ10` (`0x10` + plausible size) | **3 → all false positives** | id 1/17 are overlay Thumb code (`10 b5…`), id 85 rejected by `ndspy.lz10` → **no plain LZ10 files** |
| `NCv2` / `RTFN` / `MODS` / `BMD0` | 136 / 13 / 17 / 1 | collision / font / sound / model headers |
| **`pac` marker** | **0** | no literal `pac` magic anywhere in the file table — the LEVEL-5 pack marker present is `NPCK` |

Inside `NPCK` (payload block at header +0x3C): first byte `0x11` (LZ11-style) in 4,975 of 8,956
files, `0x00` 1,511, literal (`B`,`C`,`M`,`E`…) in 1,751 — i.e. NPCK packs frequently wrap LZ11.

**Code compression (footer-based, invisible to a first-byte sniff):**

| file | stored | after `ndspy.codeCompression.decompress` | verdict |
|---|---|---|---|
| arm9.bin | 0x49328 | 0x6CB78 | code-LZ (reversed LZ77 + 0xFF footer); literal prefix 0x4000 |
| arm7.bin | 0x26F28 | 0x26F28 | raw |
| 21 ARM9 overlays (FAT ids 0…20) | — | 21/21 decompressed | all code-LZ, each lands exactly on its OVT `ramSize` |

## 5. ARM9 entry disassembly — three-way cross-check (tools/disasm_check.py)

First 64 instructions at 0x02000800 (image offset 0x800):

* decoders: `capstone` 5.0.7 (ARM mode) vs `arm-none-eabi-objdump` (binutils 2.42) vs
  `llvm-mc` (LLVM 18) re-assembly of the capstone text → object → `llvm-objdump`:
  **64/64 identical encodings** (word, address and order), `RESULT: PASS`.
* entry state: **ARM** (`entry & 1 == 0`; first insn `E3A0C301 = mov ip, #0x04000000`; decoding the
  same bytes as Thumb gives garbage), so no Thumb transition at entry — the window contains only
  CPSR mode switches (`msr cpsr_c / cpsr_fsxc`), no `bx`/`blx`.
* The first 64 instructions (bytes 0x800–0x8FF) are **byte-identical** in the stored and the
  decompressed ARM9 image (stored/decompressed first differ at 0x4000), so this disassembly is the
  real RAM code. Full table: `work/entry_disasm_check.txt`.

## 6. Ghidra project databases

* `work/ghidra/nnk.gpr` — programs:
  * `arm9` = **decompressed** RAM image (0x6CB78 B) at `0x02000000`, `ARM:LE:32:v5`
  * `arm7` = raw image (0x26F28 B) at `0x02380000`, `ARM:LE:32:v5`
  * both imports + auto-analysis completed (RC=0): `work/ghidra_import_arm9_dec.log`,
    `work/ghidra_import_arm7_dec.log`
* `work/ghidra_stored_view/` — earlier run against the *stored* (compressed) ARM9 slice, kept only
  as evidence of where the compressed tail begins (code ≥0x4000 there is compressed bytes); do not
  analyze it.

## 7. Reproducible commands

```bash
cd /home/hermes/NiNoKuni

# 1) build the project CLI (no external crates)
cd nnktool && cargo build --release && cd ..

# 2) header + FNT/FAT + overlay tables (reproduces every number in §1–§3)
./nnktool/target/release/nnktool inspect "Ni No Kuni.nds"

# 3) full file tree / raw extraction (ARM9, ARM7, 21 overlay9 binaries)
./nnktool/target/release/nnktool fnt --full "Ni No Kuni.nds" > work/fnt_full.txt
./nnktool/target/release/nnktool extract "Ni No Kuni.nds" work

# 4) decompressed RAM images (arm9, arm7, overlays) + OVT ramSize verification
/usr/bin/python3 tools/extract_decompressed.py "Ni No Kuni.nds"

# 5) three-way entry disassembly cross-check (capstone / binutils / llvm-mc)
/usr/bin/python3 tools/disasm_check.py "Ni No Kuni.nds" 64

# 6) compression-signature sniff of the file table
/usr/bin/python3 tools/compression_sniff.py "Ni No Kuni.nds" > work/compression_sniff.txt

# 7) Ghidra headless (minutes each; project lands in work/ghidra/)
mkdir -p work/ghidra_src
cp work/arm9_dec.bin work/ghidra_src/arm9.bin
cp work/arm7_dec.bin work/ghidra_src/arm7.bin
/home/hermes/tools/ghidra/support/analyzeHeadless work/ghidra nnk \
  -import work/ghidra_src/arm9.bin -loader BinaryLoader \
  -loader-baseAddr 0x02000000 -processor ARM:LE:32:v5 -analysisTimeoutPerFile 1200
/home/hermes/tools/ghidra/support/analyzeHeadless work/ghidra nnk \
  -import work/ghidra_src/arm7.bin -loader BinaryLoader \
  -loader-baseAddr 0x02380000 -processor ARM:LE:32:v5 -analysisTimeoutPerFile 900
```

Python tools must run with `/usr/bin/python3` (ndspy + capstone come from `pip --user`; the venv
`python3` on `$PATH` does not see them). No emulator runs were used for any of this.

## 8. Data hygiene

`.gitignore` excludes the ROM (`*.nds`), `bios/`, everything under `work/` (extracted binaries,
Ghidra projects, evidence logs) and `nnktool/target/` — only source, tools and docs are committed.
