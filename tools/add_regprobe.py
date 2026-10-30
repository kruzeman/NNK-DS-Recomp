#!/usr/bin/env python3
"""Insert an env-gated per-PC register probe into melonDS (ARM.cpp), atomic +
idempotent.

Ground truth for Ni no Kuni Phase-4C register divergences: the ARM9 PC trace
proves WHICH instruction diverged; this probe dumps the full register file (and
the banked r13/r14 of SVC/IRQ, which the PC trace cannot show) at the exact
instructions around a suspected divergence.

  MELONDS_REGPROBE_PATH  output file (unset -> disabled)
  MELONDS_REGPROBE_PCS   comma-separated PC list, e.g. "0x0200723C,0x0200729C"
  MELONDS_REGPROBE_N     hit budget per run (default 200)

Log line (one per hit, before the instruction executes; R[15] convention is the
same as the PC trace: pc = R[15]-4 (Thumb) / R[15]-8 (ARM)):

  reg pc=0x<..> cpsr=0x<..> r0=.. .. r14=.. svc_sp=0x.. svc_lr=0x.. irq_sp=0x.. irq_lr=0x..
"""
import sys

MODE = 'RegProbe (Ni no Kuni Phase-4C)'
ARM = '/home/hermes/tools/melonDS/src/ARM.cpp'

s = open(ARM).read()
if MODE in s:
    print('%s already patched; nothing to do' % ARM)
    sys.exit(0)

anchor = 'namespace ARM9Trace\n{'
assert s.count(anchor) == 1, 'ARM9Trace namespace anchor: %d' % s.count(anchor)

probe = '''// RegProbe (Ni no Kuni Phase-4C): env-gated register dump at selected PCs.
// MELONDS_REGPROBE_PATH names the output file; MELONDS_REGPROBE_PCS is a
// comma-separated PC list.  Inactive unless both are set.
namespace RegProbe
{
static FILE* FP = nullptr;
static bool Tried = false;
static int  NPc = 0;
static u32  Pcs[32];
static long Limit = 200;
static long Count = 0;

static void Init() noexcept
{
    Tried = true;
    const char* p = getenv("MELONDS_REGPROBE_PATH");
    if (!p || !p[0]) return;
    const char* l = getenv("MELONDS_REGPROBE_PCS");
    if (!l || !l[0]) return;
    FP = fopen(p, "w");
    if (!FP) return;
    setvbuf(FP, nullptr, _IOLBF, 0);
    const char* q = l;
    while (*q && NPc < 32)
    {
        while (*q == ' ' || *q == ',') q++;
        if (!*q) break;
        Pcs[NPc++] = (u32)strtoul(q, (char**)&q, 0);
    }
    const char* n;
    if ((n = getenv("MELONDS_REGPROBE_N"))) Limit = atol(n);
    fprintf(FP, "# regprobe %d pcs\\n", NPc);
}

static void Log(u32* R, u32 cpsr, u32* svc, u32* irq, u32 pc) noexcept
{
    if (!Tried) Init();
    if (!FP) return;
    if (Count >= Limit) return;
    bool want = false;
    for (int i = 0; i < NPc; i++) if (Pcs[i] == pc) { want = true; break; }
    if (!want) return;
    fprintf(FP, "reg pc=0x%08X cpsr=0x%08X", pc, cpsr);
    for (int i = 0; i < 15; i++) fprintf(FP, " r%d=0x%08X", i, R[i]);
    fprintf(FP, " svc_sp=0x%08X svc_lr=0x%08X svc_spsr=0x%08X irq_sp=0x%08X irq_lr=0x%08X irq_spsr=0x%08X\\n",
            svc[0], svc[1], svc[2], irq[0], irq[1], irq[2]);
    Count++;
}

}  // namespace RegProbe

'''

s = s.replace(anchor, probe + anchor, 1)

# ---- hook: Thumb instruction dispatch -------------------------------------
t_old = '                ARM9Trace::PC(R[15] - 4, CurInstr, true);\n'
assert s.count(t_old) == 1, 'thumb hook anchor: %d' % s.count(t_old)
s = s.replace(t_old, t_old + '                RegProbe::Log(R, CPSR, R_SVC, R_IRQ, R[15] - 4);\n', 1)

a_old = '                ARM9Trace::PC(R[15] - 8, CurInstr, false);\n'
assert s.count(a_old) == 1, 'arm hook anchor: %d' % s.count(a_old)
s = s.replace(a_old, a_old + '                RegProbe::Log(R, CPSR, R_SVC, R_IRQ, R[15] - 8);\n', 1)

open(ARM, 'w').write(s)
print('patched %s' % ARM)
