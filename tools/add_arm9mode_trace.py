#!/usr/bin/env python3
"""Add a per-PC ARM/Thumb mode log to melonDS's ARM9 PC trace (atomic, idempotent).

    python3 tools/add_arm9mode_trace.py

Why: the Phase-4A PC trace prints only "0x%08x", so a runtime-loaded overlay
region can be split into ARM and Thumb code only by alignment: a PC at
2 (mod 4) is provably Thumb, but a 4-aligned PC is ambiguous.  Recompiling a
Thumb body as ARM (or vice versa) produces a wrong dispatch table that looks
like a "new barrier" instead of a translation bug.

The hook is env-gated and independent of the PC trace, so every existing
trace parser keeps working:

  MELONDS_ARM9MODE_PATH=<file>   one "0x%08x <thumb>\n" line per executed
                                 in-window instruction (thumb = 0 ARM, 1 Thumb),
                                 same window/limit as MELONDS_ARM9TRACE_*.

Rebuild build-ptrace/ afterwards, then:

  MELONDS_ARM9TRACE_PATH=pc.txt MELONDS_ARM9MODE_PATH=mode.txt MELONDS_ARM9TRACE_N=30000000 \
    xvfb-run -a build-ptrace/melonDS "<ROM>"
"""
import sys

PATH = '/home/hermes/tools/melonDS/src/ARM.cpp'
s = open(PATH).read()

if 'MELONDS_ARM9MODE_PATH' in s:
    print('already present')
    sys.exit(0)


def sub(old, new, what):
    global s
    if s.count(old) != 1:
        sys.exit('%s: anchor count %d' % (what, s.count(old)))
    s = s.replace(old, new, 1)


# (1) state: a second file handle in the ARM9Trace namespace
sub("static long Count = 0;\nstatic bool ExitAtEnd = true;\n",
    "static long Count = 0;\nstatic bool ExitAtEnd = true;\n"
    "static FILE* MFP = nullptr;   /* MELONDS_ARM9MODE_PATH: \"0xADDR thumb\" */\n",
    'state')

# (2) open it next to the PC trace
sub('    setvbuf(FP, nullptr, _IOLBF, 0);\n'
    '    fprintf(FP, "# ARM9 trace window 0x%08X..0x%08X limit %ld\\n", Lo, Hi, Limit);\n',
    '    setvbuf(FP, nullptr, _IOLBF, 0);\n'
    '    fprintf(FP, "# ARM9 trace window 0x%08X..0x%08X limit %ld\\n", Lo, Hi, Limit);\n'
    '    if ((s = getenv("MELONDS_ARM9MODE_PATH")) && s[0])\n'
    '    {\n'
    '        MFP = fopen(s, "w");\n'
    '        if (MFP) { setvbuf(MFP, nullptr, _IOLBF, 0);\n'
    '                   fprintf(MFP, "# ARM9 mode trace window 0x%08X..0x%08X limit %ld\\n", Lo, Hi, Limit); }\n'
    '    }\n',
    'open')

# (3) one mode line per executed PC
sub('    fprintf(FP, "0x%08x\\n", addr);\n'
    '    bool swi = thumb ? ((instr & 0xFF00) == 0xDF00)\n',
    '    fprintf(FP, "0x%08x\\n", addr);\n'
    '    if (MFP) fprintf(MFP, "0x%08x %d\\n", addr, thumb ? 1 : 0);\n'
    '    bool swi = thumb ? ((instr & 0xFF00) == 0xDF00)\n',
    'pc-line')

# (4) close it when the limit is reached (ARM9Trace block: it is the one that
#     follows the SWI annotation and ends with the ARM9TracePCCount bridge)
sub('        fflush(FP);\n        fclose(FP);\n        FP = nullptr;\n'
    '        if (ExitAtEnd)\n        {\n            fflush(stderr);\n            exit(0);\n        }\n    }\n}\n}\n\n'
    '/* ARM9 PC-trace index at this moment',
    '        fflush(FP);\n        fclose(FP);\n        FP = nullptr;\n'
    '        if (MFP) { fflush(MFP); fclose(MFP); MFP = nullptr; }\n'
    '        if (ExitAtEnd)\n        {\n            fflush(stderr);\n            exit(0);\n        }\n    }\n}\n}\n\n'
    '/* ARM9 PC-trace index at this moment',
    'close')

open(PATH, 'w').write(s)
print('patched %s (MELONDS_ARM9MODE_PATH)' % PATH)
