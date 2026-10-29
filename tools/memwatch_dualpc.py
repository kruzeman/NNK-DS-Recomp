#!/usr/bin/env python3
"""One-shot edit: MemWatch::Log gains a9pc/pc7 dual-PC logging (6 call sites)."""
import sys

path = '/home/hermes/tools/melonDS/src/NDS.cpp'
s = open(path).read()

old_log = '''static void Log(const char* cpu, int bits, u32 addr, u32 val, u32 pc) noexcept
{
    if (!FP) return;
    if (Count >= Limit) return;
    fprintf(FP, "W%d cpu=%s 0x%08X 0x%08X pc=0x%08X\\n", bits, cpu, addr, val, pc);
    if (++Count >= Limit) { fflush(FP); fclose(FP); FP = nullptr; }
}'''
new_log = '''static void Log(const char* cpu, int bits, u32 addr, u32 val, u32 pc7, u32 pc9) noexcept
{
    if (!FP) return;
    if (Count >= Limit) return;
    fprintf(FP, "W%d cpu=%s 0x%08X 0x%08X pc7=0x%08X pc9=0x%08X\\n",
            bits, cpu, addr, val, pc7, pc9);
    if (++Count >= Limit) { fflush(FP); fclose(FP); FP = nullptr; }
}'''
assert s.count(old_log) == 1, "log fn"
s = s.replace(old_log, new_log)

pc7 = 'ARM7.R[15] - ((ARM7.CPSR & 0x20) ? 4 : 8)'
pc9 = 'ARM9.R[15] - ((ARM9.CPSR & 0x20) ? 4 : 8)'
for cpu, bits in (('9', 8), ('9', 16), ('9', 32), ('7', 8), ('7', 16), ('7', 32)):
    old = f'MemWatch::Log("{cpu}", {bits}, addr, val, {pc9 if cpu == "9" else pc7});'
    new = f'MemWatch::Log("{cpu}", {bits}, addr, val, {pc7}, {pc9});'
    assert s.count(old) == 1, f"site {cpu}/{bits}: {s.count(old)}"
    s = s.replace(old, new)

open(path, 'w').write(s)
print("OK: log fn + 6 sites updated")
