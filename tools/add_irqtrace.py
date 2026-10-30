#!/usr/bin/env python3
"""Insert an env-gated IRQ trace into melonDS (ARM.cpp + NDS.cpp), atomic + idempotent.

Ground truth for Ni no Kuni Phase-4C (ARM9 IRQ delivery):

  MELONDS_IRQTRACE_PATH   output file (unset -> disabled)
  MELONDS_IRQTRACE_N      events to log (default 100000)

Log lines (append-mode semantics: one file, line buffered):
  setirq bit=<n> a9idx=<Count> ie=<hex> if=<hex> ime=<0/1> pc9=<hex>
      -- every NDS::SetIRQ(0, irq) (ARM9 side): the exact instruction index
         (ARM9 trace count) at which an IF source went high, and the
         IME/IE/IF state at that moment.
  trigger a9idx=<Count> pc9=<hex> oldcpsr=<hex> newlr=<hex> sp_irq=<hex>
      lr_irq=<hex> spsr=<hex> sp_cur=<hex>
      -- every ARM::TriggerIRQ() on ARM9: the exception-entry moment, the
         interrupted CPSR, the banked IRQ sp/lr BEFORE the switch (R_IRQ[0/1]
         are the bank values), and the SPSR/return lr after UpdateMode.
         pc9 = R[15]-8/-4 = the instruction that would have executed next.
"""
import sys

MODE = 'IRQ trace (Ni no Kuni Phase-4C)'

def patch(path, edits):
    s = open(path).read()
    if MODE in s:
        print('%s already patched; nothing to do' % path)
        return
    for old, new, what in edits:
        assert s.count(old) == 1, '%s: anchor %r count=%d' % (path, what, s.count(old))
        s = s.replace(old, new, 1)
    open(path, 'w').write(s)
    print('patched %s' % path)


ARM = '/home/hermes/tools/melonDS/src/ARM.cpp'
NDS = '/home/hermes/tools/melonDS/src/NDS.cpp'

# ---- ARM.cpp: TriggerIRQ hook ---------------------------------------------
patch(ARM, [
    ('#include <stdio.h>\n',
     '#include <stdio.h>\n#include <stdarg.h>\n',
     'stdarg include'),
    ('void ARM::TriggerIRQ()\n{\n    if (CPSR & 0x80)\n        return;\n\n    u32 oldcpsr = CPSR;\n',
     '''void ARM::TriggerIRQ()
{
    if (CPSR & 0x80)
        return;
    u32 oldcpsr = CPSR;
    if (Num == 0 && IRQTrace::Active())
    {
        // pc9 = next instruction that would have executed (same convention as
        // the PC hook); R_IRQ[0/1] are the BANKED IRQ sp/lr before UpdateMode.
        IRQTrace::Log("trigger a9idx=%ld pc9=0x%08X oldcpsr=0x%08X sp_irq=0x%08X lr_irq=0x%08X sp_cur=0x%08X\\n",
                      ARM9Trace::Count, R[15] - ((CPSR & 0x20) ? 4 : 8), oldcpsr,
                      R_IRQ[0], R_IRQ[1], R[13]);
    }
''',
     'TriggerIRQ'),
    ('namespace ARM9Trace\n{',
     '''// IRQ trace (Ni no Kuni Phase-4C): env-gated, inactive unless
// MELONDS_IRQTRACE_PATH is set.  Declared here (ARM.cpp owns the ARM9 trace
// counter); NDS.cpp logs SetIRQ through IRQTrace::LogSetIRQ.
namespace IRQTrace
{
static FILE* FP = nullptr;
static bool Tried = false;
static long Limit = 100000;
static long Count = 0;
static void Init() noexcept
{
    Tried = true;
    const char* p = getenv("MELONDS_IRQTRACE_PATH");
    if (!p || !p[0]) return;
    FP = fopen(p, "w");
    if (!FP) return;
    setvbuf(FP, nullptr, _IOLBF, 0);
    const char* s;
    if ((s = getenv("MELONDS_IRQTRACE_N"))) Limit = atol(s);
}
static inline bool Active() noexcept { if (!Tried) Init(); return FP != nullptr; }
static inline void Log(const char* fmt, ...) noexcept
{
    if (!Active()) return;
    if (Count >= Limit) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(FP, fmt, ap);
    va_end(ap);
    Count++;
}
}

''',
     'ARM9Trace ns'),
])

# ---- NDS.cpp: SetIRQ hook --------------------------------------------------
# The a9idx helper is declared where the memwatch uses it; reuse the same
# declaration the Phase-4A watch already added.
s = open(NDS).read()
if MODE in s:
    print('%s already patched; nothing to do' % NDS)
else:
    # declaration: piggyback on the existing ARM9TracePCCount declaration
    decl_anchor = 'long ARM9TracePCCount() noexcept;'
    if decl_anchor in s:
        s = s.replace(decl_anchor,
                      decl_anchor + '\nnamespace IRQTrace {\n'
                      'void LogSetIRQ(int bit, long a9idx, unsigned ie, unsigned ifl, int ime, unsigned pc9) noexcept;\n'
                      '}', 1)
    else:
        # fall back: declare both externs right before NDS::SetIRQ
        assert s.count('void NDS::SetIRQ(u32 cpu, u32 irq)') == 1
        s = s.replace('void NDS::SetIRQ(u32 cpu, u32 irq)',
                      'long ARM9TracePCCount() noexcept;\n'
                      'namespace IRQTrace {\n'
                      'void LogSetIRQ(int bit, long a9idx, unsigned ie, unsigned ifl, int ime, unsigned pc9) noexcept;\n'
                      '}\n\nvoid NDS::SetIRQ(u32 cpu, u32 irq)', 1)
    old = '''void NDS::SetIRQ(u32 cpu, u32 irq)
{
    IF[cpu] |= (1 << irq);
    UpdateIRQ(cpu);
'''
    assert s.count(old) == 1, 'SetIRQ body anchor: %d' % s.count(old)
    new = '''void NDS::SetIRQ(u32 cpu, u32 irq)
{
    IF[cpu] |= (1 << irq);
    UpdateIRQ(cpu);
    if (cpu == 0)
        IRQTrace::LogSetIRQ((int)irq, ARM9TracePCCount(), IE[0], IF[0], IME[0] & 1,
                             ARM9.R[15] - ((ARM9.CPSR & 0x20) ? 4 : 8));
'''
    s = s.replace(old, new, 1)
    # LogSetIRQ definition: append near the end of the file
    s += '''
// ---- IRQ trace (Ni no Kuni Phase-4C): env-gated SetIRQ logger -------------
// Definition lives here because NDS.cpp owns IE/IF/IME; the file handle and
// the ARM9 trace counter live in ARM.cpp (IRQTrace::Log + ARM9Trace::Count).
namespace IRQTrace {
void LogSetIRQ(int bit, long a9idx, unsigned ie, unsigned ifl, int ime, unsigned pc9) noexcept;
void LogSetIRQ(int bit, long a9idx, unsigned ie, unsigned ifl, int ime, unsigned pc9) noexcept
{
    extern "C" void IRQTraceSetIRQLine(long a9idx, int bit, unsigned ie, unsigned ifl,
                                       int ime, unsigned pc9);
    IRQTraceSetIRQLine(a9idx, bit, ie, ifl, ime, pc9);
}
}
'''
    open(NDS, 'w').write(s)
    print('patched %s' % NDS)

# ---- ARM.cpp: provide IRQTraceSetIRQLine ----------------------------------
s = open(ARM).read()
if 'IRQTraceSetIRQLine' not in s:
    anchor = 'namespace ARM9Trace\n{'
    assert s.count(anchor) == 1
    s = s.replace(anchor, '''extern "C" void IRQTraceSetIRQLine(long a9idx, int bit, unsigned ie,
                                  unsigned ifl, int ime, unsigned pc9) noexcept
{
    IRQTrace::Log("setirq bit=%d a9idx=%ld ie=0x%08X if=0x%08X ime=%d pc9=0x%08X\\n",
                  bit, a9idx, ie, ifl, ime, pc9);
}

''' + anchor, 1)
    open(ARM, 'w').write(s)
    print('added IRQTraceSetIRQLine to ARM.cpp')

print('done')
