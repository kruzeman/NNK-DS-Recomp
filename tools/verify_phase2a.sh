#!/bin/sh
# Phase-2A acceptance verification (Ni no Kuni DS ARM9).
# Rebuilds nnktool, re-runs every acceptance check, prints one summary per
# check.  Set SKIP_MELONDS=1 to reuse an existing melonDS trace.
#
#   sh tools/verify_phase2a.sh
#
set -e
cd /home/hermes/NiNoKuni
NNK=nnktool/target/release/nnktool
W=work/phase2a
IMG=work/ghidra_src/arm9.bin
GH=work/ghidra_arm9_functions.tsv
ROM="/home/hermes/NiNoKuni/Ni No Kuni.nds"
SCRATCH=/home/hermes/.hermes/profiles/coder/cache/scratch
MTRACE=$SCRATCH/melonds_arm9_trace.txt
MELONDS=/home/hermes/tools/melonDS/build-ptrace/melonDS

echo "=== build ==="
cargo build --release --manifest-path nnktool/Cargo.toml 2>&1 | tail -1

echo
echo "=== A. regression: nnktool disasm vs arm-none-eabi-objdump (4 ranges) ==="
sh tools/verify_phase1a.sh 2>&1 | sed -n '/=== A\./,/=== B\./p' | grep -E "encoding match|text match|both match|--"

echo
echo "=== B. seeded recovery from ALL Ghidra function entries ==="
$NNK decompile $IMG $W --base 0x02000000 --entry 0x02000800 --seeds $GH

echo
echo "--- C1. entry-address agreement (crosscheck) ---"
python3 tools/crosscheck_ghidra.py $W/arm9_functions.tsv $GH | sed -n '1,9p'
echo "--- C2. Ghidra body coverage (does not depend on the seed list) ---"
python3 tools/coverage_ghidra.py $W/arm9_decompile.txt $GH

echo
echo "=== D. recompile-all -> cc -> run (guest PC trace + HLE log) ==="
$NNK recompile-all $IMG $W/arm9_all.c --base 0x02000000 --entry 0x02000800 --seeds $GH
# -O0 keeps this acceptance run fast (cc is the bottleneck on a 17 MB file:
# -O0 ~2 min, -O1 minutes, -O2 ~15 min).  Optimization level does not change
# the generated semantics, only compile/run time.
cc -O0 -DSTEP_LIMIT=20000000 -DTRACE_LIMIT=2000000 -o $W/arm9_all $W/arm9_all.c
$W/arm9_all $W/arm9_all.image.bin > $W/native_trace.txt 2> $W/native_hle.txt || true
echo "native trace lines : $(grep -c '^0x' $W/native_trace.txt)"
echo "native summary     : $(grep '^done ' $W/native_hle.txt)"
echo "HLE hits           : $(grep -c '^hle swi' $W/native_hle.txt)"
grep '^hle swi' $W/native_hle.txt | sort | uniq -c | head -20 || true
echo "HLE semantics implemented: 0x09 Div, 0x0B CpuSet, 0x0C CpuFastSet, 0x0D Sqrt"
echo "  (FreeBIOS numbering, freebios/src/bios_common.s; anything else halts)"

echo
echo "=== E. ground truth: melonDS ARM9 PC trace (first 1000 guest PCs) ==="
if [ "$SKIP_MELONDS" = "1" ] && [ -s "$MTRACE" ]; then
    echo "reusing $MTRACE"
else
    rm -f "$MTRACE"
    timeout 180 env XDG_CONFIG_HOME=$SCRATCH/melonds_cfg SDL_AUDIODRIVER=dummy \
        MELONDS_ARM9TRACE_PATH=$MTRACE MELONDS_ARM9TRACE_N=1000 \
        xvfb-run -a "$MELONDS" "$ROM" > $SCRATCH/melonds_verify.log 2>&1 || true
    echo "melonDS trace lines: $(grep -c '^0x' $MTRACE)"
fi
echo "SWIs in the melonDS trace: $(grep -c '^# swi' $MTRACE || true)"

echo
echo "=== F. melonDS vs native PC trace comparison ==="
python3 tools/cmp_pc_traces.py $MTRACE $W/native_trace.txt
