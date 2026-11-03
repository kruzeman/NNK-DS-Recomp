# Phase-5BH — savestates survive a region-size change (NNKS v2, tolerant load)

Task: `t_34657209`.  One defect: **a savestate written by ANY earlier build was
silently REFUSED by a newer build** because a region's byte size changed, and
the runtime then quietly replayed the boot.  Savestates are this project's main
testing tool (F5/F9, `NNK_SAVESTATE`), so a build that invalidates every
existing state costs a full cold boot (~6-7 minutes per experiment) and makes
every "state + script" repro look like *"the state loaded but nothing
happened"*.

## 1. The measured defect (master `e395fb4`)

```
$ NNK_SAVESTATE=work/repro_5bc/healthy_prompt.nnks NNK_SAVESTATE_IGNORE_BUILD=1 \
  NNK_STEP_LIMIT=20000000 NNK_GX=headless build/nnk_runner 'Ni No Kuni.nds'
state: WARNING: NNK_SAVESTATE_IGNORE_BUILD=1 accepts ... written by build 0xBB96A499 on build 0xED4C2A7D
state: REFUSED ...: region 'io' is 68660 bytes, this build writes 85192
state: boot from ... failed — replaying the boot
```

Root shape: the container was region-table driven, but a region's length was
**implicit in the writer** — `io` is the concatenation of every generated field
row, so *any* phase that adds one runtime static grows it (68660 → 85192 B here,
+16532 B over five phases).  The v1 loader could only compare lengths and refuse
the whole file, and the refusal was a single `stderr` line that scrolled past
while the run became a cold boot.

## 2. What changed

**Format v2 — self-describing and version tolerant** (`tools/nnks_container.inc`,
a new fragment with no machine dependencies, `#include`d by
`tools/nnk_state.inc`):

* the 72-byte header is byte-compatible with v1; the old padding word is now
  `flags` with `NNKS_FLAG_FIELDDIR`;
* a region record carries `{id, version, offset, length, crc, nfields}` and the
  name (48 B instead of 40 B) — a stored region is matched to the running build
  **by id**, never by table position;
* after the region table, each field-table region carries its own
  **field directory** of `{name[24], off, len}` (32 B an entry, 230 entries in
  this build), so a stored field is matched **by name**.  An inserted static
  therefore cannot shift its neighbours (the v1 prefix policy does — measured
  in `nnks_selftest.c`);
* the region **version** (FNV-1a over the field names/lengths, the renderer's
  `gxd_state_layout_fp()` for `gxblob`, `sizeof(NnkStateMovie)` for the stream
  table) plus the length is what authorises a plain `memcpy`;
* **v1 files are still read**: regions by name, fields by prefix up to
  `min(len)`, and the saved pc is rescued from the trailing 4 bytes of `io`
  (`nnks_pc` is the last io field in every build since Phase-5AQ; the value is
  accepted only inside the guest's executable windows).

**Load policy**

| mode | behaviour |
|---|---|
| default | TOLERANT: refuse only magic, container version, header/payload/region CRC and ROM identity; everything else is a *layout change* restored field by field with a per-region/per-field report |
| `NNK_SAVESTATE_STRICT=1` | EXACT layout required (build fingerprint, region count, per-region id/version/length) — the determinism proof runs with this |

A tolerant load forces the restored step counter and frame counter to the SAVED
values (`record_step`/`record_frames`), so a best-effort restore can never look
like a cold boot at step 0 / frame 0.

**Loud, not silent** (`tools/nnk_state.inc`): a refused boot state prints a
`###` banner that says the run REPLAYS THE COLD BOOT; a partial restore prints a
`***` banner listing every region (stored → build size/version), the per-region
field counts and the *names* of the defaulted fields.

**CLI**: `tools/nnks_inspect.py <state> [layout]` now prints the
`{id, version, length, crc, nfields}` table and classifies the file against the
running build's layout (`EXACT` / `PARTIAL` / `INCOMPATIBLE`).  The layout text
is written next to every saved state (`<state>.layout`) and on demand with
`NNK_SAVESTATE_LAYOUT=<path>`.

**Build wiring**: `tools/split_dispatch.py` copies `nnks_container.inc` next to
the generated TU, and `tools/build_split.sh` hashes it into the object-cache
key — without that line an edit to the container left a stale `nnk_main.o`
(measured while building this phase).

## 3. Measured proof

### 3a. The operator's state (v1, build `0xBB96A499`) now loads

```
$ NNK_SAVESTATE=work/repro_5bc/healthy_prompt.nnks NNK_GX=headless <build> ROM
state: note: ... was written by build 0xBB96A499, this build is 0xED4C2A7D — regions are
       matched BY ID and fields BY NAME (tolerant restore)
state: ***  PARTIAL SAVESTATE: 8 of 11 region(s) exact, 3 restored best-effort
state:   io           PARTIAL   stored 68660 B (v0x00000000) -> build 85192 B (v0x3E513DBC)
state:                129 field(s) restored, 17 defaulted, 1 partial, 0 gone, defaulted:
                      nnk_aobj_bytes nnk_aobj_pc ... nnks_pc
state:   smc/card/input/movie/display/ram/itcm/dtcm   EXACT
state:   gxblob       PARTIAL   stored 5270752 B -> build 5270752 B (accepted from a different layout)
state: loaded ... (v1, identity OK, PARTIAL; restored step=3454999999 frame=6174,
       saved step=3455000000 frame=6174)
state: note: v1 file (no field directory): the saved pc 0x02007138 was rescued from the
       trailing 4 bytes of io
state: resume pc=0x02007138 ...
```

The run continues at the saved step/frame/pc instead of replaying the boot, and
the frame counter never reads 0.

### 3b. A layout change in a v2 state: an INSERTED io field

The probe build adds one static in the middle of the `io` table
(`nnk_p5bh_probe`, temporary, reverted).  Loading the state the clean build
wrote:

```
state: note: ... written by build 0xED4C2A7D, this build is 0x532EAC94 — tolerant restore
state: ***  PARTIAL SAVESTATE: 10 of 11 region(s) exact, 1 restored best-effort
state:   io   PARTIAL  stored 85192 B (v0x3E513DBC) -> build 85196 B (v0x70FA2563)
state:        147 field(s) restored, 1 defaulted, 0 partial, 0 gone, defaulted: nnk_p5bh_probe
state: loaded ... restored step=4999999 frame=13, saved step=5000000 frame=13
state: resume pc=0x02016B50 ...
```

i.e. exactly one field defaulted (named), the pc and the frame restored, and the
guest trajectory identical to the clean build's continued run at the same step
(`distinct_pcs=4245`, `halt_pc=0x02016B40` in both).

### 3c. STRICT still refuses, loudly, with the exact region diff

```
$ NNK_SAVESTATE=...healthy_prompt.nnks NNK_SAVESTATE_STRICT=1 ...
state: ###  REFUSED (NNK_SAVESTATE_STRICT=1) — layout differs
state: ###  build fingerprint 0xBB96A499 != 0xED4C2A7D; region 'io' stored id=1
            v0x00000000 68660 B, build id=1 v0x3E513DBC 85192 B (-16532 B)
state: ###  BOOT FROM SAVESTATE FAILED — the run REPLAYS THE COLD BOOT
state: ###  every measurement from here is a fresh boot, not the saved machine
```

### 3d. The determinism proof is intact (STRICT round trips)

| run | save | compare | result |
|---|---|---|---|
| pre-movie | step 5,000,000 → 6,000,000 | `state hash` | identical: fnv `0x2AE1D675` crc `0x25A11CF1` (30,711,632 B) |
| pre-movie | step 30,000,000 → 32,000,000 | `state hash` | identical: fnv `0xE8714014` crc `0x9DA0B2D5` |

Both restored runs printed `state: loaded ... v2 ... restored step=… frame=…`
with `resume pc=0x02016B50` / `0x0210AC1C` (never ENTRY_CONST), under
`NNK_SAVESTATE_STRICT=1`.

## 4. Tests

* `cd nnktool && cargo test --release` → **63 passed, 0 failed** (suite was 62;
  new `phase_5bh_savestate_tolerates_an_older_region_layout`, which compiles and
  runs `tools/nnks_selftest.c` against the shipped container, drives
  `tools/nnks_inspect.py` on synthetic v1/v2 files — exact / partial / bad
  magic / corrupt header — and pins the runtime policy and the build wiring).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh` → **1000 / 1000** on all three
  melonDS prefix checks (`/home/hermes/.hermes/profiles/coder/cache/scratch/p5bh/verify2a.log`).

## 4b. Regressions re-measured on this branch

| check | result |
|---|---|
| cold boot, intro band (top screen vs `work/p5t/op_u.yuv`) | f4200 → ref f0027 **0.9989**; f4300 → f0040 **0.9990**; f4400 → f0054 **0.9989** (reference 5BD mapping, all ≥ 0.99) |
| first non-blank top screen | frame 3710 (the intro film starts) — identical to the pre-change evidence `work/repro_5ax/ours_f1000_top.png`, whose frame-1000 top screen is also black (viewed, uniform black) |
| `NNK_EXC_TRACE=1` | still logs every exception return in the same form, e.g. `exc_return step=2358333455 target=0x0207CEA8 spsr=0x8000001F thumb=0 mode=31`; 5,181 returns in an 850 M-step run from `st2350`.  The *exact* 5BB pair (`step=3064069170 target=0x02137A92`) belongs to an EXACT load of that state in the 5BB build; five phases later the same v1 file is a best-effort restore (18 `io` fields defaulted), so the run reaches a different frontier — reported by the PARTIAL banner, not hidden. |
| speed, 600 M steps headless | **57.6 s** (`/usr/bin/time`), reference ≈ 61 s — no regression |
| 5 M → 6 M STRICT round trip (final binary) | fnv `0x2AE1D675` crc `0x25A11CF1` both runs |
| 30 M → 32 M STRICT round trip | fnv `0xE8714014` crc `0x9DA0B2D5` both runs |


## 5. Honest limits

* A **v1** file cannot be matched field by field (it has no directory): fields
  past its stored length are defaulted and reported, and the pc is recovered
  from the io tail as described above.  Measured consequence: a v1 state that
  was recorded *inside the intro movie* resumes correctly (frames advance,
  first captured frame `f04205.top` → reference `f0028`, corr **0.9990**) but
  the film's stream is re-opened and re-replayed by the movie HLE, so the
  picture mapping restarts from the reference head after the next scene cut;
  i.e. character-level state of the *decoder* is re-derived, not restored — the
  same behaviour the exact path already had (`nnks_movie_resync`).
* A file whose `io` layout differs by *more* than the prefix is only as good as
  the names it still shares; the banner always states how many fields were
  defaulted, so a measurement can never be mistaken for an exact restore.
