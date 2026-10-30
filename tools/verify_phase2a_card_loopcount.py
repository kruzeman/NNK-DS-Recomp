#!/usr/bin/env python3
"""Count the card poll loop in both traces around the Phase-4D divergence."""
import sys

def scan(path, lo, hi):
    idx = -1
    last = None
    counts = []
    write_idx = None
    n_poll = 0
    first_data = None
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        idx += 1
        if idx < lo or idx > hi:
            continue
        if line == '0x02016930':
            write_idx = idx
            n_poll = 0
            first_data = None
        elif line == '0x02016b30':
            if write_idx is not None:
                n_poll += 1
        elif line == '0x02016b3c' and write_idx is not None and first_data is None:
            first_data = idx
            counts.append((write_idx, n_poll, first_data))
            last = counts[-1]
    return counts

m = scan(sys.argv[1], int(sys.argv[3]), int(sys.argv[4]))
n = scan(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
print("melonDS (write_idx, polls@first data, first data idx):", m)
print("native  (write_idx, polls@first data, first data idx):", n)
