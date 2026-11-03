# SAVESTATES — the NNKS container (Phase-5AQ / 5AR / 5BH)

A savestate is the **entire emulated machine**: CPU register file (including
the banked r13/r14 and SPSR per mode), NZCV, CP15/TCM configuration, the I/O
model (timers, DMA, IPC, DIV/SQRT, VRAMCNT, EXMEMCNT/POWCNT, IRQ controller),
the card model, the SMC re-decoder state, the input script cursor, the movie
HLE cursors, the 24 MiB guest RAM window, ITCM/DTCM **and** the whole display
model (VRAM banks, the 2D register image, palette, OAM, retained polygon
lists, composed screens). Restoring one continues the guest at the instruction
that was about to execute — not at the boot entry.

Everything is code in `tools/nnk_state.inc` (spliced into the runtime TU by
`tools/split_dispatch.py` next to the other fragments) plus the state snapshot
entry points the renderer gained in `gxrender/src/gx_display.c`
(`gxd_state_size/save/load/layout_fp`).

A state contains **no game bytes that are not already guest state**: RAM, VRAM
and TCM images are the emulated machine's memory, exactly what melonDS's own
save states hold. States are build artifacts: keep them out of git.

## 1. File format

Little-endian, one header, one region table, one field directory, one payload.
All integers are native-width little-endian (`uint32_t` / `uint64_t`).
The format is versioned **2** since Phase-5BH; **version 1 files are still
read** (see §1b), because the operator's bug reproducers are v1 files.

```
offset size field
     0    4 magic "NNKS"
     4    4 version                (2; 1 is read)
     8    4 header_size            (72 + 48 * region_count + 32 * total_fields)
    12    4 region_count
    16    4 build_fp               identity: region/field layout + gx layout
    20    4 flags                  NNKS_FLAG_FIELDDIR (v1: this was padding, 0)
    24    8 rom_size               identity: card image size in bytes
    32    4 rom_hdr_crc            identity: CRC32 of ROM bytes 0x000..0x1FF
    36    4 arm9_crc               identity: CRC32 of the ROM's ARM9 (header 0x20/0x2C)
    40    8 record_step            guest step counter at the save point
    48    8 record_frames          display frame counter at the save point
    56    8 payload_size
    64    4 payload_crc            CRC32 over the whole payload
    68    4 header_crc             CRC32 over bytes 0..67
    72  48*n region table, region_count entries (v2):
             u32  id               stable numeric region id (1=io … 11=dtcm)
             u32  version          LAYOUT VERSION of this region's payload
             u64  offset           offset into the payload
             u64  length
             u32  crc32            CRC32 of this region's bytes
             u32  nfields          field-directory entries for this region (0 = blob kind)
             char name[16]         NUL-padded ASCII region name
  hsize-32*F  field directory (v2), 32 bytes per entry, grouped in region order:
             char name[24]         NUL-terminated field name
             u32  off              offset INSIDE that region's payload
             u32  len
  ...        payload
```

The header is **byte-compatible with v1** (72 bytes; `flags` occupies the old
padding word), so one parser reads both versions.

Why the field directory exists: `io` is the concatenation of every generated
field row, so a static added by a later phase shifts **every later field** by
4 or 8 bytes. Matching by name through the directory is what lets an older
state restore the fields it still shares instead of refusing the file (or,
worse, copying the wrong bytes into the wrong variable — `nnks_selftest.c`
measures exactly that for the prefix policy a v1 file must use).

The region **version** is a fingerprint of the region's layout: FNV-1a over
`(field name, field length)` for the generated field tables, the renderer's
`gxd_state_layout_fp()` for `gxblob`, `sizeof(NnkStateMovie)` for the stream
table. A version *and* length match is the only thing that authorises a plain
`memcpy` of the region.

`header_crc` is checked before anything is applied, then the identity block,
then every region's own CRC, then the payload CRC. The two CRCs are
overlapping by design: a corrupt header is reported as a header problem even
when it also changes the identity fields.

## 1b. Reading a v1 file (no field directory)

A v1 region record is `{char name[16], u64 offset, u64 length, u32 crc, u32 rsv}`
(40 bytes) and there is no directory, so:

* regions are matched to the running build **by name** (v2 matches by **id**);
* a field-table region is restored **by prefix** — the documented
  "apply the fields that exist up to `min(len)`" rule — and every field past
  the stored length is reported as defaulted.  An *inserted* field therefore
  cannot be detected in a v1 file; that is why the report names the fields and
  why the format carries a directory from v2 on;
* the saved pc is recovered from the **trailing 4 bytes of `io`**:
  `nnks_pc` is the last io field in every build since Phase-5AQ (the generator
  appends its `EXTRA` rows after every scanned static), and the value is taken
  only when it lies inside the guest's executable windows
  (`0x01FF0000..0x023FFFFF`).  Measured on two operator states: `0x02007138`
  (healthy prompt) and `0x0207CEA0` (the 5BG frontier pc).  Without it a v1
  load would resume at the boot entry; with it the run continues at the saved
  instruction.


## 2. Regions

`nnks_region_len` / `nnks_region_store` / `nnks_region_load` own each region.
Regions of kind 0 are generated field tables (below); kind 1 is the display
blob; kind 2 is the movie stream cursor table.

| region | id | kind | content | length in this build |
|---|---|---|---|---|
| `io` | 1 | 0 | CPU regs/flags/banked regs/SPSR, CP15+TCM config, counters, DISPCNT/DISPSTAT, IE/IF/IME, timers, DMA, EXMEMCNT/POWCNT, DIV/SQRT, VRAMCNT, IRQ script cursor, ARM7 IPC state | 85,192 |
| `smc` | 2 | 0 | SMC generation/band tables, per-band generations, re-decode counters, sticky T-bit, conflict counters | 134,300 |
| `card` | 3 | 0 | card command latch, address/length/position/FIFO, scheduling clock, SPI/ROM control, DRQ/poll bookkeeping | 104 |
| `input` | 4 | 0 | effective key mask + EXTKEYIN, parsed frame script + cursor, injection schedule, recorder counters | 6,288 |
| `movie` | 5 | 0 | movie HLE stream count/codec cursor | 12 |
| `moviestreams` | 6 | 2 | per-stream {codec,cursor,rom,index,frames,presents,shortr} | `p5t_nstr * 28` |
| `display` | 7 | 0 | `gxd_frames`, `gxd_last_vc` | 8 |
| `gxblob` | 8 | 1 | the whole display model (VRAM banks, 2D register image, palette, OAM, latched 3D env, retained polygon lists, composed screens) | 5,270,752 |
| `ram` | 9 | 0 | the 24 MiB guest window (main RAM 4 MiB + its mirror + shared WRAM at 0x03000000) | 25,165,824 |
| `itcm` | 10 | 0 | ITCM image | 32,768 |
| `dtcm` | 11 | 0 | DTCM image | 16,384 |

Total: 7,960 (header: 72 + 11*48 + 230 field entries) + 30,711,632 =
**30,719,592 bytes** for a state saved at step 5,000,000.

Region ids are part of the file format: never renumber one, never reuse one.
The lengths above are this build's; a state whose length differs is restored
field by field (§7).

The `gxblob` length is `gxd_state_size()`: the display snapshot is a
self-describing POD image whose host pointers (`win`, `pctx`, textures, the
window back-buffer, FILE handles, decode references) are blanked on save and
rebound/re-derived on load, so a state is portable across processes and ASLR.
The movie stream cursors are re-synchronised by replaying the Mobiclip decode
from frame 0 (deterministic) rather than storing reference frames.

## 3. The field table is generated, not hand-written

`tools/nnks_gen_state_fields.py` derives the table from the **effective
prelude** at build time and writes `work/split/*/nnk_state_fields.h`:

* `$SPLIT/nnk_main.c` — everything before `static void run(void)` (the frozen
  image prelude + the spliced fragments);
* `$SPLIT/nnk_dma.inc`, `nnk_vramcnt.inc`, `nnk_input.inc`, `nnk_smc.inc`,
  `nnk_boot.inc`.

`nnk_state_fields.h` is regenerated by `tools/build_split.sh` on every build,
so a runtime static added by a later phase is carried automatically. Each row
is `NNKS_F(name)` = `{#name, sizeof(name), &name}`.

Two scanner rules matter:

* **`static` data** at any indentation is carried (functions, prototypes and
  pointers are skipped).
* **The splitter's un-static'd shared objects** — `r`, `n`, `z`, `c`, `v`,
  `spsr_bank`, `bank13`, `bank14`, `carry_out`, `cp15_reg`, `cpsr_ctl`,
  `mode_cur` — live at column 0 in `nnk_main.c`, so they are NOT `static`
  lines. `tools/nnks_list_statics.py` accepts column-0 declarations only
  through the explicit `SHARED_DATA` whitelist. Without that gate the
  prelude's ordinary column-0 statements and the `.inc` fragments' locals
  produce false rows (`fa`, `wv`, …) that do not compile; without the
  whitelist entries for `bank13`/`bank14`/`spsr_bank` a restore silently
  zeroes the banked r13/r14/SPSR. Both were measured defects in Phase-5AQ.
  Multi-declarator statements are parsed declarator by declarator, so
  `uint32_t n, z, c, v;` yields all four rows.

Rows are grouped into payload regions by `classify()` (RAM/ITCM/DTCM/card/SMC/
movie/input/display, everything else `io`). `CONFIG` excludes run
configuration (step limit, probe/trace addresses, shot cadence): restoring a
state must never change the current process's flags.

## 4. NOT carried (host-owned)

Rebound or re-derived on load — a state must never contain a host address
(ASLR / new process / different allocation layout):

```
gxd (SDL/GL handle), gxd_shot, rom, smc_img, smc_seg[].data,
smc_seen_key, smc_seen_form, nnk_in_log, nnk_rec, storewatch_fp,
p5t_str[].fp, p5t_str[].dec   (Mobiclip reference frames — re-decoded),
p5t_buf                       (decode scratch)
```

Also not carried: run configuration (`NNK_STEP_LIMIT`, probe/trace gate
addresses, `NNK_GX_SHOT_EVERY`, …) and the default state path.

## 5. Environment hooks

| variable | effect |
|---|---|
| `NNK_SAVESTATE=<path>` | boot straight from a state instead of replaying the boot |
| `NNK_SAVESTATE_DIR=<dir>` | directory of the default state file (default: `dirname(argv[0])`) |
| `NNK_SAVESTATE_SAVE_AT=<step>` | write a state at that guest step and **keep running** |
| `NNK_SAVESTATE_SAVE=<path>` | path for `SAVE_AT` (default `<dir>/nnk_save1.nnks`) |
| `NNK_STATE_HASH_AT=<step>` | print the canonical state hash at that step |
| `NNK_SAVESTATE_STRICT=1` | require the EXACT layout (build fingerprint, region ids/versions/lengths); anything else is REFUSED with the region diff — the round-trip proof uses this |
| `NNK_SAVESTATE_LAYOUT=<path>` | write this build's region/field layout (the text `tools/nnks_inspect.py` classifies a file against) |
| `NNK_SAVESTATE_IGNORE_BUILD=1` | legacy switch from Phase-5AS; the tolerant restore is now the DEFAULT, the variable only adds a note |
| `NNKS_HASH_REGIONS=1` | per-region CRCs in the hash line (debug) |

`NNK_STATE_HASH_AT` prints
`state hash: step=<n> fnv=0x… crc=0x… bytes=<payload> build=0x…`, where
`fnv`/`crc` are taken over the **same payload bytes a save would write**, in
table order. Two runs that reach the same step with the same machine state
print the same hash; that is the round-trip test.

## 5b. The dynamic state carrier is EXPERIMENTAL (opt-in: `NNK_STATE_CARRIER=1`)

The compiled dispatch is static: the ARM/Thumb state a guest PC runs in comes
from the image's mode table, not from the branch that reached it. Phase-5AS
added an opt-in *state carrier* (`tools/nnk_smc.inc`,
`nnk_smc_carry_override()`, enabled by `NNK_STATE_CARRIER=1`) that lets the
dynamic branch form overrule a compiled case, and logged the disagreements
(`NNK_STATE_CONFLICTS`).

**It is not the default and remains experimental.** Measured with
`NNK_STATE_CARRIER=1`: the cold-boot reproducer diverges at step **18,395,574**
near `0x027FF800` (stack region), i.e. carrying the state changes guest-visible
memory beyond the point the compiled image was validated against. That defect
is untouched by Phase-5AT.

Phase-5AT removed the *reason* the carrier was needed for the 0x02062C48 crash:
that was a wrong **mode table** row (`0x02062C44` is Thumb — ov17's Thumb
`0x020FEB94` calls it with `bl` at `0x020FEBB2` — but Ghidra's seed carried
`insn_len = -1`, so it defaulted to ARM), not a lost state. The committed
`tools/nnk_mode_overrides.txt` corrects the row and the shipped image now
carries the Thumb body (see PHASE5AT.md), so **both reproducers run to their
step budget with the carrier off**. Do not flip `NNK_STATE_CARRIER` on until
the 18,395,574 divergence is root-caused.

## 6. F5 / F9 in the window

With `NNK_GX=window` the SDL event filter in `tools/nnk_input.inc` consumes
`SDL_SCANCODE_F5` / `SDL_SCANCODE_F9` before the keymap sees them and calls
`nnk_state_ui_key(5|9)`:

* **F5** writes `<NNK_SAVESTATE_DIR or dirname(argv[0])>/nnk_save1.nnks`
  (`NNK_SAVESTATE_SAVE` overrides the path) and prints `state: saved …` with
  the step and frame.
* **F9** loads that file. On success the run rewinds to the saved instruction
  (`state: F9 restored …`); a refused state prints `state: F9 refused … — the
  run is unchanged` and the run continues untouched.

Saving does not disturb the run-by-design: the file is written to `<path>.tmp`
and renamed, so an interrupted save never leaves a half state.

## 7. Identity, the tolerant restore, and STRICT

**Refused (always)** — a real incompatibility, and nothing is applied:

1. magic `NNKS` (a non-NNKS file), container version outside 1..2;
2. header CRC32, payload CRC32 — a corrupt file;
3. every region's offset/length must lie inside the payload and its CRC32 must
   match — a corrupt region;
4. `rom_size`, `rom_hdr_crc` (first 0x200 bytes), `arm9_crc` (the ROM's ARM9
   blob as named by the header at 0x20/0x2C).

**Restored best-effort (default)** — everything that is not in the list above.
A different `build_fp`, region count, region length, region version or field
table is a *layout change*, not an incompatibility:

* regions are matched to the running build by **id** (v2) or **name** (v1);
  an unknown stored region is dropped, a missing one is defaulted, and both
  are reported;
* a field-table region whose length/version does not match is restored through
  its **field directory by name**; the fields that do not exist in this build
  are reported *by name* in a `state: PARTIAL …` banner;
* the restored step counter and frame counter are forced to the SAVED values
  (`record_step`, `record_frames`) even when those fields were defaulted, so
  the resumed run never looks like a cold boot at step 0 / frame 0;
* the run **resumes at the saved pc** (v1: rescued from the io tail, §1b).

**`NNK_SAVESTATE_STRICT=1`** keeps the exact-layout contract: the file must
have the same build fingerprint, the same region count and the same
id/version/length for every region, otherwise

```
state: ##############################################################
state: ###  REFUSED (NNK_SAVESTATE_STRICT=1) — layout differs
state: ###  <path>
state: ###  build fingerprint 0xBB96A499 != 0xED4C2A7D; region 'io' stored id=1 v0x00000000 68660 B, build id=1 v0x3E513DBC 85192 B (-16532 B)
state: ##############################################################
```

Measured, `NNK_SAVESTATE_STRICT=1` on the operator's state
`work/repro_5bc/healthy_prompt.nnks` (build `0xBB96A499`): exactly the banner
above, followed by

```
state: ###  BOOT FROM SAVESTATE FAILED — the run REPLAYS THE COLD BOOT
state: ###  every measurement from here is a fresh boot, not the saved machine
```

and the run then ran its 2,000,000-step budget from step 0
(`done steps=2000001 … halt=step budget exhausted`) — loud, not silent.

Without STRICT the same file loads (`state: loaded … PARTIAL; restored
step=3454999999 frame=6174, saved step=3455000000 frame=6174`) with
`8 of 11 region(s) exact` and, in `io`, `129 field(s) restored, 17 defaulted,
1 partial` — the defaulted names are printed.

The loader verifies every stored region's bounds and CRC **before the first
region is touched**, so a refusal cannot leave a half-applied machine. The one
state-sized region is `moviestreams`: a state recorded inside a movie names
streams the fresh process has not opened yet, so the loader rebuilds them
from the saved codecs (`p5t_stream()` re-opens the same container from the
same card image) and then applies the cursors.

Measured refusal cases (header byte flipped in a copy):

```
NNKS -> XNKS              state: REFUSED … bad magic (not an NNKS savestate)
version 3                 state: REFUSED … container version 3, this build reads 1..2
rom_hdr_crc byte flipped  state: REFUSED … header CRC mismatch (corrupt or from another format)
```

Each case announces the cold-boot replay and then runs its step budget, i.e.
it keeps running instead of crashing or restoring silently.

## 8. Determinism: what a restore guarantees, and what it does not

The emulator has **no wall-clock reads** (no `SDL_GetTicks`, no `time()`, no
sleep): guest time is `cycles`/`steps`, and presentation is driven by the
emulated VBlank. A headless run is therefore reproducible and a restore is
exact. Measured on the merged build:

| run | save | compare | result |
|---|---|---|---|
| pre-movie | step 30,000,000 → 32,000,000 | `state hash` | identical (fnv `0x16418EE6`, crc `0xDA68B041`) |
| pre-movie | same | frames 60 | byte-identical (`cmp`, 2/2 screens) |
| mid-movie | step 400,000,000 → 420,000,000 | `state hash` | identical (fnv `0xD79EC5F7`, crc `0x9E22B18E`) |
| mid-movie | same | frames 725/750 | byte-identical |
| in the intro film | step 2,300,000,000 → 2,450,000,000 | frames 4125…4375 (11 frames × 2 screens) | **22/22 byte-identical**, same halt (`pc=0x02016B50`, `distinct_pcs=22884`) |

The last one is the interesting one: the state was recorded while the Mobiclip
movie was playing, so it also proves the per-stream cursors and the
`gxblob` VRAM/compositor snapshot. On load the stream table is rebuilt from
the saved codecs and the decoder is re-opened and replayed sequentially from
frame 0 (`nnks_movie_resync`). Rewinding an existing decoder with
`mobiclip_flush()` instead left the lower half of the picture corrupt
(measured in a window F9: rows 102..191 differed), which is why the resync
re-opens it now.

Window F5/F9 (xvfb + xdotool, `tools/nnks_win_f5f9.sh`): with `NNK_GX=window`
the run was booted from the in-film state, F5 saved at frame 4124, the guest
presented on to frame 4150 (the picture moved on), F9 restored, and the frame
4125 capture written by the restored run was byte-identical to the one the
continued run had written (`cmp`); the frame-4150 capture differs. Window
captures of the same three moments are the PNG evidence.

The one thing that is *not* part of the machine is **real keyboard input in
the window**. SDL key events arrive at host wall-clock times while the
emulator runs as fast as the host can (there is no real-time pacing), so the
same physical press spans a different number of emulated frames from run to
run. That is the source of "same binary, same presses, different result" in
interactive sessions — not a clock inside the emulator: the headless and
window paths both produced byte-identical frames for the same machine state.
For reproducible window sessions drive the game with `NNK_INPUT_FILE`
(frame-indexed script) and use F5/F9 only as discrete, frame-synchronous
events. A state restore is also independent of that: the effective key mask
is part of the `input` region.

## 9. Tools and checks

* `tools/nnks_inspect.py <state> [layout]` — header identity, the per-region
  `{id, version, length, crc, nfields}` table, the stored field directory, and
  a **verdict against this build's layout**: `EXACT` (byte-for-byte restore,
  what STRICT requires), `PARTIAL` (loads best-effort; the fields this build
  would default are named) or `INCOMPATIBLE` (magic/version/CRC — the file is
  refused and the run is not from a saved state).  The layout comes from
  `argv[2]`, `NNK_SAVESTATE_LAYOUT`, `<state>.layout` (written next to every
  saved state) or `<state-dir>/nnk_layout.txt`; without one it says so and
  prints the table only.  One command answers "will my F5 state load?".
* `tools/nnks_selftest.c` — host harness that compiles the SHIPPED container
  `tools/nnks_container.inc` (`cc -O2 -std=c11 -I tools tools/nnks_selftest.c`)
  and checks exact restore, restore across an INSERTED field, the v1 prefix
  policy's measured misalignment, every identity refusal, and that a partial
  restore is neither the saved machine nor a cold boot.  The class test
  (`cargo test phase_5bh`) runs it.
* `tools/nnks_scan_shots.py '<glob>'` — print PPM captures that are not a
  single colour (non-blank evidence).
* Round trip (STRICT — the exact-layout contract):
  ```
  NNK_SAVESTATE_SAVE_AT=30000000 NNK_SAVESTATE_SAVE=/tmp/s.nnks \
  NNK_STATE_HASH_AT=32000000 NNK_STEP_LIMIT=32000000 ./nnk_runner ROM
  NNK_SAVESTATE=/tmp/s.nnks NNK_SAVESTATE_STRICT=1 NNK_STATE_HASH_AT=32000000 \
  NNK_STEP_LIMIT=32000000 ./nnk_runner ROM
  ```
  the two `state hash:` lines must be identical, and any `NNK_GX_SHOT` frames
  written after the save point must be byte-identical (`cmp`).
