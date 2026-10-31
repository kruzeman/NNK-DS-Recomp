#!/usr/bin/env python3
"""Phase-5G: add an ADDRESS WINDOW to the melonDS ARM9 IO bus trace.

Why: the existing IO trace (tools/melonds-io-trace.patch + add_iotrace_ts.py)
logs every ARM9 IO bus access, which is the right tool for measuring card-bus
timing but useless for "who writes this one register": the boot polls
DISPSTAT/VCOUNT hundreds of thousands of times, so an unfiltered capture
either hits MELONDS_IOTRACE_N long before the interesting window or writes
gigabytes of noise.  Phase-5G needs the complete IE (0x04000210) write history
-- including the RMW done by the SDK helpers at 0x020060B4 / 0x020060E4 -- to
find the exact guest instruction that programs ARM9 IE bit 19.

  MELONDS_IOTRACE_LO=0x04000210 MELONDS_IOTRACE_HI=0x04000210
  -> only accesses in [LO, HI] are logged (the N budget counts logged lines).

Env knobs, output format and the ts= unit are unchanged from
tools/melonds-io-trace.patch; the filter is applied before the line budget, so
N now bounds the *matching* accesses.

The hook lives in the ARM9IOTrace namespace of the patched melonDS tree
(/home/hermes/tools/melonDS, uncommitted verification hooks).  This script is
the record of the change and is idempotent.

usage: python3 tools/add_iotrace_window.py [path/to/melonDS/src/NDS.cpp]
then:  make -j -C build-ptrace melonDS   and re-capture with
       MELONDS_IOTRACE_PATH=... MELONDS_IOTRACE_LO=... MELONDS_IOTRACE_HI=...
"""
import sys

P = sys.argv[1] if len(sys.argv) > 1 else '/home/hermes/tools/melonDS/src/NDS.cpp'
s = open(P).read()

MARK = 'Phase-5G: optional ADDRESS window'
if MARK in s:
    print('%s already patched; nothing to do' % P)
    sys.exit(0)

OLD_VARS = """static long Count = 0;

static inline bool Active() noexcept"""
NEW_VARS = """static long Count = 0;
/* Phase-5G: optional ADDRESS window so the trace can target a single IO
 * register (e.g. IE 0x04000210) instead of drowning in DISPSTAT polls.
 * MELONDS_IOTRACE_LO / MELONDS_IOTRACE_HI, same hex parsing as the memwatch. */
static u32 AddrLo = 0;
static u32 AddrHi = 0xFFFFFFFFu;

static inline bool Active() noexcept"""

OLD_INIT = """            if (Limit < 1) Limit = 1;
            if (FP) setvbuf(FP, nullptr, _IOLBF, 0);"""
NEW_INIT = """            if (Limit < 1) Limit = 1;
            if (FP && (s = getenv("MELONDS_IOTRACE_LO"))) AddrLo = (u32)strtoul(s, nullptr, 0);
            if (FP && (s = getenv("MELONDS_IOTRACE_HI"))) AddrHi = (u32)strtoul(s, nullptr, 0);
            if (FP) setvbuf(FP, nullptr, _IOLBF, 0);"""

OLD_LOG = """    if (!Active()) return;
    if (Count >= Limit) return;"""
NEW_LOG = """    if (!Active()) return;
    if (addr < AddrLo || addr > AddrHi) return;
    if (Count >= Limit) return;"""

for old, new, what in ((OLD_VARS, NEW_VARS, 'namespace vars'),
                       (OLD_INIT, NEW_INIT, 'env init'),
                       (OLD_LOG, NEW_LOG, 'Log filter')):
    assert s.count(old) == 1, '%s: anchor %r count=%d' % (P, what, s.count(old))
    s = s.replace(old, new, 1)

open(P, 'w').write(s)
print('patched %s' % P)
