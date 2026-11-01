# Phase-5T — HLE the movie decode: the plan, the plane contract, the hook

Follows the operator's morning directive (the movie path switches to HLE; the
LLE chain of barriers I6/I7 in `PHASE5R.md` is deliberately left open) and the
DQ8 HLE decoder work on branch `feature/movie-hle-t_15eff66c`
(`reports/MOVIE_HLE.md`: override at the codec entry, host frame pacing =
decode-step count, ladder side-effect emulation).

## 1. The stream is a standard MobiClip MODS movie — verified with the oracle

The intro is `data/movie/op_u.mods` in the FNT (`work/fnt_full.txt`, file id
9243, ROM 0x0BB33A00, 0x1FFD44 bytes).  `tools/p5t_extract.py` pulls it out of
the ROM through the FAT; its header is exactly the structure the guest holds:

```
4d 4f 44 53 "MODS" | "N3 \n\0" | u32 frames = 0x525 (1317) | w = 0x100 |
h = 0xC0 | ... | u32 0x1E = 30 fps
```

The local FFmpeg 9.0.1 (`~/.hermes/tools/ffmpeg-9.0.1-linux-x64/bin/ffmpeg`)
**has the `mods` demuxer and the `mobiclip` decoder** (the DQ8 branch only
found `moflex`), so the public spec reaches this container directly:

```
Input #0, mods, from 'op_u.mods': Duration 00:01:49.75, bitrate 153 kb/s
  Stream #0:0: Video: mobiclip, yuv420p (ycgco), 256x192, 12 fps
1317 frames decoded in 0.55 s
```

⇒ `mobiclip` + `yuv420p` + **colorspace `ycgco`** (the MODS variant; MOFLEX is
YCbCr).  No host-side container reverse engineering is needed: the frame that
the guest's decoder produces can be compared against this oracle exactly.

## 2. THE PLANE CONTRACT — measured, and it is a plain raster Y plane

Phase-5P's melonDS RAM dump (`ts = 2.6179e9`, plane at ring slot `0x022C0880`)
matched against every frame of both intro candidates
(`tools/p5t_match_plane.py`, 256x192 8-bit, 2x-downsampled review PNG):

| movie | best frame | corr | MAE |
|---|---|---|---|
| `op_d.mods` | f0976 | 0.28 | 49.5 |
| `op_u.mods` | **f0130** | **1.0000** | **0.0** |

**`0x022C0880` == the reference decoder's Y plane of `op_u.mods` frame 130,
byte for byte (0/49152 samples differ).**  So:

* the intro movie is `op_u.mods` (not `op_d`), and
* the guest's decoded frame buffer is a **plain raster 8-bit Y plane**, 256x192,
  stride 256 — the HLE only has to write that plane; no per-block/tiled layout
  is involved.

`tools/p5t_codec_ptrs.py` then reads the codec struct at the callback argument
(`0x0227834C`, identical in both engines, `PHASE5R.md` §3) out of the same dump:

```
+0x00 0x022E52B0   stream cursor (the guest advances it by the callback's return)
+0x04 0x00000100   width 256        +0x08 0x000000C0  height 192
+0x0C 0x022C0880   \                  +0x24 0x022CC890
+0x10 0x022AE860    |  six Y ring    +0x28 0x022BA870
+0x14 0x0229C840    |  slots         +0x2C 0x022A8850
+0x18 0x0228A820    |  (0x12020     +0x30 0x02296830
+0x1C 0x02278800    |   apart)       +0x34 0x02284810
+0x20 0x022D28A0   /                +0x38 0x022DE8B0
+0x1B4            the signed DC-offset field of barrier I6 (LLE side)
```

Each "second" pointer is exactly `Y + 0xC010`, i.e. a slot is
`Y (0xC000) + 0x10 pad + U (0x3000) + V (0x3000) + 0x10` = `0x12020`.  The
player object `0x02278264` (Phase-5P) carries the callback at `+0x38`, the
current/next source plane at `+0x3C`/`+0x40`, the VRAM destination at `+0x44`
and the 0x200 stride at `+0x48`.

## 3. The hook (the DQ8 recipe, at the NNK decode step)

`tools/nnk_smc.inc` now carries the Phase-5T HLE, off unless `NNK_MOVIE_YUV`
names a raw yuv420p file:

* **entry** — when the live ITCM decoder reaches `0x01FF8540` (the callback the
  player calls through `blx r1` at `0x0207B218`, `PHASE5M.md`), `r0` names the
  codec struct; the hook records it and resets the frame index on a new
  session (a stream cursor that moves backwards);
* **the guest callback still runs unchanged.**  Queue depth, stream cursor,
  slot ring, pacing and the display state machine therefore advance exactly as
  they do with the guest decoder — that is the DQ8 lesson (their §14: skipping
  the decode made the queue depth hit capacity and the player saw EOF after 6
  frames);
* **return** — when the callback returns to its caller (`0x0207B21C`, the
  instruction after the `blx`), the host frame for this decode step is copied
  over the player's source plane (`+0x3C`, `+0x40`) and all six Y ring slots.
  The guest's own converter/compositor then presents host pixels — that path is
  the one Phase-5P proved faithful (native composed screen correlates 0.44-0.47
  with its own ring buffers, and the ring plane reaches the screen unchanged).

Frame index = decode steps since the session started (one source frame per
step, the guest's own `blx` contract), clamped at the last frame past EOF.

## 5. Result (measured, this run) — the hook fires, the session does not yet play

Run: `p5t2` (image `arm9_thumb_ovl_itcm_p5s2.c` + this hook),
`NNK_STEP_LIMIT=2.33e9`, `NNK_MOVIE_YUV=work/p5t/op_u.yuv`,
`NNK_GX_SHOT_EVERY=20`, log `p5t/run3.log`.

```
done steps=2330000001 ... halt=step budget exhausted
movie: hle source=.../op_u.yuv frames=1317 plane=256x192
movie: present n=1 frame=0 cursor=0x022E48E8 src=0x00000020 step=2256256944
movie: session codec 0x0227834C -> 0x022E8B74 at step 2256257061 (last index 0, presents 1)
movie: present n=2 frame=0 cursor=0x02355110 src=0x00000020 step=2256353923
```

What that measures, honestly:

* **the hook is reached and fires** — the callback entry `0x01FF8540` arrives
  once the player starts (step 2,256,256,944, i.e. exactly the movie window
  Phase-5M/P measured), `r0 = 0x0227834C` on the first call: the codec struct
  address this phase read out of the melonDS dump;
* **only two callback entries in the following 74 M steps**, and the second
  arrives with `r0 = 0x022E8B74` (a ring-adjacent RAM address, not the codec
  struct) — i.e. after the first decode step the player is not driving the
  decode callback in a per-frame loop in this image;
* **no GX capture exists past display frame 4160** (208 top-screen PPMs, the
  last one black) while Phase-5P's runs captured 4,200..4,800 — so in this
  build the display tick stops advancing exactly when the movie path starts,
  before any frame can be compared;
* `player+0x3C` reads `0x20` at the return point, i.e. the player object's
  source-plane fields are not yet populated at the first decode step, so the
  first present wrote nothing (its six codec slot pointers were still zero).

### The two named blockers (both bounded, both measurement-shaped)

**B1 — the display clock stalls when the movie path starts.** `gxd_frames`
stops at 4160 although the guest keeps executing to the step budget
(`distinct_pcs=21993`). `nnk_display_tick()` only renders when `vcount()` crosses
into 192; the movie loop is the first place where the guest spends nearly all
its time in **live-decoded ITCM** (`nnk_smc_exec_any`), and that path charges
`card_tick(cost)` from `cost` the live decoder returns — if those ITCM forms
return `cost = 0` the emulated time-of-day never crosses a scanline, so no
frame is ever rendered or dumped.  Measurement: run the same image with
`NNK_MOVIE_YUV` unset and `NNK_ITCMDUMP`-style sampling of `vcount()`/`steps`
across 2.256..2.30e9, or simply print `steps`+`vcount()` every 10 M steps; the
fix is to charge a non-zero cost for live-decoded ITCM instructions (the
main-RAM live path already does).

**B2 — the player does not iterate the decode callback per frame.** The second
entry carries `r0 = 0x022E8B74`, and the codec struct's own ring slots
(`+0x0C..+0x20`) are still zero at the first return, so the movie player is
still initialising when the first decode step fires.  The guest's
`decode step` loop is at `0x01FF8540`'s caller `0x0207B21C`.. and the ring is
driven from `0x02278264`'s fields, which are also still unset (`+0x3C = 0x20`).
Next measurement (one run, no rebuild): `NNK_STOREWATCH="0x02278260:0x022782C0"`
plus `NNK_PRED_PC=0x01FF8540,0x0207B218,0x0207B21C`, and dump the ITCM-relative
`steps`/`vcount()` from B1 — that names which of the two stops first.

What is *not* in doubt: the decoder port itself.  The host side decodes the
game's own `op_u.mods` to the guest's exact plane (byte-identical Y for frame
130 of the melonDS dump), so once the plane is written at the right moment the
converter/compositor path Phase-5P proved faithful is fed correct pixels.

## 6. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5t
# 1. the movie and its host decode (the reference decode is the oracle)
python3 tools/p5t_extract.py '/home/hermes/NiNoKuni/Ni No Kuni.nds' \
        work/fnt_full.txt data/movie/op_u.mods $S/op_u.mods
$FFMPEG -i $S/op_u.mods -map 0:v -f rawvideo -pix_fmt yuv420p work/p5t/op_u.yuv
# 2. the measurements above
python3 tools/p5t_match_plane.py $S/op_u.yuv shots/p5p_mel_dec8_022C0880.png 1400
python3 tools/p5t_codec_ptrs.py <melonDS RAM dump> $S/op_u.yuv 130
# 3. build + run
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5s2.c \
  bash tools/build_split.sh $S/p5t2
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2330000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_MOVIE_YUV=$PWD/work/p5t/op_u.yuv NNK_GX_SHOT=$S/shots/f NNK_GX_SHOT_EVERY=20 \
    $S/p5t2 work/phase3a/arm9_stored_padded.bin
python3 tools/p5t_check_frames.py work/p5t/op_u.yuv "$S/shots/*.top.ppm" shots p5t
```
