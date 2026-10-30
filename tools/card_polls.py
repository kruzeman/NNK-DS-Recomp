#!/usr/bin/env python3
"""Analyse the melonDS card-bus capture: per-transfer poll counts and delays.

Reads an ARM9 IO bus trace (MELONDS_IOTRACE_PATH format) and reports, for each
ROMCNT write that starts a transfer, how many ROMCNT polls the guest makes
before the DRQ bit (bit23) first reads set, and what the ROMCNT value is.
"""
import sys

path = sys.argv[1]
lo = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 0) if len(sys.argv) > 3 else 10**18

polls = 0
drq_polls = 0
xfer_start = None
words = 0
transfers = []
with open(path) as f:
    for ln, line in enumerate(f, 1):
        if ln < lo or ln > hi:
            continue
        p = line.split()
        rw, addr, val = p[0], int(p[1], 16), int(p[2], 16)
        if addr == 0x040001A4 and rw == 'W32':
            if xfer_start is not None:
                transfers.append((xfer_start[0], xfer_start[1], polls, drq_polls, words))
            xfer_start = (ln, val)
            polls = drq_polls = words = 0
        elif addr == 0x040001A4 and rw.startswith('R'):
            if xfer_start is not None:
                polls += 1
                if val & 0x800000:
                    drq_polls += 1
        elif addr == 0x04100010 and rw.startswith('R'):
            if xfer_start is not None:
                words += 1
if xfer_start is not None:
    transfers.append((xfer_start[0], xfer_start[1], polls, drq_polls, words))

print("transfers:", len(transfers))
print("ln_start  ROMCNT_write      polls  drq_polls  words   delay13 gap2 b27 size")
for ln, val, polls, drqp, words in transfers:
    delay = val & 0x1FFF
    gap2 = (val >> 16) & 0x3F
    print(" %-8d 0x%08X  %6d %6d  %6d   %5d %4d %3d %4d"
          % (ln, val, polls, drqp, words, delay, gap2, (val >> 27) & 1, (val >> 24) & 7))
