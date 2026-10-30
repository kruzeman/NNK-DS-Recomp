#!/usr/bin/env python3
"""Dump the card command sequence (CARD_COMMAND writes + transfer starts and the
first data bytes read) from the timestamped melonDS IO trace."""
import sys

path = sys.argv[1]
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 10 ** 9
cmd = 0
last_cmd_pc = 0
xfer = None
n = 0
lines = []
ln = 0
for line in open(path):
    ln += 1
    if ln < lo or ln > hi:
        continue
    p = line.split()
    rw, addr, val = p[0], int(p[1], 16), int(p[2], 16)
    if addr == 0x040001A8 and rw == 'W32':
        cmd = val & 0xFF
        last_cmd_pc = 0x02016930
    elif addr == 0x040001A4 and rw == 'W32':
        xfer = (ln, cmd, val)
        n = 0
        print("ln=%d start cmd=0x%02X romcnt=0x%08X" % (ln, cmd, val))
    elif addr == 0x04100010 and rw == 'R32' and xfer is not None:
        if n < 4:
            print("   word%d 0x%08X" % (n, val))
        n += 1
        if n > 200:
            xfer = None
