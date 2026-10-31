#!/usr/bin/env python3
"""Phase-4Z: ts-gated ARM9 PC trace in melonDS (the Phase-4Y named probe).

Why: PHASE4Y.md names the follow-up barrier as "capture a melonDS ARM9
pc-trace around ts 50.48M..54.3M to see which wait blocks the module-manager
thread".  The stock ARM9 PC hook (`tools/melonds-arm9-ptrace.patch`) logs the
first N executed PCs inside an address window with no time information, so it
cannot be aimed at a *time* window.  This adds:

  MELONDS_ARM9TRACE_TSLO / _TSHI   master-cycle window
                                   (= NDS::ARM9Timestamp >> ARM9ClockShift,
                                    the same unit the IO trace's ts= uses)
  * lines are only logged for TSLO <= ts < TSHI
  * in ts-gated mode every line becomes "0xADDR ts=<cycles>" so the PC trace
    aligns directly against p4q_mel_io.txt
  * when ts reaches TSHI the file is closed and the emulator exits (bounded
    run); MELONDS_ARM9TRACE_EXIT=0 keeps it running instead.

The hook lives in the ARM9Trace namespace of the patched melonDS tree
(/home/hermes/tools/melonDS, uncommitted verification hooks).  This script is
the record of the change and is idempotent.

usage: python3 tools/add_arm9trace_ts.py [path/to/melonDS/src/ARM.cpp]
then:  rebuild build-ptrace/ (make -j melonDS) and capture with
       MELONDS_ARM9TRACE_PATH=... MELONDS_ARM9TRACE_TSLO=... _TSHI=...
"""
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/home/hermes/tools/melonDS/src/ARM.cpp'
s = open(P).read()

if 'ARM9TRACE_TSLO' in s:
    print("already patched")
    sys.exit(0)

OLD_DECL = """static u32 Lo = 0x02000000;
static u32 Hi = 0x02400000;

static void Init() noexcept
{"""
NEW_DECL = """static u32 Lo = 0x02000000;
static u32 Hi = 0x02400000;
/* Phase-4Z: master-cycle (ts) gate, same unit as the IO trace's ts= field. */
static bool TSGate = false;
static u64 TSLo = 0;
static u64 TSHi = ~0ULL;

static void Init() noexcept
{"""

OLD_ENV = """    if ((s = getenv("MELONDS_ARM9TRACE_EXIT"))) ExitAtEnd = atoi(s) != 0;
    if (Limit < 1) Limit = 1;
    setvbuf(FP, nullptr, _IOLBF, 0);
    fprintf(FP, "# ARM9 trace window 0x%08X..0x%08X limit %ld\\n", Lo, Hi, Limit);"""
NEW_ENV = """    if ((s = getenv("MELONDS_ARM9TRACE_EXIT"))) ExitAtEnd = atoi(s) != 0;
    if ((s = getenv("MELONDS_ARM9TRACE_TSLO"))) { TSLo = strtoull(s, nullptr, 0); TSGate = true; }
    if ((s = getenv("MELONDS_ARM9TRACE_TSHI"))) { TSHi = strtoull(s, nullptr, 0); TSGate = true; }
    if (Limit < 1) Limit = 1;
    setvbuf(FP, nullptr, _IOLBF, 0);
    fprintf(FP, "# ARM9 trace window 0x%08X..0x%08X limit %ld ts %llu..%llu\\n",
            Lo, Hi, Limit, (unsigned long long)TSLo, (unsigned long long)TSHi);"""

OLD_PC = """static void PC(u32 addr, u32 instr, bool thumb) noexcept
{
    if (!Tried) Init();
    if (!FP) return;
    if (addr < Lo || addr >= Hi) return;
    if (Count >= Limit) return;

    fprintf(FP, "0x%08x\\n", addr);
    if (MFP) fprintf(MFP, "0x%08x %d\\n", addr, thumb ? 1 : 0);"""

NEW_PC = """static void PC(u32 addr, u32 instr, bool thumb, u64 ts) noexcept
{
    if (!Tried) Init();
    if (!FP) return;
    if (TSGate && ts < TSLo) return;
    if (TSGate && ts >= TSHi)
    {
        /* window done: close the trace and stop the emulator (bounded run) */
        fflush(FP); fclose(FP); FP = nullptr;
        if (MFP) { fflush(MFP); fclose(MFP); MFP = nullptr; }
        if (ExitAtEnd) { fflush(stderr); exit(0); }
        return;
    }
    if (addr < Lo || addr >= Hi) return;
    if (Count >= Limit) return;

    if (TSGate) fprintf(FP, "0x%08x ts=%llu\\n", addr, (unsigned long long)ts);
    else        fprintf(FP, "0x%08x\\n", addr);
    if (MFP) fprintf(MFP, "0x%08x %d\\n", addr, thumb ? 1 : 0);"""

CALL_OLD = ["ARM9Trace::PC(R[15] - 4, CurInstr, true);",
            "ARM9Trace::PC(R[15] - 8, CurInstr, false);"]
CALL_NEW = ["ARM9Trace::PC(R[15] - 4, CurInstr, true, (u64)(NDS.ARM9Timestamp >> NDS.ARM9ClockShift));",
            "ARM9Trace::PC(R[15] - 8, CurInstr, false, (u64)(NDS.ARM9Timestamp >> NDS.ARM9ClockShift));"]

for old, new, what in ((OLD_DECL, NEW_DECL, 'declarations'),
                       (OLD_ENV, NEW_ENV, 'env knobs'),
                       (OLD_PC, NEW_PC, 'PC()')):
    assert old in s, "anchor not found: " + what
    s = s.replace(old, new, 1)

for old, new in zip(CALL_OLD, CALL_NEW):
    assert s.count(old) == 1, "expected exactly one call site: " + old
    s = s.replace(old, new, 1)

open(P, 'w').write(s)
print("patched ARM9Trace with the ts gate: %s" % P)
