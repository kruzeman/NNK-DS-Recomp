# PHASE-5AJ — the card image is the ONLY input

## 1. The defect (measured, not asserted)

Phase-5AH removed the decoded-movie dependency: the runtime decodes
`data/movie/*.mods` out of the card image itself, bit-exact against FFmpeg.  The
shipped binary still needed three files DERIVED off-line from the ROM, so a
clean directory holding only the ROM and the runner could not boot:

```
$ cd /tmp/clean_rom        # Ni No Kuni.nds + nnk_runner only
$ ./nnk_runner arm9_stored_padded.bin
cannot open guest image arm9_stored_padded.bin
```

* argv[1] (`arm9_stored_padded.bin`, 445,304 B) — the image the dispatch loop
  loads into main RAM;
* `NNK_SMC_IMAGE` `0x02000000:work/ghidra_src/arm9.bin` — the recompiler's ARM9
  input, i.e. the SMC generation-0 words every compiled case was built from;
* `NNK_SMC_IMAGE` `0x020C0000:work/phase3a/ovl_p4f4.bin` — the arena window the
  overlay cases were built from.

The operator requirement ("все должно браться из рома, а не с внешних мест")
makes those three a defect, not a convenience: the shipped program must take a
`.nds` and nothing else.

## 2. What is actually derived (measured against the off-line references)

| # | image | how | byte-identical to |
|---|---|---|---|
| 1 | guest image, 445,304 B @ `0x02000000` | ROM header ARM9 slice `0x4000+0x49328`, zero-padded | `work/phase3a/arm9_stored_padded.bin`, md5 `c9cf9596def17a08a9aeee259c877890` |
| 2 | decompressed ARM9, 445,304 B | DS code compression (reversed LZ77 + 0xFF footer) of the same slice | `work/ghidra_src/arm9.bin` (= the recompiler input), md5 `2a19d5a14be083b40f07c8f80f5deb94` |
| 3 | arena window, 786,432 B @ `0x020C0000` | zero window + overlay 17 decompressed at its OVT base `0x020D0A00` | `work/phase3a/ovl_p4f4.bin` except 8,929 B of loader-written data (§2.3) |

### 2.1 Image 1 is the stored slice, not the decompressed one

MEASURED: `arm9_stored_padded.bin` equals `ROM[0x4000 .. 0x4000+0x49328]` for
all 299,816 bytes and is ZERO for the remaining 145,488 (the file is exactly
`0x6CB78`, the DISPATCH image size).  That is not an accident of packing: the
guest decompresses its own ARM9 in place at boot (the decompressor whose flag
stream sits at `0x0200098C`, completing at the auto-load walker entry
`0x020009FC`), which is precisely why the SMC snapshot has to notice the change.
So derivation 1 is a header read plus a `memcpy` into a zeroed window — the
decompressor is needed for derivation 2, not for booting.

### 2.2 The decompressor is the shipped one

`tools/nnk_rom.inc: nnk_code_decompress()` implements Nintendo "code
compression" as GBATEK/DSDecmp/ndspy describe it: the footer is the last
`headerLen` bytes (`[pad 0xFF][extraSize u32][compLen u24 | headerLen u8]`), the
stream is read BACK TO FRONT, flags are consumed from `data[-1-readBytes]` with
masks `0x80..0x01`, and a set flag means `len=(b1>>4)+3`,
`disp=(((b1&0xF)<<8)|b2)+3`, with ndspy's `disp>written → disp=2` clamp at the
tail.  Bytes before the compressed region are a literal passthrough prefix.

Measured on this ROM (ARM9): stored `0x49328`, `extraSize` `0x23850`,
`headerLen` 11, `compLen` `0x45328`, passthrough `0x3E00` → output
`0x3E00 + 0x81F58 = 0x6CB78 = 445,304` and md5 `2a19d5a1…`, i.e. identical to
`work/ghidra_src/arm9.bin`.  All 21 overlays use the same format and land
exactly on their OVT `ramSize` (as 5AF/DSD_OVERLAYS.md measured).

`tools/rom_derive_selftest.c` compiles the RUNTIME fragment (not a copy) on the
host and dumps what it derives; that is the cross-check the class-46 test and
this document quote.

### 2.3 The arena window: what a ROM cannot reproduce, and why it does not matter

`ovl_p4f4.bin` is a snapshot of live RAM at the first dispatch, so it contains
what the GUEST wrote after the loader copied overlay 17: 8,929 bytes of the
module's `data`/`bss` (the largest run is 9,216 B at `0x02159EE0`, overlay 19's
static-init table; plus the 16-byte ctor list inside the OVT sinit range
`0x0213A26C..0x0213A290`).  Those are DATA, never decoded as instructions, and
the derived window holds the ROM's own (zero) fill there.  Everything the
dispatch image compiled from overlay 17 — its 468,720 bytes of code — is
byte-identical.

Consequence: a PC inside one of those data bands would be classified stale and
re-decoded from live RAM (which is the correct instruction anyway).  No PC
executes there; §5's run measures the same speed as 5AE, which is the empirical
form of that statement.

### 2.4 The SMC generation-0 segments

`smc_arm()` now builds its snapshot from the card image when `NNK_SMC_IMAGE` is
unset: segment 0 = derivation 2 at `0x02000000`, segment 1 = derivation 3 at
`0x020C0000` (0xC0000 B).  `NNK_SMC_IMAGE` still parses files exactly as before
— for A/B diagnosis — and `NNK_SMC_GEN0_OVL=<id>` (default 17) changes which
overlay is placed in the window.

### 2.5 ITCM / DTCM

Nothing is derived because nothing was ever loaded: the runtime models the TCMs
as zero-filled memory (`itcm[0x8000]`, `dtcm[0x4000]`) and the guest fills ITCM
with its own DMA from main RAM (`cp15 W id=0x100/0x911/0x910` in the boot log).
The dsd extract's `arm9/itcm.bin` (1,344 B) is a debug artifact; no phase's
`run` commands referenced it.

### 2.6 No first-run cache, and none needed

The whole derivation is two decompressions (445 KB + 469 KB ≈ 3 ms) plus a
`memcpy`, done once per process.  A cache would add a file outside the ROM —
i.e. exactly what this phase removes — so Phase-5AH's "no cache, no first-run
step" decision stands, and a missing/absent cache cannot crash because no cache
is consulted.

## 3. Where the change lives

| file | change |
|---|---|
| `tools/nnk_rom.inc` (new) | NITRO header parse (arm9/arm7/FNT/FAT/OVT), `mmap` of the card image, `nnk_code_decompress`, the three derivations, argv→gmem loader |
| `tools/nnk_smc.inc` | `#include "nnk_rom.inc"`; `smc_arm()` derives its segments from the ROM when `NNK_SMC_IMAGE` is unset |
| `tools/p5aj_rom_splice.py` (new) | splices the frozen dispatch image's `main()` (argv is the `.nds`) and `io_load_rom()` (the card model uses the argv image, no `NNK_ROM_PATH`) |
| `tools/build_split.sh` | runs that splice after the 5x/5AE/5AF ones (it wraps `io_load_rom`'s tail, which 5AF rewrote) and registers `5aj_*` features |
| `tools/split_dispatch.py` | copies `nnk_rom.inc` next to `nnk_main.c`; it is part of the object-cache key |
| `tools/nnk_prelude_manifest.txt` | four `required` rows (5AJ), so the prelude audit fails a build that drops the derivation |
| `nnktool/src/recompile.rs` | canonical `main()` + `io_load_rom()` carry the same text (the frozen image is spliced from it) |
| `tools/rom_derive_selftest.c` (new) | host harness for the fragment (md5/FNV cross-check) |
| `nnktool/src/decoder_regression_tests.rs` | class 46 (3 tests) |
| `README.md`, `run.sh` (new) | `./run.sh "/path/Ni No Kuni.nds"` only; overrides documented as reviewer tools |

## 4. Tests

`cargo test` → **47 passed** (was 45).  Class 46:

* `phase_5aj_rom_derivation_matches_the_references` — builds
  `tools/rom_derive_selftest.c` (which `#include`s the runtime fragment) with
  the host `cc`, (a) rejects an 8 KiB blob of `0xAB` as "not a NITRO card
  image", (b) on the real ROM pins the three derivations (`445304 /
  75cf0984527e4c41`, `445304 / 6087d11200ee29b2`, `786432 / 6d1bfd9443da9efd`)
  and compares derivations 1 and 2 byte-for-byte against
  `arm9_stored_padded.bin` / `ghidra_src/arm9.bin`.
* `phase_5aj_build_and_prelude_take_the_card_image` — the build still splices
  the transform, the canonical prelude still calls
  `nnk_rom_image_load(path, gmem, IMAGE_LEN)`, the old
  `cannot open guest image`/`fopen(argv[1])` contract is gone, and `smc_arm()`
  no longer requires `NNK_SMC_IMAGE`.
* The Phase-5AG manifest test also covers the four new rows (they are
  registered by `build_split.sh`, so they must be `required`).

`tools/prelude_audit.py` (run by the build) reports
`prelude: N/N features present` including the four 5AJ rows.

## 5. ACCEPTANCE — the clean-directory run

