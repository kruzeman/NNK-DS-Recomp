#!/usr/bin/env python3
"""Faithful melonDS card model + guest poll loop, one event check per instruction.

Card model transcribed from melonDS 1.1 src/NDSCart.cpp (see p4d_sim2.py).
Guest loop instruction costs measured from the melonDS timestamp capture:
  poll iteration (DRQ clear) 9 cycles: ldr ROMCNT(1) tst(1) beq-taken(3) tst(1) bne-taken(3)
  set poll -> ROMDATA read 6:          ldr(1) tst(1) beq-not-taken(3) ldr ROMDATA(1)
  ROMDATA read -> next poll 9:         cmp(1) strcc(3) addcc(1) tst(1) bne-taken(3)
Expected (measured) shape for ROMCNT=0xA1416657: 909 clear polls before word 0,
then clear-poll counts 1,1,0 repeating, 128 words, 1123 ROMCNT polls total.
"""
XFER, GAP2, LEN = 5, 1, 0x200
COST = {'ldr': 1, 'ldrd': 1, 'alu': 1, 'str': 3, 'br': 3}

cmddelay = 8 + 0x657 + GAP2
t = 0
pos = 0
count = 0
poscart = 0
poscpu = 0
late = False
drq = False
nxt = XFER * (cmddelay + 4)
polls = 0
clear_polls = 0
per_word = []


def fire():
    global nxt, count, pos, poscart, late, drq
    while nxt is not None and t >= nxt:
        nxt = None
        if pos >= LEN:
            return
        count += 1
        poscart ^= 1
        pos += 4
        drq = True
        if count < 2:
            d = 4 + (GAP2 if not (pos & 0x1FF) else 0)
            nxt = t + XFER * d
        else:
            late = True


def step(what):
    global t
    t += COST[what]
    fire()


while len(per_word) < 128:
    step('ldr')                      # ldr r2,[0x040001A4]
    polls += 1
    if not drq:
        clear_polls += 1
        step('alu')                  # tst
        step('br')                   # beq taken -> 0x02016B4C
        step('alu')                  # tst
        step('br')                   # bne taken -> poll
        continue
    per_word.append(clear_polls)
    clear_polls = 0
    step('alu')                      # tst
    step('br')                       # beq not taken
    step('ldrd')                     # ldr r1,[0x04100010]  (read happens now)
    poscpu ^= 1
    if count > 0:
        count -= 1
    drq = False
    if pos < LEN:
        if late:
            late = False
            d = 4 + (GAP2 if not (pos & 0x1FF) else 0)
            nxt = t + XFER * d
    step('alu')                      # cmp r3,#128
    step('str')                      # strcc
    step('alu')                      # addcc
    step('alu')                      # tst r2,#0x80000000
    step('br')                       # bne -> poll

print("words=%d polls=%d first=%d pattern=%s hist=%s"
      % (len(per_word), polls, per_word[0], per_word[1:13],
         sorted({k: per_word.count(k) for k in set(per_word)}.items())))
