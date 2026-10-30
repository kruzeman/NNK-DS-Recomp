#!/bin/sh
# Phase-4G translation sweep: per-load overlay slot images + runtime slot
# registry, then the split build with the live gx_display bridge.
#
# Same one-pass structure as tools/p4e_sweep.sh (see that file for the why of
# each step).  What Phase-4G adds:
#
#   (1) the overlay slot image is now assembled PER LOAD GENERATION:
#         tools/dump_ovl_targets.py  -- per-overlay ROM images + arenas +
#                                       the address runs where two overlays of
#                                       one arena disagree (conflicts)
#         tools/ovl_registry.py      -- the registry: generations from the
#                                       runtime's NNK_OVL_TRACE log + the
#                                       resident-module match of a captured
#                                       slot dump
#       The base snapshot (OVL_BIN) still supplies the arenas whose loads the
#       earlier phases verified; the resident arena of the current run is
#       overwritten from a per-load slot capture (OVL_SLOT_DUMP) because the
#       guest decrypts and initialises pages of the resident overlay at run
#       time -- an image compiled from the ROM alone cannot contain them.
#
#   (2) the generated runtime carries the overlay-slot registry
#       (nnktool/src/recompile.rs, NNK_OVL_TRACE=<path> writes one
#       `ovl_load id=.. rom=.. dest=.. gen=..` line per load).
#
#   S=<scratch> tools/p4g_sweep.sh <tag>
#
# Knobs: PREV_C, BASE_SEEDS, OVL_BIN, OVL_SLOT_DUMP, OVL_SLOT_ADDR,
#        OVL_SLOT_SIZE, SKIP_SEEDS=1
set -e
cd "$(dirname "$0")/.."
S=${S:-/home/hermes/.hermes/profiles/coder/cache/scratch}
TAG=${1:-p4g}
NNK=nnktool/target/release/nnktool
IMG=work/ghidra_src/arm9.bin
PREV_C=${PREV_C:-work/phase3a/arm9_thumb_p4f2.c}
BASE_SEEDS=${BASE_SEEDS:-work/phase3a/seeds_p4f2.tsv}
OVL_BIN=${OVL_BIN:-work/phase3a/ovl_p4f4.bin}
OVL_SLOT_DUMP=${OVL_SLOT_DUMP:-}
OVL_SLOT_ADDR=${OVL_SLOT_ADDR:-0x02159FE0}
OVL_SLOT_SIZE=${OVL_SLOT_SIZE:-0x21E0}

echo "=== 1. gap seeds (static image) from the 30M-PC trace + mode trace ==="
if [ -n "$SKIP_SEEDS" ]; then
    cp "$BASE_SEEDS" "work/phase3a/seeds_$TAG.tsv"
    echo "  (reusing $BASE_SEEDS)"
else
    python3 tools/mk_trace_seeds.py "$S/melonds_p4e_pc.txt" "$PREV_C" "$BASE_SEEDS" \
        "work/phase3a/seeds_$TAG.tsv" "$S/melonds_p4e_mode.txt"
fi

echo
echo "=== 2. recompile the static image with those seeds ==="
$NNK recompile-all "$IMG" "work/phase3a/arm9_thumb_$TAG.c" --base 0x02000000 \
    --entry 0x02000800 --seeds "work/phase3a/seeds_$TAG.tsv" 2>&1 | tail -3

echo
echo "=== 3. per-load overlay slot images (Phase-4G) ==="
ROM=${ROM:-Ni No Kuni.nds}
[ -f "$ROM" ] || ROM="/home/hermes/NiNoKuni/Ni No Kuni.nds"
python3 tools/dump_ovl_targets.py "$ROM" work/ovl_targets
python3 tools/ovl_registry.py conflicts --images work/ovl_targets > work/ovl_targets/conflicts.txt 2>&1 || true
head -8 work/ovl_targets/conflicts.txt
SLOT="work/phase3a/ovl_$TAG.bin"
cp "$OVL_BIN" "$SLOT"
if [ -n "$OVL_SLOT_DUMP" ]; then
    python3 tools/ovl_registry.py match --dump "$OVL_SLOT_DUMP" --images work/ovl_targets | tail -6
    python3 tools/ovl_registry.py slots --dump "$OVL_SLOT_DUMP" \
        --arena "$OVL_SLOT_ADDR" --size "$OVL_SLOT_SIZE" --out "work/phase3a/slot_$TAG.bin"
    python3 - "$SLOT" "work/phase3a/slot_$TAG.bin" "$OVL_SLOT_ADDR" <<'PY'
import sys
slot, cap, addr = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
base = 0x020C0000
img = bytearray(open(slot, "rb").read())
c = open(cap, "rb").read()
off = addr - base
n = 0
for i, b in enumerate(c):
    if b and img[off + i] != b:
        img[off + i] = b
        n += 1
open(slot, "wb").write(img)
print("per-load slot image: %d bytes refreshed at 0x%08X (0x%X captured)" % (n, addr, len(c)))
PY
fi

echo "=== 4. overlay seeds + recompile the slot image ==="
python3 tools/mk_region_seeds.py "$S/melonds_p4e_pc.txt" 0x020C0000 0x02180000 \
    "work/phase3a/seeds_${TAG}_ovl.tsv" "$S/melonds_p4e_mode.txt" | head -3
$NNK recompile-all "$SLOT" "work/phase3a/ovl_$TAG.c" \
    --base 0x020C0000 --entry 0x02139E48 --seeds "work/phase3a/seeds_${TAG}_ovl.tsv" \
    2>&1 | tail -3

echo
echo "=== 5. splice overlay + ITCM cases into the image ==="
python3 tools/merge_itcm_cases.py "work/phase3a/arm9_thumb_$TAG.c" "work/phase3a/ovl_$TAG.c" \
    "work/phase3a/arm9_thumb_ovl_$TAG.c" - 0x020C0000 0x02180000
python3 tools/merge_itcm_cases.py "work/phase3a/arm9_thumb_ovl_$TAG.c" "$S/itcm_image_p4d.c" \
    "work/phase3a/arm9_thumb_ovl_itcm_$TAG.c" "$S/melonds_arm9_trace_itcm.txt"

echo
echo "=== 6. split build with the live gx_display bridge ==="
SPLIT_DIR=out/split/c NNK_IMAGE_C="work/phase3a/arm9_thumb_ovl_itcm_$TAG.c" \
    NNK_GX=1 BUILD_JOBS=12 tools/build_split.sh out/split/arm9_gx | tail -3
