#!/usr/bin/env bash
# Ni no Kuni DS (B2KJ) — run the recompiled game from the card image ALONE.
#
#   ./run.sh "Ni No Kuni.nds"
#
# The ROM is the only input (Phase-5AJ).  The guest image the dispatch loop
# boots, the decompressed ARM9 that the dispatch image was compiled from and
# the overlay arena window are all derived from the card image in memory at
# start-up (tools/nnk_rom.inc).  Nothing else is read and no environment
# variable is required:
#
#   $ mkdir /tmp/clean && cd /tmp/clean
#   $ cp <repo>/out/p5aj/nnk_runner . ; cp "/path/Ni No Kuni.nds" .
#   $ ./nnk_runner "Ni No Kuni.nds"
#   rom: card image Ni No Kuni.nds (...) arm9 299816 B @0x4000 -> 0x02000000 ...
#   smc: generation-0 arm9 0x02000000..0x0206CB78 (445304 bytes) derived from the ROM
#
# BUILD (the dispatch image is a frozen artifact; the splices + prelude audit
# run inside build_split.sh):
#   NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \
#       bash tools/build_split.sh out/p5aj/nnk_runner
#
# OPTIONAL (debug/reviewer tools — never requirements):
#   NNK_GX=window             show the SDL2 window instead of the headless path
#   NNK_STEP_LIMIT=<n>        stop after n guest instructions
#   NNK_GX_SHOT=<base>        write <base>.<frame>.top.ppm / .bot.ppm captures
#   NNK_GX_SHOT_EVERY=<n>     ... every n-th presented frame
#   NNK_SMC_IMAGE="0xADDR:path[,...]"
#                             A/B the SMC generation-0 snapshot from files
#                             instead of the ROM (what every phase before 5AJ
#                             required: 0x02000000:<arm9.bin>,0x020C0000:<ovl_p4f4.bin>)
#   NNK_SMC_GEN0_OVL=<id>     overlay placed in the derived arena window (17)
#   NNK_ROM_PATH=<path>       card image for the card model when argv[1] is a
#                             raw guest image (legacy A/B path only)
#   NNK_MOVIE_YUV=<file.yuv>  A/B the movie HLE against a raw yuv420p dump
#                             instead of decoding data/movie/*.mods from the ROM
#   NNK_MOVIE_YUV2=<file.yuv> ... for the bottom screen
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

ROM=${1:-}
RUNNER=${NNK_RUNNER:-out/p5aj/nnk_runner}

if [[ -z $ROM ]]; then
  echo "usage: ./run.sh \"/path/Ni No Kuni.nds\"" >&2
  echo "  the card image is the only input; every guest image is derived from it" >&2
  exit 2
fi
if [[ ! -f $ROM ]]; then
  echo "run.sh: no such card image: $ROM" >&2
  exit 2
fi
if [[ ! -x $RUNNER ]]; then
  echo "run.sh: no runner at $RUNNER — build it first:" >&2
  echo "  NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c \\" >&2
  echo "      bash tools/build_split.sh out/p5aj/nnk_runner" >&2
  exit 2
fi

# Headless by default: no SDL window, but the display bridge still renders and
# presents every frame, so captures and correlations are real.
export NNK_GX=${NNK_GX:-headless}
exec "$RUNNER" "$ROM"
