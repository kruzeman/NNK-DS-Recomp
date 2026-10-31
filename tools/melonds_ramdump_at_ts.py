#!/usr/bin/env python3
"""Add MELONDS_RAMDUMP_PATH to melonDS's ARM9 PC trace (atomic, idempotent).

    python3 tools/melonds_ramdump_at_ts.py && (cd /home/hermes/tools/melonDS/build-ptrace && make -j12 melonDS)

Purpose: emulator-vs-emulator MEMORY comparison.  The existing probes can log
writes to a watm window (MELONDS_MEMWATCH_*) and the PCs melonDS executes
(MELONDS_ARM9TRACE_*), but not the VALUE a word holds when no write ever lands
in the window.  This adds exactly that: the first ARM9 PC logged inside the
trace window dumps melonDS's 4 MiB main RAM to MELONDS_RAMDUMP_PATH
(one-shot, so a bounded trace window = a bounded snapshot).

Combine with the existing ts gate, e.g. RAM at trace-ts 27,000,000:

    MELONDS_ARM9TRACE_PATH=/tmp/pc.txt MELONDS_ARM9TRACE_TSLO=27000000 \
    MELONDS_ARM9TRACE_TSHI=27020000 MELONDS_ARM9TRACE_EXIT=1 \
    MELONDS_RAMDUMP_PATH=/tmp/ram_27m.bin \
      xvfb-run -a build-ptrace/melonDS "<ROM>"

The dump is raw main RAM (MainRAMMask+1 bytes, 4 MiB on a DS), so byte/word
compares against the native gmem dump are direct (`gmem` is 0x02000000-based;
main-RAM offset 0x3FFFA8 is guest 0x02FFFFA8).
"""
import sys

NDS_CPP = '/home/hermes/tools/melonDS/src/NDS.cpp'
ARM_CPP = '/home/hermes/tools/melonDS/src/ARM.cpp'

NDS_ANCHOR = 'thread_local NDS* NDS::Current = nullptr;\n'
NDS_ADD = r'''
/* Ni no Kuni DS Phase-5B: one-shot main-RAM dump, driven from the ARM9 PC
 * trace (ARM.cpp).  MELONDS_RAMDUMP_PATH names the output file.  Exists so a
 * word that no write-watch ever sees (e.g. an untouched but read flag at the
 * top of main RAM) can still be compared against the native run's gmem. */
extern "C" void NNK_RAMDump(const char* path) noexcept
{
    NDS* n = NDS::Current;
    if (!n || !path || !path[0]) return;
    FILE* f = fopen(path, "wb");
    if (!f) return;
    size_t len = (size_t)n->MainRAMMask + 1u;
    fwrite(n->MainRAM, 1, len, f);
    fclose(f);
    fprintf(stderr, "NNK_RAMDump: %zu bytes of main RAM -> %s\n", len, path);
}
'''

ARM_ANCHOR = '''    if (TSGate) fprintf(FP, "0x%08x ts=%llu\\n", addr, (unsigned long long)ts);
    else        fprintf(FP, "0x%08x\\n", addr);
'''
ARM_ADD = '''    if (!RAMDumpDone)
    {
        const char* rp = getenv("MELONDS_RAMDUMP_PATH");
        if (rp && rp[0]) { RAMDumpDone = true; NNK_RAMDump(rp); }
    }
'''

ARM_DECL_ANCHOR = 'static bool TSGate = false;\n'
ARM_DECL_ADD = '''static bool TSGate = false;
/* Phase-5B: one-shot main-RAM dump at the first PC inside the trace window. */
static bool RAMDumpDone = false;
extern "C" void NNK_RAMDump(const char* path) noexcept;
'''


def main():
    s = open(NDS_CPP).read()
    if 'NNK_RAMDump' in s:
        print('NDS.cpp: already patched')
    else:
        assert s.count(NDS_ANCHOR) == 1, 'NDS.cpp anchor count %d' % s.count(NDS_ANCHOR)
        s = s.replace(NDS_ANCHOR, NDS_ANCHOR + NDS_ADD, 1)
        open(NDS_CPP, 'w').write(s)
        print('NDS.cpp: NNK_RAMDump added')

    s = open(ARM_CPP).read()
    if 'MELONDS_RAMDUMP_PATH' in s:
        print('ARM.cpp: already patched')
        return
    assert s.count(ARM_DECL_ANCHOR) == 1, 'ARM.cpp decl anchor count %d' % s.count(ARM_DECL_ANCHOR)
    s = s.replace(ARM_DECL_ANCHOR, ARM_DECL_ADD, 1)
    assert s.count(ARM_ANCHOR) == 1, 'ARM.cpp trace anchor count %d' % s.count(ARM_ANCHOR)
    s = s.replace(ARM_ANCHOR, ARM_ANCHOR + ARM_ADD, 1)
    open(ARM_CPP, 'w').write(s)
    print('ARM.cpp: MELONDS_RAMDUMP_PATH hook added')


if __name__ == '__main__':
    main()
