#!/bin/sh
# Phase-1A acceptance verification (Ni no Kuni DS ARM9).
# Rebuilds nnktool, re-runs every acceptance check, prints one summary per check.
# Runs against the checkout this script lives in (Phase-4L), not a fixed path;
# a worktree reaches the main checkout's work/ tree through the `work` symlink.
set -e
cd "$(dirname "$0")/.."
NNK=nnktool/target/release/nnktool

echo "=== build ==="
cargo build --release --manifest-path nnktool/Cargo.toml 2>&1 | tail -1

echo
echo "=== A. nnktool disasm vs arm-none-eabi-objdump (4 sampled ranges) ==="
run_range() {
    start=$1; end=$2; mode=$3; name=$4
    if [ "$mode" = thumb ]; then opt="--disassembler-options=force-thumb"; else opt=""; fi
    $NNK disasm work/ghidra_src/arm9.bin --base 0x02000000 --start "$start" --end "$end" --mode "$mode" > "work/v_$name.txt" 2>/dev/null
    arm-none-eabi-objdump -D -b binary -m arm $opt --adjust-vma=0x02000000 --start-address="$start" --stop-address="$end" work/ghidra_src/arm9.bin > "work/vo_$name.txt"
    echo "-- $name ($start..$end, $mode)"
    python3 tools/cmp_objdump.py "work/v_$name.txt" "work/vo_$name.txt" | sed -n '1,4p'
}
run_range 0x02000800 0x02000B00 arm entry
run_range 0x0200B904 0x0200BA04 arm fn
run_range 0x0200C8C0 0x0200C9C0 arm fn2
run_range 0x02060C00 0x02060D00 thumb thumb

echo
echo "=== B. function recovery from the entry (+ Thumb-rooted run) ==="
$NNK decompile work/ghidra_src/arm9.bin work/phase1a --base 0x02000000 --entry 0x02000800 | sed -n '1,2p'
$NNK decompile work/ghidra_src/arm9.bin work/phase1a/thumb --base 0x02000000 --entry 0x02060C00 --entry-mode thumb | sed -n '1p'

echo
echo "=== C. Ghidra cross-check ==="
python3 tools/crosscheck_ghidra.py work/phase1a/arm9_functions.tsv work/ghidra_arm9_functions.tsv | sed -n '1,13p'

echo
echo "=== D. recompile-entry -> C -> cc -> run -> first-64 trace check ==="
$NNK recompile-entry work/ghidra_src/arm9.bin work/phase1a/arm9_entry.c --base 0x02000000 --entry 0x02000800 | sed -n '1p'
cc -O2 -o work/phase1a/arm9_entry_recompiled work/phase1a/arm9_entry.c
cc -O2 -DNNK_TRACE_EXEC -o work/phase1a/arm9_entry_recompiled_exec work/phase1a/arm9_entry.c
work/phase1a/arm9_entry_recompiled work/phase1a/arm9_entry.image.bin > work/phase1a/entry_trace.txt 2> work/phase1a/entry_stderr.txt
work/phase1a/arm9_entry_recompiled_exec work/phase1a/arm9_entry.image.bin > work/phase1a/entry_trace_exec.txt 2>/dev/null
python3 tools/check_entry_trace.py work/phase1a/entry_trace.txt work/entry_disasm_check.txt 64 | tail -2
echo "stderr shims: $(grep -c call-shim work/phase1a/entry_stderr.txt)"
tail -1 work/phase1a/entry_trace.txt
echo "exec-trace lines: $(grep -c '^0x' work/phase1a/entry_trace_exec.txt)"
