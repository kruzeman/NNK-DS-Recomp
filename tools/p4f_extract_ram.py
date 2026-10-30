#!/usr/bin/env python3
"""Extract a guest-RAM window from a full main-RAM dump (NNK_DUMP_MEM output,
base 0x02000000) so it can serve as a refreshed overlay snapshot."""
import sys

src, lo, hi, dst = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
data = open(src, "rb").read()
base = 0x02000000
a, b = lo - base, hi - base
assert 0 <= a < b <= len(data), (a, b, len(data))
open(dst, "wb").write(data[a:b])
print(f"{dst}: {b - a} bytes = guest 0x{lo:08X}..0x{hi:08X} from {src}")
