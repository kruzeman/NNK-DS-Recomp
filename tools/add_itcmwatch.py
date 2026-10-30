#!/usr/bin/env python3
"""Insert the ITCM write watch into melonDS CP15.cpp (atomic, all sites)."""
path = '/home/hermes/tools/melonDS/src/CP15.cpp'
s = open(path).read()

# 1) stdlib include
old = '#include <stdio.h>\n#include <string.h>\n'
assert s.count(old) == 1, 'include'
s = s.replace(old, '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n')

# 2) ItcmWatch namespace before ARMv5::DataRead8
anchor = 'void ARMv5::DataRead8(u32 addr, u32* val)'
assert s.count(anchor) == 1, 'anchor'
ns = '''// ---------------------------------------------------------------------------
// ITCM write watch (Ni no Kuni DS Phase-4A ground truth).  ITCM stores are
// intercepted here (addr < ITCMSize) and never reach NDS.cpp, so this hook
// captures who fills the ITCM with code.  Env-gated, inactive unless
// MELONDS_ITCMWATCH_PATH is set.  Format: W<bits> 0x<addr> 0x<val> pc=0x<PC>
//   MELONDS_ITCMWATCH_N  writes to log (default 300000)
// ---------------------------------------------------------------------------
namespace ItcmWatch
{
static FILE* FP = nullptr;
static bool Tried = false;
static long Limit = 300000;
static long Count = 0;
static inline void Log(u32 addr, int bits, u32 val, u32 pc) noexcept
{
    if (!Tried)
    {
        Tried = true;
        const char* p = getenv("MELONDS_ITCMWATCH_PATH");
        if (p && p[0])
        {
            FP = fopen(p, "w");
            const char* s;
            if (FP && (s = getenv("MELONDS_ITCMWATCH_N"))) Limit = atol(s);
            if (Limit < 1) Limit = 1;
            if (FP) setvbuf(FP, nullptr, _IOLBF, 0);
        }
    }
    if (!FP || Count >= Limit) return;
    fprintf(FP, "W%d 0x%08X 0x%08X pc=0x%08X\\n", bits, addr, val, pc);
    if (++Count >= Limit) { fflush(FP); fclose(FP); FP = nullptr; }
}
}

'''
s = s.replace(anchor, ns + anchor, 1)

# 3) log calls in the four ITCM write branches
pc = 'R[15] - ((CPSR & 0x20) ? 4 : 8)'
reps = [
    ('        DataCycles = 1;\n        *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;',
     '        DataCycles = 1;\n        ItcmWatch::Log(addr, 8, val, %s);\n        *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;' % pc, 1),
    ('        DataCycles = 1;\n        *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;',
     '        DataCycles = 1;\n        ItcmWatch::Log(addr, 16, val, %s);\n        *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;' % pc, 1),
    ('        DataCycles = 1;\n        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;',
     '        DataCycles = 1;\n        ItcmWatch::Log(addr, 32, val, %s);\n        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;' % pc, 1),
    ('        DataCycles += 1;\n        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;',
     '        DataCycles += 1;\n        ItcmWatch::Log(addr, 32, val, %s);\n        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;' % pc, 1),
]
for old_r, new_r, expect in reps:
    assert s.count(old_r) == expect, (old_r[:50], s.count(old_r))
    s = s.replace(old_r, new_r, 1)

open(path, 'w').write(s)
print('CP15.cpp: include + ItcmWatch ns + 4 log sites OK')
