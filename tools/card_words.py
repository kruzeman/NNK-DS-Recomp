#!/usr/bin/env python3
"""Per-word poll counts for one menu of melonDS card transfers.

For the transfer starting at trace line <start_ln>, walk forward and print, for
each ROMDATA read, how many ROMCNT polls happened since the previous ROMDATA
read (i.e. the length of each DRQ-clear wait).
"""
import sys

path = sys.argv[1]
start_ln = int(sys.argv[2])
words_limit = int(sys.argv[3]) if len(sys.argv) > 3 else 200

polls = 0
w = 0
started = False
hist = {}
with open(path) as f:
    for ln, line in enumerate(f, 1):
        if ln < start_ln:
            continue
        p = line.split()
        rw, addr = p[0], int(p[1], 16)
        if addr == 0x040001A4 and rw.startswith('R'):
            polls += 1
        elif addr == 0x04100010 and rw.startswith('R'):
            if started:
                print("word %3d: %4d polls since previous word" % (w, polls))
            else:
                print("word   0: %4d polls before first DRQ" % polls)
                started = True
            hist[polls] = hist.get(polls, 0) + 1
            polls = 0
            w += 1
            if w >= words_limit:
                break
print("histogram:", sorted(hist.items()))
