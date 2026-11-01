# Phase-5AC — the movie band: the measured configuration matrix, the mechanism, and the canonical configuration

Follows Phase-5AB (`3a9fc79`).  The card asked which of `NNK_MOVIE_YUV` /
`NNK_MOVIE_YUV2` broke the presented movie band, whether the shadow fix changed
the guest's own movie path, and which configuration is canonical.

Short answer:

* **The `=1` form IS mis-wired, and it is now fixed** (class 40):
  `NNK_MOVIE_YUV=1` used to switch the HLE *on* with no source at all.  Measured
  in this phase: `NNK_MOVIE_YUV=1` and `NNK_MOVIE_YUV` unset produce **14/14
  byte-identical** display frames — the hook contributed nothing.  A bare
  truthy value is now explicitly "HLE OFF, the guest decoder runs".
* **The band regression is a second, independent defect**: on the shadow-fixed
  build the guest's movie decode **session starts 3,455 display frames early**
  (step 323.4M / frame 578 instead of step 2.256e9 / frame 4033), each stream
  presents exactly **7** frames (its whole 6-slot Y ring + 1 in flight) and the
  driver then stops calling the decode callback for the remaining 2.03e9 steps.
  The band is therefore never fed host frames (`=path` configs: uniformly black)
  and the guest's own decoder resumes from a broken reference (`=1`: macroblock
  garbage, corr 0.29 top / 0.07 bottom).
* Phase-5AA's `corr 0.997` is real and re-verified here, but **only in the
  unscripted session of that build** (`p5aa/run4`, 2.45e9 steps: top 0.9987,
  bottom 0.9985 at f4200).  It is not a property of the current binary.

## 1. The matrix, as measured

Every run: the same binary built through the fixed build path (prelude splice
applied), `NNK_GX=headless`, `NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin"`,
`NNK_INPUT_FILE=tools/nnk_operator_session.nnk`, frames every 100.

| # | config | frames captured | top @ f4200 | bottom @ f4200 |
|---|---|---|---|---|
| cfg0 | `NNK_MOVIE_YUV` unset | f100..f1400 (+**byte-identical to cfg1**) | band not reached in budget | — |
| cfg1 | `NNK_MOVIE_YUV=1` | f100..f4400 | mean 38.5 sd 7.9, best `op_u` f0022 **corr 0.2896** (MAE 35.2) | mean 42.1 sd 10.6, best `op_d` f0015 **corr 0.0671** |
| cfg2 | `=1` + `NNK_MOVIE_YUV2=op_d.yuv` | f100..f400 (**byte-identical to cfg1**) | identical to cfg1 (see §2) | identical to cfg1 |
| cfg3 | `NNK_MOVIE_YUV=op_u.yuv` | f100..f1800 | band not reached in budget | — |
| cfg4 | `=op_u.yuv` + `NNK_MOVIE_YUV2=op_d.yuv` | f10..f4210 (every 10) | **22/22 frames f4000..f4210 flat black** (mean 0.0 sd 0.0) | flat black |
| ref | 5AA build, **no input script** (`p5aa/run4`) | f10..f4380 (every 5) | **corr 0.9987** (MAE 45, `op_u` f0027) | **corr 0.9985** (`op_d` f0027) |

Screen timeline, identical in cfg0/cfg1/cfg4 and in the 5AA run4 up to f4000:
f100..f3600 flat black, f3700 flat 63.8, f3800..f4000 the Mobiclip splash
(mean 61.9 / 55.8 / 55.8), f4100 black, f4200+ the intro movie — so the input
script does not move the story; the band is exactly where the configurations
diverge.

Side-by-sides (`/home/hermes/NiNoKuni/shots/`, native left | reference luma
right, 516x192):

* `p5ac_top_cfg1_f4200.png` — cfg1 top vs `op_u` f22 (the macroblock garbage)
* `p5ac_bot_cfg1_f4200.png` — cfg1 bottom vs `op_d` f15
* `p5ac_top_cfg4_f4200.png` — cfg4 top (black) vs `op_u` f22
* `p5ac_top_5aa_f4200.png` — 5AA working frame vs `op_u` f27
* `p5ac_bot_5aa_f4200.png` — 5AA working bottom vs `op_d` f27

## 2. cfg0 == cfg1 == cfg2, byte for byte

`tools/nnk_smc.inc` `p5t_init()` used to do

```c
const char *p = getenv("NNK_MOVIE_YUV");
p5t_state = 1;
if (!p || !p[0]) return;
p5t_state = 2;              /* "on" for ANY non-empty value */
```

and `p5t_stream()` then tried `fopen(p, "rb")` — with `p = "1"` that fails for
the **first** codec, `p5t_nstr` stays 0, so no stream is ever registered and
`p5t_present()` returns before writing a byte.  `NNK_MOVIE_YUV2` is never even
read, because it is only consulted when `p5t_nstr == 1`.

Measured: cfg0 vs cfg1 → **14/14 frames byte-identical** (f100..f1400, own
runs, `cmp`); cfg2 vs cfg1 → **4/4 identical** (f100..f400); and the
orchestrator's `/tmp/shots_5ab` (44 frames, f100..f4400) is byte-identical to
this phase's own cfg1 f100/f200, which is what pins its provenance to `=1`.
cfg1 is therefore the HLE-off picture, exactly, while the banner still printed
`movie: hle src1=1` on every run.

**Fix (class 40)** — `NNK_MOVIE_YUV` names a source, it is not a boolean:

```c
probe = fopen(p, "rb");
if (!probe) {
    fprintf(stderr, "movie: NNK_MOVIE_YUV=%s is not a readable yuv420p file"
                    " -> HLE OFF (the guest's own decoder runs).  Canonical"
                    " configuration: NNK_MOVIE_YUV=<op_u.yuv>"
                    " NNK_MOVIE_YUV2=<op_d.yuv>\n", p);
    return;
}
fclose(probe);
p5t_state = 2;
```

Cargo class 40 `movie_hle_requires_a_readable_source` (41 tests total) pins the
gate ordering (`fopen` before `p5t_state = 2`), the announced fallback, the
canonical pair in the message, and re-implements the decision for
`{unset, "", "0", "1", "on", "yes", a missing path} -> off`, `{op_u.yuv} -> on`.
One runtime run of the fixed binary with `NNK_MOVIE_YUV=1` (to 330M steps,
`/tmp/p5ac/fix_cfg1.log`) prints the new line instead of `hle src1=1`.

## 3. The mechanism of the band regression

Present logs, same scripted session, same binary:

| build | first present | presents per stream | callback entries |
|---|---|---|---|
| 5AA (`p5aa/run4`, unscripted, 2.45e9) | step 2,256,256,944 = display f4033 | 53 | 106 |
| current shadow-fixed cfg4 (2.36e9) | step 323,488,217 = display **f578** | **7** | 14 |
| current shadow-fixed cfg3 (443M) | step 323,488,334 = display f578 | 7 | 14 |

The same two player objects are involved in both builds — codec `0x0227834C`
with ring slot0 `0x02278800` (player `0x02278264`) and codec `0x022E8B74`
(player `0x022E8A8C`), i.e. the intro's op_u/op_d pair — but in the
shadow-fixed build they are initialised and decoded at display frame 578, deep
inside the pre-splash black region, ~3,455 frames before the movie is on
screen.  Each stream then presents exactly 7 frames (source frames 0..6) and
the decode callback is never entered again through 2.36e9 steps: 7 is the
player's whole ring (six Y slots at `codec+0x0C..0x20` plus one in flight, the
measured `cursor` alternating between two read positions).

So the band is a **consumer problem, not a colour problem**:

* `=path` configs (cfg3/cfg4): the HLE writes the 7 host frames into the ring
  early, then the driver goes quiet.  At the band nothing writes the ring and
  nothing presents → 22/22 flat black frames.  This is exactly the "blackout"
  Phase-5AB reported; it is not a YCoCg/brightness error and it is not caused
  by `NNK_MOVIE_YUV2`.
* `=1`/unset (cfg0/cfg1/cfg2): the guest's own decoder is the only writer; it
  resumes from a reference frame that is no longer the one the stream expects,
  which is precisely what macroblock garbage (DCT blocks, flat grey
  non-predicted areas) looks like — corr 0.29 top, 0.07 bottom.

The one measurement that separates "the shadow fix did this" from "the
scripted session did this" is the 5AA run4 dataset: **without an input script**
that build starts the session at the movie (f4033) and matches at 0.998.  The
scripted session's own screen timeline is byte-identical up to f4000 in every
build, so the script does not re-time the story — the early session is created
by the guest code that only became reachable after the read shadow made the
`0x06006004` vptr round trip work (the 0x020E1B32 site, Phase-5AB).  That call
is now the prime suspect for the pre-load.

Honest gap: a scripted run of the *unscripted* 5AA build was not possible (no
5AA-era binary reproduces the crash-free scripted session), and a current-build
unscripted run started at 14:45 was killed at 93M steps: the box is at load
~38 (four DQ8 emulators from other cards), 2.4e9 steps would have taken >80
minutes.  **Whether the current binary still matches 0.998 without an input
script is therefore unmeasured**, and it decides between "the shadow fix
changed the movie path" and "the script is the trigger".

## 4. Canonical configuration

* **Canonical, measured: `NNK_MOVIE_YUV=<op_u.yuv> NNK_MOVIE_YUV2=<op_d.yuv>`**
  (top screen = op_u, bottom = op_d) — the only configuration that has ever
  produced corr >= 0.99 on both screens (0.9987 / 0.9985 at f4200, 5AA build,
  unscripted session), and it needs a raw 256x192 yuv420p file (73,728 B per
  frame, 1317 frames per stream).
* `NNK_MOVIE_YUV=1` is **now explicitly the guest-decoder path** (`HLE OFF`),
  i.e. the best *of the four measured scripted configurations*: it is the only
  one that shows a picture at all in the band, and it no longer claims to have
  a movie source it does not have.  It is not the acceptance configuration —
  see §5.
* `NNK_MOVIE_YUV2` alone (without a first source) is meaningless and cannot
  enable anything: the source-1 gate now rejects it before stream 2 is looked
  at.

## 5. Remaining gap and named follow-up

1. **Why does the shadow-fixed guest create the movie session at f578 instead
   of f4033?**  Probe the virtual call at 0x020E1B32 (object at 0x06006000,
   the vptr methods stored by pcs 0x020429C8 / 0x020FB130 / 0x020F9004) against
   melonDS's first hit of `MELONDS_REGPROBE_PCS=0x0207B218`: if melonDS's first
   decode call is at ts ~2.6179e9 (f4033) and native's is at 323.4M (f578), the
   new branch is a pre-load that must not run there.
2. **Resume the ring-full driver at the band** (or make the HLE refresh the
   six ring slots per displayed frame once the movie window is on screen), so
   the band is fed even when the guest fills its ring early.  This is the fix
   that turns cfg4 from black back into 0.998 without touching the shadow.
3. Re-run the acceptance (2.4e9 steps, `=op_u` + `=op_d`) **on an idle box**,
   scripted and unscripted, and record both.  Do not run 2.4e9-step captures
   with the machine at load 38: 0.5-0.7M steps/s makes them unaffordable.

## 6. Verification on this branch

```
cd nnktool && cargo test --release   -> 41 passed; 0 failed   (class 40 new)
build: NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
       bash tools/build_split.sh /tmp/p5ac/arm9_p5ac_fix
       -> "prelude splice: ... predates the Phase-5X read shadow", BUILD OK
runtime: NNK_MOVIE_YUV=1 to 330M steps -> "movie: NNK_MOVIE_YUV=1 is not a
       readable yuv420p file -> HLE OFF (the guest's own decoder runs)"
SKIP_MELONDS=1 bash tools/verify_phase2a.sh -> rc=0, green (A 192/192 + 64/64 +
       64/64 + 125/125, A2 PASS, B, D recompile+run, E/F 1000/1000 = 100.0%)
```

No crash regression: the audit run of the current binary under cfg4 reached
`done steps=2360000001 ... halt=step budget exhausted` with **zero**
`untranslated pc` lines (FACT 1 stays fixed; class 39 untouched).
