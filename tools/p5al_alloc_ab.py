#!/usr/bin/env python3
"""Phase-5AL: A/B the guest heap allocator (0x0202A13C) between the native
runtime (NNK_DEBUG_AT_PC) and melonDS (StreamProbe).

Both logs are reduced to the ordered sequence of (caller, size) requests, which
is what decides the heap layout; the first index where they differ names the
divergence.
"""
import re
import sys

native = open(sys.argv[1]).read().splitlines()
md = open(sys.argv[2]).read().splitlines()

pat_n = re.compile(r"dbg(\d+) step=(\d+) pc=0202A13C .*?r1=([0-9A-F]{8}).*?r14=([0-9A-F]{8})")
nat = []
for line in native:
    m = pat_n.search(line)
    if m:
        nat.append((int(m.group(2)), int(m.group(3), 16), int(m.group(4), 16)))

pat_m = re.compile(r"sp hit=(\d+) pc=0x0202A13C ts=(\d+) r0=0x([0-9A-F]{8}) r1=0x([0-9A-F]{8}) r2=0x([0-9A-F]{8}) r3=0x([0-9A-F]{8}) lr=0x([0-9A-F]{8})")
mel = []
for line in md:
    m = pat_m.search(line)
    if m:
        mel.append((int(m.group(2)), int(m.group(4), 16), int(m.group(7), 16)))

print("native entries: %d   melonds entries: %d" % (len(nat), len(mel)))
n = min(len(nat), len(mel))
first = None
for i in range(n):
    if (nat[i][1], nat[i][2] & ~1) != (mel[i][1], mel[i][2] & ~1):
        first = i
        break
if first is None:
    print("no difference in the first %d allocations" % n)
    sys.exit(0)
print("FIRST DIFFERING ALLOCATION at index %d (1-based %d)" % (first, first + 1))
lo = max(0, first - 4)
for i in range(lo, min(first + 4, n)):
    mark = "  <-- DIFF" if (nat[i][1], nat[i][2] & ~1) != (mel[i][1], mel[i][2] & ~1) else ""
    print("  #%d native step=%d size=0x%X lr=0x%08X | melonds ts=%d size=0x%X lr=0x%08X%s"
          % (i + 1, nat[i][0], nat[i][1], nat[i][2], mel[i][0], mel[i][1], mel[i][2], mark))
