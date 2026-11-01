# Phase-5AB — the VRAM read shadow was never in the build, and the movie band is black

Follows Phase-5AA (`13b3c45`).  Two items from the operator: (A) a new halt at
0x020E1B32 with `r4=0xE3A03102`, (B) "ВИДЕО КРИВОЕ" even though the top-screen
frames correlate 0.997 with the FFmpeg decode of `op_u`.

**(A) is fixed and verified.**  The read shadow's offset math was never wrong:
the shipped binary had no read shadow at all.  The 13b3c45 build was split from
`scratch/p5aa/split2`, whose `nnk_main.c` contains no `gxd_sh_rd` — Phase-5X put
the shadow in `nnktool/src/recompile.rs` (canonical) and in
`tools/p5x_runtime_splice.py` (measurement), but the build recipe compiles the
FROZEN `work/phase3a/arm9_thumb_ovl_itcm_p5u.c`, which predates both, verbatim.
5Z's "frontier with the shadow" was a spliced-image measurement, never the
build.

**(B) is measured, and it is worse than a colour/pacing error:** with the movie
source enabled the composed screens go black after ~frame 580 and stay exactly
black (mean 0.0, sd 0.0) through the whole movie band.  Verdict and the named
follow-up are in §5.

## 1. (A) Reproduction, register for register

```
cd /tmp/nnk_movie            # the orchestrator's 13:37 build (arm9_intro2)
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
NNK_MOVIE_YUV=1 NNK_STEP_LIMIT=470000000 \
NNK_SMC_IMAGE="0x02000000:arm9.bin,0x020C0000:ovl_p4f4.bin" \
NNK_INPUT_FILE=tools/nnk_operator_session.nnk ./nnk_runner arm9_stored_padded.bin
```

```
untranslated pc=0xe3a03102
halt state: r0=06006004 r1=00000001 r2=00000020 r3=00000001 r4=E3A03102
            r5=06006000 r6=0000449F r7=00000020 r13=027E3A30 r14=020E1B35 nzcv=0000
done steps=333957733 distinct_pcs=23105 halt_pc=0x020E1B32 halt=dispatch to untranslated pc
gx: 602 frames presented
```

That is the operator's halt word for word (he stopped at 469,041,706 steps /
844 frames because he tapped by hand; the code point is identical).

The site is Thumb, from `arm-none-eabi-objdump` on `ovl_p4f4.bin`
(`--adjust-vma=0x020C0000`, `-M force-thumb`):

```
0x020E1B26  adds r0, r5, #4        ; r0 = object+4 = 0x06006004
0x020E1B28  ldr  r4, [r0, #0]      ; the C++ vptr the constructor stored
0x020E1B2C  ldr  r4, [r4, #0x24]   ; the virtual method
0x020E1B32  blx  r4                ; -> 0xE3A03102
```

## 2. (A) The probe: the shadow works when it is there

`tools/p5ab_shadow_probe.py` (new) splices a bounded, PC/step-tagged log into
the guest accessors of a frozen image — st8/16/32, ld8/16/32, one watched window
and, alongside each hit, the shadow's current dword at 0x06006004 (0xDEADBEEF
when no shadow is allocated).  Two runs of that binary, `NNK_MOVIE_YUV` on and
off, are byte-for-byte identical in this log:

```
p5ab w step=333957453 pc=0x020E1ACE a=0x06006000 v=0x0213BC78 sh04=0x00000000
p5ab w step=333957468 pc=0x020429C8 a=0x06006004 v=0x0206927C sh04=0x00000000
p5ab w step=333957504 pc=0x020FB130 a=0x06006004 v=0x0213D074 sh04=0x0206927C
p5ab w step=333957511 pc=0x020F9004 a=0x06006004 v=0x0213CF2C sh04=0x0213D074
p5ab r step=333957727 pc=0x020E1B28 a=0x06006004              sh04=0x0213CF2C
```

The store->load round trip through 0x06006000 is exact (the read at 0x020E1B28
is the `ldr r4,[r0]` of §1) and the run continues: `done steps=340000001
halt_pc=0x0200911C halt=step budget exhausted`, no `untranslated pc`.

## 3. (A) Root cause: the runtime TU, not the guest

| build | `gxd_sh_rd` in the runtime TU | halt at 0x020E1B32 |
|---|---|---|
| `scratch/p5aa/split2` (behind `arm9_intro2` = the 13b3c45 binary) | **absent** | yes |
| `work/split/p5ab_c` (p5u image + canonical phase-5X transform) | present | no |

The guest halves of those two splits are **byte-identical** (`md5sum` of
`nnk_part_001.c`/`002`/`003` match), so the guest was never the variable.  The
frozen image `work/phase3a/arm9_thumb_ovl_itcm_p5u.c` simply predates the
prelude change, and `tools/build_split.sh` compiled it verbatim.

## 4. (A) Fix (class 39) and the measured effect

* `tools/build_split.sh`: if the image named by `NNK_IMAGE_C` does not contain
  `gxd_sh_rd`, the canonical transform (`tools/p5x_runtime_splice.py`, the
  identical text `nnktool/src/recompile.rs` carries) is applied to a build
  artifact next to the split output and THAT image is split.  A frozen image can
  no longer silently drop a committed prelude fix.
* cargo class 39 `vram_read_shadow_round_trips_a_guest_store`: the shadow's three
  functions and the lazy allocation; ld8/16/32 consult it; st8/16/32 all keep it
  current; the offset math re-implemented from the C text (the vptr word's slot,
  window edges, no aliasing between the palette/VRAM/OAM slabs, a straddling
  access refused); and the build-path guard above.
* `cargo test --release` in `nnktool`: **40 passed; 0 failed** (39 -> 40).

Acceptance run (built through the new path, `NNK_MOVIE_YUV` enabled, operator
script, the same SMC images):

```
done steps=470000001 traced=470000000 distinct_pcs=23201 hle_hits=7760
     halt_pc=0x02009114 halt=step budget exhausted
halt state: r0=00000000 r1=6000009F ... r13=027E3A00 r14=02009114 nzcv=0110
gx: 845 frames presented, 205152 unhandled io writes
```

470,000,001 steps — past the operator's 469,041,706 — with no `untranslated pc`
line and a healthy register file.

## 5. (B) The movie output: measured verdict

Run for the audit: the fixed binary, `NNK_MOVIE_YUV=work/p5t/op_u.yuv`
(the op_d decode also exists — `scratch/p5t/op_d.yuv`, 1317 frames), the
operator script, `NNK_GX_SHOT_EVERY=20`, to 2.46e9 steps:

```
done steps=2460000001 distinct_pcs=23201 halt_pc=0x02009114 halt=step budget exhausted
gx: 4398 frames presented
nnk_input: summary script entries=113 applied=113
```

Composed-screen statistics over all 219 captured frames (both screens, luma):

```
frames with content: 28 of 219 (captured 20..4380)
first: f00040 top 255 bot 255, f00100 top 0 bot 34 ... f00580 top 25 bot 26
last : f00580 (top 25, bot 26)
in 4100..4390: []            <-- every sampled movie frame is exactly black
```

* native TOP vs the FFmpeg decode of `op_u.mods`: **corr 0.0000, MAE 0.00**
  (both sides constant; the native side is black at frames 4120..4380).
* native BOTTOM vs the FFmpeg decode of `op_d.mods`: **corr 0.0000, MAE 0.00**.
* side-by-sides: `shots/p5ab_sbs_top_f4140.png`, `shots/p5ab_sbs_bot_f4140.png`
  (native left, reference right; both native panels are black).

So in THIS configuration the complaint is not a YCoCg/brightness or pacing
error: after ~frame 580 neither screen receives any picture at all, and the
movie band is uniformly black — a much stronger defect than 5AA's verified
0.997 top-screen match (that measurement was taken with the 5AA configuration,
which also set `NNK_MOVIE_YUV2`).  The gap is now "no composite output at all",
not "wrong colours".

Ground truth attempted for alignment: a fresh melonDS capture
(`MELONDS_GXLOG_2DEVERY`, 121 `.scr`, frames 4100..4700, same press schedule)
does **not** sit at the same story point — at f4200 it shows an in-game dialogue
on the bottom screen, and several frames read back as capture tearing (noise)
— so melonDS frame numbers are not 1:1 comparable to native here; the older
`scratch/p5p/mel/f04200..f04600.scr` set (both screens carrying the movie) is the
usable melonDS reference for a content-matched comparison.

**Named follow-up (do this first, it is cheap):** bisect the blackout on the same
script — (1) `NNK_MOVIE_YUV` unset (does the screen stay alive past frame 580?),
(2) `NNK_MOVIE_YUV2=$P/op_d.yuv` set (5AA's configuration), (3) with the HLE on,
dump DISPCNT A/B + master brightness + the six ring slots at frame 600.  The
prime suspect is the HLE present path writing a stale codec's six Y ring slots
before the movie exists, and the compositor then handing the movie BG an
all-black plane while it is force-blanked for the movie window.

## 6. Verification

```
cd nnktool && cargo test --release              -> 40 passed; 0 failed   (class 39 new)
SKIP_MELONDS=1 bash tools/verify_phase2a.sh     -> green (A, A2 PASS, C1/C2,
                                                   D recompile+run, E/F 1000/1000)
build: NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
       bash tools/build_split.sh work/split/p5ab_fix_bin
       -> logs "prelude splice: ... predates the Phase-5X read shadow"
       -> 32 TUs compiled, BUILD OK
```

## 7. Open frontier after this phase

1. The blackout bisect above (the movie output audit's next half).
2. Once the movie band renders again: re-run the (B) measurement with
   `NNK_MOVIE_YUV2=scratch/p5t/op_d.yuv` so the two screens get their own
   streams, and compare bottom-screen frames against the op_d decode.
3. Regenerate the dispatch image from `nnktool/src/recompile.rs` when the
   recompile-all pipeline is next run, so the frozen-image splice becomes a
   no-op (it is idempotent by construction).
