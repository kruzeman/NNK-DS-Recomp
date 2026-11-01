# Phase-5AK — `[stream+0x124]` = 0x80808080: an uninitialised field, not a bad decode

Follows Phase-5AI (`4cd62bf`, merged here as `63d7c3d`).  5AI named the barrier
"who initialises `[stream+0x124]`" — the field that parks the guest at
`0x02009108` after the A|Start movie skip.  **Answered, with the writer named
and the value's origin measured.**  The field is never initialised by any
constructor; the value is the *movie audio driver's own 8-bit-silence fill*
(`memset(right[i], 0x80, 0x6000)`) left behind in a heap block that the
post-movie stream objects are allocated in.  This document also rules out the
three classes the card listed and gives the exact remaining gap.

Status: **no behaviour change** in this phase (nothing was faked to make the
guest continue).  `cargo test` 48/48, `verify_phase2a` green, movie band and
notice screen untouched, screenshots attached.

## 1. The measurement (the instrument 5AI shipped, plus NNK_DEBUG_AT_PC)

Tooling: **`NNK_STOREWATCH="0x022CC714:0x022CCA84"`** (the existing store watch,
`tools/nnk_smc.inc` §"generic guest-store watch") and **`NNK_DEBUG_AT_PC`** for
the call arguments.  Session: the canonical build (§6) with
`NNK_INPUT_FILE=tools/nnk_operator_session.nnk`, `NNK_IPC_PROBE=1`,
`NNK_STEP_LIMIT=334200000` — it reproduces the 5AI halt exactly
(`halt_pc=0x02009114 distinct_pcs=23201 hle_hits=7760`).

845 writes landed in the two-object window.  Every write that touches
`0x022CC8CC+0x124` (= `0x022CC9F0`) and the sibling's `+0x124` (= `0x022CC838`),
in order:

| step | pc | lr | address | bits | old value | who |
|---|---:|---|---|---:|---|---|
| 323133090 | 0x02009BB4 | 0x0207AB40 | 0x022CC838 | 32 | 0 | memset-0 of the **sibling** object |
| 323133172 | 0x02009CF0 | 0x0207AB5C | 0x022CC8CC | 32 | 0 | 0x80 fill reaches the **aborting** object's first word |
| 323133391 | 0x02009CF0 | 0x0207AB5C | 0x022CC9F0 | 32 | 0 | 0x80 fill covers `+0x124` → **0x80808080** |
| 325896101 | 0x01FFDAD4 | 0x01FF8698 | 0x022CC9F0 | 32 | 0x80808080 | audio decode copy (`0x022BAAD0` → `0x022CC9F0`) — writes 0x80808080 again |
| 325959385 | 0x01FFCF3C | 0x01FF863C | 0x022CC838 | 32 | 0 | the same copy on the sibling — writes **0** |

and the object's own construction, 590 steps before the abort (all from the
store watch; `+0x124` appears in **none** of them):

| step | pc | write |
|---|---:|---|
| 334057213 | 0x02043700 | `[this] = 0x02069264` (base ctor `0x020436F8`: `str r2,[r0]` / `str r1,[r0,#12]`) |
| 334057222 | 0x020437FC | `[this] = 0x020692A8`; then three `memset(this+0x14/0x20/0x2C, 0, 12)` |
| 334057415 | 0x02043EC8 | `0x02043E98`: `+0x54,+0x98,+0x9C,+0xA0,-0xFC,+0x100,+0x120=0x7FFF`, `+0x14=0x80000`, `+0x18=0x60000` |
| 334057429 | 0x020DBD90 | overlay-17 thunk |
| 334057437 | 0x020D9ADC | final vptr `[this] = 0x0213B70C` |

**`+0x124` is written by exactly one function in the whole image**: `0x02044788`
(`str r0,[sl,#292]`), the stream *data* setup —

```
0x02044760  r2 = arg; if (r2 && [r2] != 0x54414154 /* "TAAT" */) r2 = 0
0x02044788  [this+0x124] = r2 ? r2+8 : 0        ; data pointer, or NULL
```

`NNK_DEBUG_AT_PC=0x02044788` fires **once** in the whole 334 M-step run —
step **42,038,614**, during boot — and never for the post-movie objects.  So
for them `+0x124` is *uninitialised memory*, and the ctor chain above (which
zeroes only `+0x14..+0x38` and a list of named fields) does not clear it.

## 2. Where the 0x80808080 comes from (measured, not inferred)

`NNK_DEBUG_AT_PC=0x02009C90` (the SDK `memset(dst, byte, len)` at
`0x02009C90`, word loop at `0x02009CF0`) at the five calls that fill the movie
audio buffers:

```
dbg78 step=323038690 r0=02284810 r1=00000080 r2=00006000 ... r14=0207AB5C
dbg79 step=323062235 r0=02296830 ...
dbg80 step=323085780 r0=022A8850 ...
dbg81 step=323109568 r0=022BA870 ...
dbg82 step=323133113 r0=022CC890 ...        <- the 5th (last) buffer
```

Caller = **overlay 13** (`ov013.bin`, base `0x02079F80`) at `0x0207AB58`:

```
0x0207AAD0  ldr  r0,[r4,#0x14] ; lsl r0,r0,#8 ; bl 0x0207BC0C   ; alloc left
0x0207AAF8  ldr  r0,[r4,#0x14] ; lsr r0,r0,#1 ; lsl r0,r0,#8
0x0207AB04  bl   0x0207BC0C                                      ; alloc right
0x0207AB50  mov  r1, r7 (=0x80)
0x0207AB4C  lsr  r2, r1, #1 ; lsl r2, r2, #8                     ; = 0x6000
0x0207AB58  bl   0x02009C90        ; memset([player+0x60][ch], 0x80, 0x6000)
0x0207AB5C  ...                     ; ch = r6 = 0..[player+0xA8]-1 = 0..4
```

with `[player+0x14] = 0xC0` (192 samples) → `(0xC0>>1)<<8 = 0x6000`.  The five
right-channel buffers are a pool at `0x02284810 + ch*0x12020`; the 5th is
**`0x022CC890..0x022D2890`**, and the aborting object at **`0x022CC8CC`** is
`buffer5 + 0x3C` — inside it.  The sibling at `0x022CC714` is one object
(`0x1B8`) earlier, *outside* the buffer, and was explicitly zeroed at
`0x02009BB4` (`0x022CC700..0x022CC87C`) — which is exactly why its `+0x124`
reads **0** and the aborting object's reads the fill pattern.

## 3. The three classes the card asked to name — all ruled out, with evidence

* **(a) "freshly-filled heap poison from our own allocator/memzero" — no.**
  `grep -r 80808080` over every `.c/.h/.inc/.py` in the tree and over the
  23.6 MB generated dispatch image: **0 hits**.  No runtime path writes the
  pattern; the value is produced by guest `memset(…, 0x80, …)` (§2).
* **(b) "a VRAM/OAM read-back through the Phase-5X shadow" — no.**  The value
  is in main RAM (`0x022CC9F0`), written by a guest store with the raw address
  logged; the shadow only serves `0x05000000/0x06000000/0x07000000`.
* **(c) "a wrong-stride read landing in an unrelated structure" — no.**  The
  stride is right: `0x022CC8CC+0x124` is the last field of a live object whose
  vptr (`0x0213B70C`), list links (`+4`, `+8`), player back-pointer (`+0xC`)
  and ctor-set fields are all correct.  Only the fields *no code initialises*
  (`+0x120`, `+0x124`, `+0x130`, …) hold the fill pattern — obj vs sibling
  differ on **29 words**, every one of them a field no writer touches.

The class is therefore: **an uninitialised struct field inheriting the previous
user's 8-bit-silence fill** ("stale heap content"), i.e. a *heap-layout*
question, not a decode, window or shadow bug.

## 4. Cross-check against melonDS — the read model is NOT divergent

The guard does `r0 = [this+0x124]; if (r0) { if (ldrh [r0] <= state) OS_Terminate }`
(`0x02044868`, disassembled in PHASE5AI §3).  `0x80808080` is outside every
mapped window, so the decisive value is what an *unmapped* read returns.
melonDS (`/tmp/melonds_pristine/src/NDS.cpp`, `NDS::ARM9Read16`) switches on
`addr & 0xFF000000`, has no case for `0x80……`, and falls through to a bare
`return 0;` — **melonDS reads 0 there too**, exactly like this runtime
(`win_off()` rejects the address → `ld16()` = 0).  So:

* our model does not diverge from melonDS in the read model, and changing
  `ld16()` to return open-bus `0xFFFF` would *diverge from the reference* (it
  would also make `bhi` pass — measured as the only value that saves the call);
* the divergence, if any, is **upstream in the heap layout**: which block the
  six objects of the `0x0207AA90` loop get.  It is unmeasured here because this
  project has no input-scripted melonDS run (5AI §3), and making one is a
  separate task.

## 4b. H-A vs H-B (the operator's two hypotheses) — discriminated

* **H-A ("the field should be 0; a runtime poison / skipped ctor / short memzero
  left it garbage") — DISPROVED as a runtime fault, CONFIRMED as the field's
  state.**  No writer of `+0x124` was skipped: the field has exactly one writer
  in the whole image (`0x02044788`), it runs once at boot for other objects,
  and it is *not part of this class's construction at all* — the ctor chain
  (`0x02043E98 → 0x020437D8 → 0x020436F8`) writes `+0x14/+0x18/+0x54/+0x98/
  +0x9C/+0xA0/+0xFC/+0x100/+0x120` plus vptr/list links and never touches
  `+0x124`.  The 0x80808080 is not our poison (0 hits for the pattern in every
  source file and in the 23.6 MB image) — it is the guest's own
  `memset(…,0x80,…)` (§2).  So H-A resolves to: the field is *deliberately
  never initialised*; it is the heap block's leftover.
* **H-B ("the guest relies on an unmapped read") — this is the MECHANISM, and
  it is the only thing that saves the call.**  Our `ld16()` (§"Guest accessors"
  in the prelude) consults the Phase-5X shadow, ITCM, DTCM, the GBA slot, IO and
  the flat window, then `return 0;` — and `0x80808080` is claimed by none of
  them (`gxd_sh_rd(0x80808080, 2)` is NULL: the shadow only serves
  0x05000000/0x06000000/0x07000000).  **melonDS is identical**: `NDS::ARM9Read16`
  switches on `addr & 0xFF000000`, has no `0x80` case, and returns 0.  Both
  emulators therefore halt; only a *hardware* open-bus value (`0x8080`/`0xFFFF`,
  i.e. anything `> new_state`) would make `bhi` pass.  A REGPROBE "differ" test
  cannot discriminate this: melonDS never reaches the step (no input-scripted
  melonDS run) and its read model is the same 0.  So the missing knowledge is a
  **hardware open-bus decision**, not an emulator divergence — and changing
  `ld16()` to answer open bus would intentionally diverge from melonDS.



## 5. Why the guest still parks (the remaining gap, stated exactly)

After the skip the game rebuilds its stream set (`0x0207AA90`, overlay 6/13;
measured: 6 ctor calls at 334,050,428…334,057,208) and the 6th object is handed
a heap block that is the tail of the freed movie-audio right-channel buffer.
Its `+0x124` is `0x80808080`; the guard dereferences it, gets 0 from the bus
model, and takes `bl 0x020090D8` → IRQ-off WFI at `0x02009108` (guest parked,
top/bottom screens FLAT 0x00 from display frame 585 — the frames in §6 are the
same 223-byte single-colour PNGs 5AI recorded).

**Remaining gap (one measurement, one decision):**
1. measure the *allocation sequence* the six `0x0207AA90` objects take in
   melonDS (or the 5 audio buffers' frees) at ~`0x0207Axxx`/step 323–334 M and
   see whether hardware/melonDS hands object 6 a zeroed block instead of the
   0x80-filled tail; that needs the input-scripted melonDS run;
2. if the layouts agree, then `[+0x124]` is genuinely 0x80808080 on hardware
   too and the *only* thing that saves the game is the DS open-bus value — which
   melonDS does not model either, so the honest next step is a **hardware
   open-bus decision** (documented, with the two emulators permanently
   differing), not a silent `ld16()` change.

## 6. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/stream-init
ln -sfn /home/hermes/NiNoKuni/work work
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
  SPLIT_DIR=work/split/c bash tools/build_split.sh out/split/p5ak_s1

S=~/.hermes/profiles/coder/cache/scratch/p5ak
RUN="NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds'
     NNK_MOVIE_YUV=/home/hermes/NiNoKuni/work/p5t/op_u.yuv
     NNK_MOVIE_YUV2=~/.hermes/profiles/coder/cache/scratch/p5t/op_d.yuv
     NNK_GX=headless NNK_INPUT_FILE=tools/nnk_operator_session.nnk
     NNK_SMC_IMAGE=0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin"

# (a) the write watch (evidence table §1)
$RUN NNK_STEP_LIMIT=334200000 NNK_STOREWATCH="0x022CC714:0x022CCA84" \
     NNK_STOREWATCH_PATH=$S/sw_obj.txt NNK_STOREWATCH_N=500000 ./out/split/p5ak_s1 \
     work/phase3a/arm9_stored_padded.bin

# (b) the fill's arguments (§2)
$RUN NNK_STEP_LIMIT=323200000 NNK_DEBUG_AT_PC=0x02009c90 NNK_DEBUG_N=1000000 \
     ./out/split/p5ak_s1 work/phase3a/arm9_stored_padded.bin

# (c) the only writer of +0x124 (§1)
$RUN NNK_STEP_LIMIT=334058100 NNK_DEBUG_AT_PC=0x02044788 NNK_DEBUG_N=4 \
     NNK_TRACE_FROM=334050000 ./out/split/p5ak_s1 work/phase3a/arm9_stored_padded.bin

# (d) screenshots (both screens, after the skip)
$RUN NNK_STEP_LIMIT=334200000 NNK_GX_SHOT=$S/shots/f NNK_GX_SHOT_EVERY=5 \
     ./out/split/p5ak_s1 work/phase3a/arm9_stored_padded.bin
python3 tools/p5ai_ppm2png.py $S/shots/f.00600.top.ppm shots/p5ak/p5ak_f600_top_black.png
```

## 7. No regressions (measured this branch)

* `cargo test` in `nnktool`: **48 passed, 0 failed** — 46 + the two new class-46
  tests (`phase_5ak_stream_state_guard_terminates_on_the_unmapped_data_pointer`,
  `phase_5ak_audio_silence_fill_reaches_the_stream_object`).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh`: **green** (log
  `$S/verify2a.log`); no source behaviour changed by this phase, which is why
  the acceptance image is the same one 5AI shipped.
* Scripted A|Start run to 334.2 M steps: identical guest outcome
  (`distinct_pcs=23201 hle_hits=7760 halt_pc=0x02009114`), i.e. the ARM7
  back-pressure fix and everything upstream of the skip is untouched.
* Screenshots (fresh, this branch): `shots/p5ak/p5ak_f560_top_movie.png`
  (real movie content, 4852-byte PNG) and
  `shots/p5ak/p5ak_f600_top_black.png` / `p5ak_f600_bot_black.png`
  (223 bytes = one colour, the parked state).
