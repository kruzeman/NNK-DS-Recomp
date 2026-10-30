#!/bin/sh
# Phase-4C iterate helper: rebuild the emitter, regenerate the two-region
# dispatch image, split-build it, then run the native boot and compare the PC
# trace against the measured melonDS capture.
#   sh tools/it_irq.sh [native_trace_out]
set -e
cd "$(dirname "$0")/.."
S=/home/hermes/.hermes/profiles/coder/cache/scratch
NNK=nnktool/target/release/nnktool
OUT=${1:-$S/nat_pc.txt}
LOG=${OUT%.txt}.log
IMG=/home/hermes/NiNoKuni/work/phase3a

cargo build --release --manifest-path nnktool/Cargo.toml 2>&1 | grep -E "^error" -A3 && exit 1
$NNK recompile-all /home/hermes/NiNoKuni/work/ghidra_src/arm9.bin $IMG/arm9_thumb.c \
    --base 0x02000000 --entry 0x02000800 --seeds $IMG/seeds_phase3a3.tsv >/dev/null
$NNK recompile-all $S/itcm_blob.bin $S/itcm_image.c \
    --base 0x01FF8000 --entry 0x01FF8000 --seeds $S/seeds_itcm.tsv >/dev/null
python3 tools/merge_itcm_cases.py $IMG/arm9_thumb.c $S/itcm_image.c $IMG/arm9_thumb_itcm.c \
    $S/melonds_arm9_trace_itcm.txt | tail -1
NNK_IMAGE_C=$IMG/arm9_thumb_itcm.c NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=20000000ull" \
    tools/build_split.sh 2>&1 | tail -2
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" work/split/arm9_split \
    $IMG/arm9_stored_padded.bin > "$OUT" 2> "$LOG"
tail -2 "$LOG"
python3 tools/cmp_pc_traces.py $S/melonds_irq_pc.txt "$OUT" --lo 0x01FF8000 --hi 0x02400000 | sed -n '1,8p'
