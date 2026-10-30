#!/usr/bin/env python3
"""Phase-4D: add the ARM9 master timestamp to the melonDS IO bus trace.

Why: the Phase-3A IO trace (`tools/melonds-io-trace.patch`) logs the address,
value and PC of every ARM9 bus access but not *when* it happened, so the card
transfer timing (cmddelay / xfercycle / DRQ latency) could only be inferred
from PC counts.  Adding `ts=` (= `NDS::ARM9Timestamp >> ARM9ClockShift`, the
same unit `NDS::ScheduleEvent` takes) makes the timing directly measurable:
one line per access, `R32|W32 ... pc=0x.. ts=<master cycle>`.

The hook lives in the ARM9IOTrace namespace of the patched melonDS tree
(/home/hermes/tools/melonDS, uncommitted verification hooks).  This script is
the record of the change and is idempotent.

usage: python3 tools/add_iotrace_ts.py [path/to/melonDS/src/NDS.cpp]
then:  rebuild build-ptrace/ (make -j melonDS) and re-capture with
       MELONDS_IOTRACE_PATH=... MELONDS_IOTRACE_N=400000
"""
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/home/hermes/tools/melonDS/src/NDS.cpp'
s = open(P).read()

SIG_OLD = "static void Log(const char* rw, u32 addr, u32 val, u32 pc) noexcept"
SIG_NEW = "static void Log(const char* rw, u32 addr, u32 val, u32 pc, u64 ts) noexcept"
FMT_OLD = 'fprintf(FP, "%s 0x%08X 0x%08X pc=0x%08X\\n", rw, addr, val, pc);'
FMT_NEW = ('fprintf(FP, "%s 0x%08X 0x%08X pc=0x%08X ts=%llu\\n", rw, addr, val, pc,\n'
           '            (unsigned long long)ts);')
CALL_OLD = "((ARM9.CPSR & 0x20) ? 4 : 8));"
CALL_NEW = "((ARM9.CPSR & 0x20) ? 4 : 8), (u64)(ARM9Timestamp >> ARM9ClockShift));"

if SIG_NEW in s:
    print("already patched")
    sys.exit(0)

assert SIG_OLD in s, "ARM9IOTrace::Log signature not found"
assert FMT_OLD in s, "ARM9IOTrace::Log fprintf not found"
s = s.replace(SIG_OLD, SIG_NEW, 1)
s = s.replace(FMT_OLD, FMT_NEW, 1)

n = 0
out = []
for line in s.split('\n'):
    # only the six ARM9IOTrace::Log call sites inside NDS::ARM9Read*/Write*
    if 'ARM9IOTrace::Log(' in line and line.rstrip().endswith(CALL_OLD):
        line = line.replace(CALL_OLD, CALL_NEW)
        n += 1
    out.append(line)
s = '\n'.join(out)
assert n == 6, "expected 6 ARM9IOTrace::Log call sites, patched %d" % n
open(P, 'w').write(s)
print("patched %d call sites: %s" % (n, P))
