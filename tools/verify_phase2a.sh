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
echo "=== A2. Thumb disasm cross-check: battery + recovered bodies vs llvm-mc/objdump ==="
# Thumb artifacts (Phase-3A3) live under work/, which is never committed:
# regenerate the seed table + recovery + dispatch image if they are absent.
if [ ! -f work/phase3a/arm9_thumb.c ] || [ ! -f work/phase3a/arm9_decompile.txt ]; then
    python3 tools/mk_thumb_seeds.py work/phase3a/seeds_thumb.tsv
    $NNK recompile-all $IMG work/phase3a/arm9_thumb.c --base 0x02000000 --entry 0x02000800 --seeds work/phase3a/seeds_thumb.tsv
    SEEDS=work/phase3a/seeds_thumb.tsv
    # Phase-3A3 trace-gap seeds: close the recovery holes the boot actually
    # hits (trace PCs with no dispatch case).  Needs the extended melonDS
    # ground truth; without it the image keeps its known gaps (honest) and
    # the rest of the regression still runs.
    if [ -s "$SCRATCH/melonds_arm9_trace_10m.txt" ]; then
        python3 tools/mk_trace_seeds.py "$SCRATCH/melonds_arm9_trace_10m.txt" \
            work/phase3a/arm9_thumb.c work/phase3a/seeds_thumb.tsv work/phase3a/seeds_phase3a3.tsv
        $NNK recompile-all $IMG work/phase3a/arm9_thumb.c --base 0x02000000 --entry 0x02000800 --seeds work/phase3a/seeds_phase3a3.tsv
        SEEDS=work/phase3a/seeds_phase3a3.tsv
    fi
    $NNK decompile $IMG work/phase3a --base 0x02000000 --entry 0x02000800 --seeds $SEEDS
fi
python3 tools/thumb_disasm_check.py

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
# Phase-4J/4K: the emitter's image includes the runtime fragments by name, so
# they must sit next to the generated TU (the split build copies them itself).
cp tools/nnk_smc.inc $W/ 2>/dev/null || true
cp tools/nnk_boot.inc $W/ 2>/dev/null || true
[ -f $W/nnk_smc_map.h ] || cp out/split/c/nnk_smc_map.h $W/ 2>/dev/null || true
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
