# Phase-5BD — the movie HLE binds by GUEST IDENTITY, not by stream ordinal

Follows Phase-5BC (master `bd64b2d`).  Branch `wt/t_e40b23ae`.

Build: `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w" NNK_GX=1 BUILD_JOBS=20
NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh out/p5bd2/nnk_runner`
→ `BUILD OK`, `== prelude audit: 58 required, 2 divergent, 3 obsolete ==` (58/58 present,
both new 5BD rows required).

**Result: the in-game cutscene now decodes and presents `data/movie/s01.mods` — the
container the guest itself opened (581 frames) — instead of `op_u.mods` (1317).  Every
presented frame is bit-exact against an independent decode of that container
(72/72 Y-plane CRCs), and the displayed picture matches s01's reference at
corr 0.998+ while the old binding's film scores 0.36.  The intro still binds
`op_u`/`op_d` and still reproduces f4200/f4300/f4400 → f27/f40/f54 at 0.999.**

## 1. The defect

`tools/nnk_smc.inc: p5t_stream()` chose the container from the **registration
order**:

```
const char *path = (env && env[0]) ? env : (p5t_nstr == 1) ? P5T_ROM_SRC2 : P5T_ROM_SRC1;
```

`P5T_ROM_SRC1/2` are `data/movie/op_u.mods` / `op_d.mods`.  Phases 5BC measured
that the in-game cutscene's player object (`0x022C9E50`) carries **581 frames,
256x192**, i.e. `s01.mods`; registration order fed it `op_u.mods` (1317 frames).
That is the operator's «играет НЕ ТО видео / интро искажённое» symptom: durations,
scene content and the residual-ghost background all came from the wrong film.

## 2. The identity that replaces the ordinal

The MobiClip player object holds a **verbatim copy of the container's 48-byte
MODS header** at `player+4` = `codec-0xE4` (measured `0x022C9E54` in the 5BC
cutscene state, `0x02278268` / `0x022E8A90` for the two intro streams):

```
+0  "MODS"      +4  "N3\n\0" (0x000A334E)   +8  frames    +12 width  +16 height
+20 payload words 0x36a8 / 0x12301c / 1     (s01.mods byte for byte)
```

The 52-byte header's last word is the first frame-record length, which the player
object overwrites with its codec pointer — hence `NNK_MODS_IDENT 48`.  That prefix
is unique among the card's `data/movie/*.mods` (op_u/op_d share frames/size but
differ at +0x20/+0x28/+0x2C), so it identifies the file **without** a path string
and without trusting any guest pointer.

`tools/nnk_mobiclip.inc` gained the offline half of the rule:

* `nnk_fnt_dir_at()` / `nnk_fnt_dir_find()` — walk the Nitro FNT the same way the
  filesystem does (`nnk_fnt_lookup`, case-insensitive, dir ids masked `0x0FFF`);
* `nnk_rom_movie_match(rom, rom_len, hdr, path, cap, &nf)` — enumerate every
  `.mods` the card really contains under `data/movie/`, compare its own 48-byte
  header against the guest's copy, and return the **number** of matches.  It never
  picks a substitute: 0 = miss, >1 = ambiguous;
* `nnk_mobi_crc32()` for the diagnostic below.

`tools/nnk_smc.inc` gained the live half:

* `p5t_guest_hdr(codec, out, &where)` — read the guest's copy out of main RAM
  (window `codec-0x140 .. codec`, no cached pointer), validated by `"MODS"`,
  the version word, sane frames/width/height;
* `p5t_bind()` — `NNK_MOVIE_MODS*` override (announced) → guest identity
  (unique match) → **reported** ordinal fallback (no header copy / ambiguous
  header).  A header that matches nothing prints
  `hle: movie path '<guest container frames=… hdr-crc=…>' not found under data/movie/ — NOT substituting another movie`
  and the stream stays unbound (no frames presented);
* `p5t_open()` — one place that parses the container from the card image, so the
  frame count is always the bound container's (`s->frames = s->mods.nframes`);
* `p5t_stream()` no longer names any container and only registers the codec —
  the binding happens at the **first decode step**, when the guest has filled
  its player object (`p5t_present()` binds before `nnk_mobi_frame()`);
* a same-object session change (the guest's header changes) closes the source and
  rebinds, so no ring slot of the previous film can be presented;
* `NNK_MOVIE_TRACE=1` prints `requested → bound, frames, CRC`:
  `movie: trace codec=0x022C9F38 guest-hdr@0x022C9E54 nf=581 256x192 payload=0x0012301C crc=1146C579 -> data/movie/s01.mods`
  and per present `movie: trace present codec=… path=… frame=N/M ycrc=… step=…`.

Manifest rows (`required`, both present in the 58/58 audit):
`5bd_movie_identity → nnk_rom_movie_match` (nnk_mobiclip.inc),
`5bd_movie_bind → p5t_guest_hdr` (nnk_smc.inc).

## 3. Measured: the binding

Cutscene state `work/repro_5bb/st3400.nnks` (frame 6076), ROM-only, no side files:

```
movie: stream 0 codec=0x0227834C registered (source by guest identity) step=3399999999
movie: stream 1 codec=0x022E8B74 registered (source by guest identity) step=3399999999
movie: stream 2 codec=0x022C9F38 registered (source by guest identity) step=3399999999
movie: trace codec=0x022C9F38 guest-hdr@0x022C9E54 nf=581 256x192 payload=0x0012301C crc=1146C579 -> data/movie/s01.mods
movie: stream 2 codec=0x022C9F38 source=rom:data/movie/s01.mods frames=581 256x192 via guest header identity step=3403753722
movie: trace present codec=0x022C9F38 path=data/movie/s01.mods frame=0/581 ycrc=5B1D85F1 step=3403753722
movie: trace present codec=0x022C9F38 path=data/movie/s01.mods frame=71/581 ycrc=18751D16 step=3599902431
```

Cold boot (`op_u`/`op_d` intro), unchanged behaviour, now also by identity:

```
movie: trace codec=0x0227834C guest-hdr@0x02278268 nf=1317 256x192 payload=0x001FFCD4 crc=3A37A002 -> data/movie/op_u.mods
movie: stream 0 codec=0x0227834C source=rom:data/movie/op_u.mods frames=1317 256x192 via guest header identity step=2256256960
movie: trace codec=0x022E8B74 guest-hdr@0x022E8A90 nf=1317 256x192 payload=0x002012B0 crc=F8EFF4BA -> data/movie/op_d.mods
movie: stream 1 codec=0x022E8B74 source=rom:data/movie/op_d.mods frames=1317 256x192 via guest header identity step=2256354765
```

## 4. Measured: the presented frames are s01's, bit for bit

Independent reference: `data/movie/s01.mods` extracted from the card (Nitro
FNT+FAT, no guest involvement) and decoded with the committed
`tools/mobiclip_selftest.c` → `s01.yuv` (581 frames).  Comparing every logged
`ycrc` against the reference frame's Y plane (CRC-32/ISO-HDLC):

```
data/movie/s01.mods: 72 presents, 72/72 frame CRCs equal to the reference decode
data/movie/op_u.mods: 60 presents, 60/60 frame CRCs equal to the reference decode
```

and the wrong film is nowhere in the stream: no `op_u` present line exists for
`codec=0x022C9F38`.

## 5. Measured: the screen shows s01 (and the intro still shows op_u)

Display captures (`NNK_GX=headless NNK_GX_SHOT=… NNK_GX_SHOT_EVERY=3`), identified
by Pearson correlation of box-filtered 64×48 luma (kills the Y2R dither) against
the reference dumps:

| capture (display frame) | best s01 frame | corr | best op_u frame | corr |
|---|---|---|---|---|
| cutscene bot f6210 | f0025 | **0.9985** | — | — |
| cutscene bot f6330 | f0049 | **0.9982** | f1198 | 0.364 |
| cutscene bot f6390 | f0061 | **0.9984** | — | — |
| cutscene bot f6420 | f0067 | **0.9985** | — | — |
| cutscene bot f6168 | f0016/f0024 | 0.965 (scene cross-fade) | — | — |

The matched reference index advances with the display frame (≈1 movie frame per
5 displayed frames = 12 fps), which is the film running, not a frozen frame.

Intro (cold boot, `NNK_GX_SHOT_EVERY=10`, top screen = `op_u`):

| capture | best op_u frame | corr |
|---|---|---|
| f4200.top | **f0027** | **0.9990** |
| f4300.top | **f0040** | **0.9992** |
| f4400.top | **f0054** | **0.9994** |

i.e. exactly the 5AY mapping f27/f40/f54, ≥ 0.99.

## 6. Length and end of film (honest)

The stream presents exactly the bound container's frames: `s->index` runs
`0 .. nframes-1` and only clamps at the last frame (`past EOF: hold the last
frame`) — nothing is padded from another file, and the guest's own frame counter
drives its end-of-movie path.

A long run from `st3400` (limit 5.4e9) reached step **4,467,284,179** with
**382/581** s01 presents and then halted on an **unimplemented SWI** unrelated to
the binding:

```
hle swi imm24=0x2C0001 num=0x2C lo=0x01 st=arm at=0x02143186 r0=0 r1=0x4000 r2=0xFFFFFF01
halt state: … r14=020D834F
done steps=4467284179 … halt_pc=0x02143186 halt=step budget exhausted
```

So the *end of the film* is not reached in this phase; the stop is a service-call
HLE gap (SWI `0x2C`), not a movie-boundary or padding effect.  This is the one
acceptance item that remains unproven and it needs its own card.

## 7. Verification

| check | result |
|---|---|
| class test (`cd nnktool && cargo test --release`) | **61 passed, 0 failed** (suite was 60; new `phase_5bd_movie_binds_by_guest_identity_not_by_ordinal`) |
| `SKIP_MELONDS=1 sh tools/verify_phase2a.sh` | strict prefix **1000/1000**, window-filtered **1000/1000**, in-order **1000/1000** |
| 5BB `NNK_EXC_TRACE` signature (`st2350` + `skip_prompt2.nnk`) | `exc_return step=3064069170 target=0x02137A92 spsr=0x4000003F thumb=1 mode=31` — intact |
| savestate round trip 5M→6M | `fnv=0x4C39447B crc=0x8EC70217` both on save-run and on reload |
| savestate round trip 30M→32M | `fnv=0xD2AE4A32 crc=0xE89D4628` both on save-run and on reload |
| ROM-only | card image is the only input; no `NNK_MOVIE_YUV*`, no side files, no decoded bytes on disk |
| speed | 600M steps **61.18 s** (reference 57 s, +7.3 % — inside the 10 % bound) |

The class test pins the class, not the instance: the Rust mirror of the rule has
no index/ordinal argument (unique → that container, ambiguous → reported, miss →
refused), and the source guards fail if `p5t_stream()` ever names a container
again, if the two ordinal fallbacks stop being reported, if the 48-byte identity
is replaced by the 52-byte header, if a miss stops being loud, or if binding moves
after the first decode.

## 8. Notes / not done here

* `work/repro_5bd`-style artifacts are not committed; the captured frames and the
  reference `s01.yuv` live under `work/p5bd/` and the scratch dir (movie bytes are
  never committed).
* The `p5t_buf`-level evidence (ycrc) is exact; the screen-level numbers above are
  correlation, and the two early cutscene captures (6078 before the film reaches
  the screen, 6168 in a cross-fade) are not scored at 0.99.
* The in-game prompt's missing text / "No" button is the 5BA AOBJ defect (not this
  card, unchanged).
* The SWI `0x2C` halt at 4.467e9 (above) is the next blocker inside the film.
