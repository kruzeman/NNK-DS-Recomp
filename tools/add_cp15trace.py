#!/usr/bin/env python3
"""Insert an env-gated CP15 trace into melonDS/src/CP15.cpp (atomic, idempotent).

Logs every CP15Write (id, value, ARM9 PC) and every read of the TCM-relevant
CP15 registers to MELONDS_CP15TRACE_PATH so the Ni no Kuni boot's TCM
configuration (ITCM/DTCM enable + size/base) can be modelled exactly.

Format:
  W id=0xXXX val=0xXXXXXXXX pc=0xXXXXXXXX
  R id=0xXXX val=0xXXXXXXXX pc=0xXXXXXXXX
"""
import sys

path = '/home/hermes/tools/melonDS/src/CP15.cpp'
s = open(path).read()

MARK = '// CP15 trace (Ni no Kuni Phase-4B)'
if MARK in s:
    print('CP15 trace already present; nothing to do')
    sys.exit(0)

# 1) stdlib include (stdlib.h for getenv/atol)
old_inc = '#include <stdio.h>\n#include <string.h>\n'
assert s.count(old_inc) == 1, 'include anchor: %d' % s.count(old_inc)
s = s.replace(old_inc, '#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n', 1)

# 2) tracer namespace before CP15Write
anchor = 'void ARMv5::CP15Write(u32 id, u32 val)'
assert s.count(anchor) == 1, 'CP15Write anchor'
ns = '''// CP15 trace (Ni no Kuni Phase-4B): env-gated, inactive unless
// MELONDS_CP15TRACE_PATH is set.  Logs every CP15 write and every read of
// the TCM-relevant registers (c1 control 0x100, DTCM base 0x910, ITCM size
// 0x911) so the guest's TCM configuration can be reproduced exactly.
namespace CP15Trace
{
static FILE* FP = nullptr;
static bool Tried = false;
static void Log(char op, u32 id, u32 val, u32 pc) noexcept
{
    if (!Tried)
    {
        Tried = true;
        const char* p = getenv("MELONDS_CP15TRACE_PATH");
        if (p && p[0])
        {
            FP = fopen(p, "w");
            if (FP) setvbuf(FP, nullptr, _IOLBF, 0);
        }
    }
    if (!FP) return;
    fprintf(FP, "%c id=0x%03X val=0x%08X pc=0x%08X\\n", op, id, val, pc);
}
}

'''
s = s.replace(anchor, ns + anchor, 1)

# 3) log the write (right at the commented printf)
old_w = '''void ARMv5::CP15Write(u32 id, u32 val)
{
    //if(id!=0x704)printf("CP15 write op %03X %08X %08X\\n", id, val, R[15]);
'''
new_w = '''void ARMv5::CP15Write(u32 id, u32 val)
{
    CP15Trace::Log('W', id, val, R[15] - ((CPSR & 0x20) ? 4 : 8));
    //if(id!=0x704)printf("CP15 write op %03X %08X %08X\\n", id, val, R[15]);
'''
assert s.count(old_w) == 1, 'CP15Write body anchor'
s = s.replace(old_w, new_w, 1)

# 4) log the TCM-relevant reads (insert at the start of the matching cases
#    in the read switch)
old_r = '''    case 0x910:
        return DTCMSetting;
    case 0x911:
        return ITCMSetting;
'''
new_r = '''    case 0x910:
        CP15Trace::Log('R', id, DTCMSetting, R[15] - ((CPSR & 0x20) ? 4 : 8));
        return DTCMSetting;
    case 0x911:
        CP15Trace::Log('R', id, ITCMSetting, R[15] - ((CPSR & 0x20) ? 4 : 8));
        return ITCMSetting;
'''
assert s.count(old_r) == 1, 'CP15 read 0x910/0x911 anchor'
s = s.replace(old_r, new_r, 1)

# control-register read: find `case 0x100:` in the read switch (returns CP15Control)
if '    case 0x100:\n        return CP15Control;' in s:
    s = s.replace('    case 0x100:\n        return CP15Control;',
                  "    case 0x100:\n        CP15Trace::Log('R', id, CP15Control, R[15] - ((CPSR & 0x20) ? 4 : 8));\n        return CP15Control;", 1)
else:
    sys.stderr.write('note: control read case not in expected shape (skipped)\\n')

open(path, 'w').write(s)
print('CP15.cpp: include + CP15Trace ns + W/R log sites OK')
