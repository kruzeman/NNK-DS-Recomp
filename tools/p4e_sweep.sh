#!/bin/sh
# Phase-4E translation sweep + live-display build, in ONE pass.
#
# The boot's coverage class is "PC the guest executes that the dispatch image
# has no case for".  Two sources of that class exist and both are closed here
# with the same mechanism (seed the PC, recompile, verify the gap set is 0):
#
#   1. mid-body holes INSIDE the static ARM9 image (0x02000000..0x0206DA04)
#      -> tools/mk_trace_seeds.py against the 30M-PC melonDS trace, with the
#         per-PC ARM/Thumb MODE trace as ground truth (a 4-aligned PC inside
#         Thumb code is otherwise mis-decoded as ARM);
#   2. runtime-loaded OVERLAY modules in main RAM (0x020C0000..0x02180000):
#      NITRO overlays the boot copies from the card and calls.  They are not
#      in the static image at all, so they are recompiled from a memory dump
#      of the loaded slots and spliced in as extra dispatch cases.
#
# Inputs (all in the MAIN checkout's work/, gitignored):
#   work/ghidra_src/arm9.bin            static image
#   work/phase3a/seeds_phase3a3.tsv     baseline (Ghidra + 10M-PC trace) seeds
#   work/phase3a/ovl_p4e.bin            dump of 0x020C0000..0x02180000
#   $S/itcm_image_p4d.c                 ITCM blob dispatch image
#   $S/melonds_p4e_pc.txt               30M-PC melonDS trace (ground truth)
#   $S/melonds_p4e_mode.txt             per-PC ARM/Thumb mode trace
#
#   S=<scratch> tools/p4e_sweep.sh <tag>
#
# Output: out/split/arm9_gx (windowed/headless SDL2 display build).
set -e
cd "$(dirname "$0")/.."
S=${S:-/home/hermes/.hermes/profiles/coder/cache/scratch}
TAG=${1:-p4e2}
NNK=nnktool/target/release/nnktool
IMG=work/ghidra_src/arm9.bin
PREV_C=${PREV_C:-work/phase3a/arm9_thumb_p4e.c}
BASE_SEEDS=${BASE_SEEDS:-work/phase3a/seeds_phase3a3.tsv}

echo "=== 1. gap seeds (static image) from the 30M-PC trace + mode trace ==="
python3 tools/mk_trace_seeds.py "$S/melonds_p4e_pc.txt" "$PREV_C" "$BASE_SEEDS" \
    "work/phase3a/seeds_$TAG.tsv" "$S/melonds_p4e_mode.txt"

echo
echo "=== 2. recompile the static image with those seeds ==="
$NNK recompile-all "$IMG" "work/phase3a/arm9_thumb_$TAG.c" --base 0x02000000 \
    --entry 0x02000800 --seeds "work/phase3a/seeds_$TAG.tsv" 2>&1 | tail -4

echo
echo "=== 3. overlay seeds (per-PC ARM/Thumb from the mode trace) ==="
python3 tools/mk_region_seeds.py "$S/melonds_p4e_pc.txt" 0x020C0000 0x02180000 \
    work/phase3a/seeds_p4e_ovl.tsv "$S/melonds_p4e_mode.txt" | head -4
$NNK recompile-all work/phase3a/ovl_p4e.bin work/phase3a/ovl_p4e.c \
    --base 0x020C0000 --entry 0x02139E48 --seeds work/phase3a/seeds_p4e_ovl.tsv \
    2>&1 | grep -E "recompile-all|thumb instructions"

echo
echo "=== 4. splice overlay + ITCM cases into the image ==="
python3 tools/merge_itcm_cases.py "work/phase3a/arm9_thumb_$TAG.c" work/phase3a/ovl_p4e.c \
    "work/phase3a/arm9_thumb_ovl_$TAG.c" - 0x020C0000 0x02180000
python3 tools/merge_itcm_cases.py "work/phase3a/arm9_thumb_ovl_$TAG.c" "$S/itcm_image_p4d.c" \
    "work/phase3a/arm9_thumb_ovl_itcm_$TAG.c" "$S/melonds_arm9_trace_itcm.txt"

echo
echo "=== 5. split build with the live gx_display bridge ==="
SPLIT_DIR=out/split/c NNK_IMAGE_C="work/phase3a/arm9_thumb_ovl_itcm_$TAG.c" \
    NNK_GX=1 BUILD_JOBS=12 tools/build_split.sh out/split/arm9_gx | tail -3
