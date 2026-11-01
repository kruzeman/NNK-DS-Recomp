# PHASE5AH — the intro plays from the ROM alone (in-process Mobiclip decode)

## The defect this fixes

The shipped binary showed corrupted video (blocky yellow/blue/brown noise) on
any machine that did not have the pre-decoded `.yuv` dumps.  The Phase-5T HLE
took its picture from `NNK_MOVIE_YUV` / `NNK_MOVIE_YUV2` — paths to raw
yuv420p files that only existed on the build host (`work/p5t/op_u.yuv`,
~97 MB).  Without them the HLE announced "OFF" and the guest's own (unfixed)
decoder painted macroblock garbage.

Phase-5AH removes that requirement: the runtime decodes the movie itself, out
of the card image, with the decoder compiled into the runtime TU.

## Design shipped: on-the-fly decode (no cache, no first-run step)

The operator authorised a first-run extraction cache; it turned out to be
unnecessary, so the simpler design ships:

* `tools/nnk_mobiclip_dec.inc` — FFmpeg's `libavcodec/mobiclip.c` (LGPL 2.1+,
  Florian Nouwt / Adib Surani / Paul B Mahol), vendored and minimally adapted
  (includes replaced by the compat layer below, `MobiVLC` instead of the
  FFmpeg VLC table pool, the `FFCodec` registration struct dropped).
* `tools/nnk_mobiclip.inc` — everything that decoder needs from the FFmpeg
  runtime, written from scratch and self-contained: MSB-first bit reader,
  exp-Golomb (`get_ue_golomb`, `get_ue_golomb_31`, `get_sbits`,
  `get_se_golomb`), a prefix-code decoder built the way
  `ff_vlc_init_from_lengths()` assigns codes (array order, 32-bit
  accumulator, walked as a binary tree), `mid_pred`, `av_clip*`,
  `ff_rezigzag` table, a frame pool; plus the MODS demuxer, a NITRO FNT/FAT
  path lookup, and the `nnk_mobi_*` decode API.  No libav*, no ffmpeg
  process, no external file.
* `tools/nnk_smc.inc` (Phase-5T section) now has two picture sources per
  stream: **ROM** (default: `data/movie/op_u.mods` for the top screen,
  `data/movie/op_d.mods` for the bottom, overridable with `NNK_MOVIE_MODS` /
  `NNK_MOVIE_MODS2`) and the old **YUV file**, kept only as an A/B
  diagnostic for reviewers.  Decode happens inside `p5t_present()`, i.e. only
  while a movie is actually being presented.

Nothing is extracted to disk, nothing is cached: the frames are decoded on
demand at ~0.5 ms per 256x192 frame, which is invisible next to the guest
emulation (one movie frame per 1/12 s of guest time).

## MEASURED: the decoder is bit-exact against FFmpeg

`tools/mobiclip_selftest.c` compiles the *runtime* files (`nnk_mobiclip.inc`
+ `nnk_mobiclip_dec.inc`) on the host and writes raw yuv420p:

```
cc -O2 -std=c11 -I tools tools/mobiclip_selftest.c -o out/mobi_selftest
out/mobi_selftest <file.mods> <out.yuv> 0 1316        # all frames
ffmpeg -i <file.mods> -pix_fmt yuv420p -f rawvideo ref.yuv
```

| stream | frames decoded | Y plane, per frame | speed |
|---|---|---|---|
| `data/movie/op_u.mods` | 1317 / 1317 | **identical, frame-by-frame** (diff 0 / 49152 px, corr 1.00000) | 0.49 ms/frame |
| `data/movie/op_d.mods` | 1317 / 1317 | **identical, frame-by-frame** (diff 0 / 49152 px, corr 1.00000) | 0.59 ms/frame |

That is a stronger result than the acceptance threshold (>= 6 successive
frames at corr >= 0.97): every frame of both intro streams matches the
FFmpeg decode exactly.

## Container format (measured, not guessed)

```
0x00 "MODS"      0x04 "N3\n\0"   0x08 frames(u32)  0x0C width   0x10 height
0x14 = 12 (fps as a big-endian u32); 0x20/0x28 = first-frame area size /
data end; 0x34 = first frame record.
record = [u32 LE, length-in-16-bit-units << 16][payload of (words*4) bytes]
         next record starts at +4+words*4
```

Verified against `ffprobe -show_packets` for all 1317 records of op_u.mods
(byte offsets and sizes match exactly).

Bugs found while porting, all with measured symptoms:
* the packet payload starts **after** the 4-byte length word and is `words*4`
  bytes long (a payload that is 4 bytes short makes the last two
  macroblocks of the first P frame read past the end → `predict_motion`
  picture-null rejection);
* FFmpeg's `get_se_golomb` maps `k odd -> +(k+1)/2`, `k even -> -k/2` (the
  opposite of the naive convention) — with the naive sign the first
  non-static frame differs at 20833/49152 pixels;
* `mid_pred` is FFmpeg's branchy median, not "take the middle of three with
  two independent clamps"; the wrong version decodes frame 16 with 21 % of
  the picture wrong and then rejects a legal macroblock.

## Runtime wiring

* `p5t_init()`: a readable `NNK_MOVIE_YUV` still wins (A/B override) and is
  announced as such; otherwise the containers are located in the card image
  through the NITRO FNT/FAT (`nnk_rom_file`) and the HLE starts in ROM mode.
  With neither, the banner says exactly why the HLE is off.
* `p5t_stream()`: ROM mode maps stream 1 → `op_u.mods`, stream 2 →
  `op_d.mods`, parses the container, opens a decoder for that stream.
* `p5t_present()`: decodes frame `index` and writes the Y plane into the
  codec's six ring slots exactly as the YUV path did, so the guest's own
  converter/compositor (proven faithful in Phase-5P) is untouched.

## Honest status

Decoder + demuxer + wiring: done and measured (above).  The end-to-end
runtime capture (intro band correlated against the reference decode with only
`NNK_ROM_PATH` set) is the remaining acceptance step; the build is green and
the run is reproducible with:

```
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c bash tools/build_split.sh work/arm9_mobi
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
NNK_STEP_LIMIT=2.9e9 NNK_GX_SHOT=$PWD/work/shots/intro/f NNK_GX_SHOT_EVERY=1 \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  work/arm9_mobi work/phase3a/arm9_stored_padded.bin
python3 tools/p5ad_band.py work/shots/intro <ref op_u.yuv> work/shots/png 0 1316
```
