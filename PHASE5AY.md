# Phase-5AY — the message pool: its consumer, its trigger, and the fix that ends the walk stall

Follow-up to Phase-5AX (`PHASE5AX.md`, commit `1c76d55`).  **This phase ships a
runtime change** in `tools/nnk_dma.inc` (the ARM7 mailbox completion is now a
handshake, not a free-running counter).  Measured result: the operator's save
**no longer stalls in the ring walk** — the pool drains, the guest leaves the
reclaim path and returns to the OS idle thread; the intro still does not start
from that save, and §6 names the barrier that remains.

## 0. Headline

1. **The object at `0x0206CA58` is a 9-ring publish/reclaim pool, not an OS
   message queue, and Phase-5AX's "internally inconsistent image" reading is
   wrong.**  The operator's image is an ordinary lockstep point (reclaim index
   == publish index == 5, in-use list empty).  Fields are in §1.
2. **Consumer: `0x0200ACC8`** — RECLAIM: waits for the ARM7's shared-mailbox
   sequence to move, moves ring `alloc_idx` onto the internal FIFO
   (`+0x00`/`+0x10`), advances `+0x14`, decrements `+0x1C`, increments the
   round sequence `+0x04`.  The FIFO pop is `0x0200B250`.  **Publisher:
   `0x0200AE88`** (intake list `+0x08`/`+0x0C` -> ring `post_idx` `+0x18`),
   fed by `0x0200AE50`; its caller is the IPC mailbox loop at `0x0200BADC`.
3. **Trigger: the ARM7's mailbox sequence `*(*(0x0206E580)) = *(0x0206CAA0)`**,
   written in this runtime by `nnk_p4s_fire()` — one completion per class-7
   card request, armed at the next 100 Hz tick slot (Phase-4S).
4. **Measured defect in the operator's state (pre-fix):** reclaim and publish
   advanced one-for-one (101/101, one pair per ~335k steps) until
   **step 546,142,360**, where a reclaim ran with no publish since
   545,475,619 and took ring 6, whose chain still ended at the FIFO tail
   (`last-of-bin == old tail == 0x0206D470`) — the 2-cycle of Phase-5AX.  Its
   publish arrived at 561,221,412 (+15.4M steps) and the next reclaim
   (561,888,612) never returned -> `halt_pc 0x0200AD78` at every budget.
5. **Fix (shipped):** the sequence is a handshake, so a completion is armed
   only when the previous one has been consumed
   (`*(0x0206CAA0) == *(0x0206CA5C)`).  With it: **no cycle and no walk
   halt** — 51 reconciles/publishes instead of 102/101, no ring re-taken, the
   run ends in the OS idle thread (`halt_pc 0x0200911C`), `distinct_pcs`
   24154 -> 24159; the cold boot still plays the intro (top-screen band
   **0.9970..0.9990**, MAE 2.7..3.0, against `work/p5t/op_u.yuv`).

## 1. The object (measured on the operator's image)

`0x0206CA58`; 9 rings at `0x0206CA7C`; 256 x 24-byte blocks at
`0x0206CD20..0x0206E520`:

| off | meaning | operator's image | this build's own save at the same step |
|---|---|---|---|
| +0x00 | FIFO head | `0x0206D950` | `0x0206CF00` |
| +0x04 | round sequence | `0x3B` | `0x04` |
| +0x08 | intake head | `0` | `0` |
| +0x0C | intake tail | `0` | `0` |
| +0x10 | FIFO tail | `0x0206D938` | `0x0206CEE8` |
| +0x14 | reclaim index | `5` | `4` |
| +0x18 | publish index | `5` | `4` |
| +0x1C | in-flight counter | `0` | `0` |
| +0x20 | publish counter | `0x3C` | `0x04` |

rings: `0x0206D860, D890, D8C0, D8F0, D920, D7A0, D7D0, D800, D830`.
Phase-5AX read `+0x10` as "queue tail inside a pending slot list"; it is the
FIFO tail, and a ring keeps its content after a reclaim (the guest refills it
on the next publish) — which is exactly why a reclaim must never run twice
between publishes.

## 2. The four routines (disassembly cross-checked against the running RAM —
`0x0200AD00..0x0200B100` and `0x0200BA60..0x0200BB00` are byte-identical to
`work/arm9_dec.bin` at step 25M)

```
0x0200B250  pop_FIFO()      head=*(+0x00); *(+0x00)=head->next; if empty *(+0x10)=0
0x0200AE50  intake_add(n)   append n to (+0x08,+0x0C)
0x0200AE88  PUBLISH(flags)  if intake empty -> return 1
                            ring[*(+0x18)] = intake head; +0x18 = (+0x18+1)%9
                            +0x08 = +0x0C = 0; +0x1C++; +0x20++
0x0200ACC8  RECLAIM(flags)  wait while *(+0x04) == *(*(0x0206E580))
                            r5 = ring[*(+0x14)]; +0x14 = (+0x14+1)%9
                            walk r5 to its last node; if (+0x10) *(+0x10)=r5
                            else *(+0x00)=r5; (+0x10)=last; +0x1C--; +0x04++
```
RECLAIM callers: `0x0203BE10`, `0x02056624`, `0x02056698`, `0x020567FC`,
`0x0203C878/0x0203C9BC/0x0203CA88/0x0203D100` (driver loops).  PUBLISH caller:
the IPC mailbox loop (`lr = 0x0200BADC` at every publish store), i.e. a
publish follows an IPC delivery.

## 3. Measured pre-fix rhythm in the operator's state (headless)

`NNK_STOREWATCH=0x0206CA58:0x0206CAA4`, `NNK_SAVESTATE=work/repro_5ax/
operator_pre_intro.nnks`, `NNK_INPUT_FILE=work/repro_5ax/press_a_at_frame920.nnk`:

```
   512,307,024  PUBLISH bin=5
   512,639,681  RECLAIM idx=5  old tail=0x0206D938  last-of-bin=0x0206D968
   512,644,559  PUBLISH bin=6
   ...          one pair per ~335,0xx steps (the 100 Hz tick), 101 pairs
   545,475,619  PUBLISH bin=5
   545,807,333  RECLAIM idx=5  last-of-bin=0x0206D470
   546,142,360  RECLAIM idx=6  old tail=0x0206D470  last-of-bin=0x0206D470  <-- cycle
   561,221,412  PUBLISH bin=6  (15.4M steps late)
   561,888,612  RECLAIM idx=7  -> walks the closed ring forever
```
101 publishes, 102 reclaims, 102 host sequence bumps -> one surplus ack, one
surplus reclaim (the first request arms TWO completions: `t0+164,505` and the
next tick slot, Phase-4S).  Cold boot, same script, 700M: the same rhythm with
no surplus — reclaims at 18,724,632 (idx 0..3), 524,954,170 (4), 525,289,167
(5), 573,532,845 (6), each preceded by its publish, ending in the idle loop.

## 4. The fix (`tools/nnk_dma.inc`, Phase-5AY block)

`nnk_p4s_request()` returns without arming when the ARM7's word is still
unconsumed:

```c
p = ld32(0x0206E580); obj = p ? p : 0x0206CAA0;
if (ld32(obj) != ld32(0x0206CA5C)) return;   /* an ack is already in flight */
```
No new file-scope statics (deliberately: the NNKS `io` region must keep its
68,648-byte size so the operator's state still loads — two probe statics grew
it by 8 B and the loader refused the file, measured).  Diagnostics are behind
`NNK_P4S_TRACE`.

## 5. Result with the fix

| measurement | pre-fix | with fix |
|---|---|---|
| operator's state, 700M | `halt_pc 0x0200AD78` (ring walk), distinct_pcs 24154 | **`halt_pc 0x0200911C` (OS idle), distinct_pcs 24159**, hle_hits 8313 |
| operator's state, 2.6e9 | still in the walk (5AX) | idle; `movie: present` **0** |
| pool storewatch (700M) | 101 publishes / 102 reclaims, ring re-taken at 546,142,360 | **51 publishes / 51 reclaims, no duplicate last-of-bin, no cycle** |
| card stream | 184 `B7` blocks `0x0FB65600..0x0FB6F800`, then two `B8` | identical (same addresses, +-few steps) |
| cold boot 700M | 21313 pcs, idle | 21313 pcs, idle (`0x02009118` -> `0x02009120` = a different WFI slot) |
| cold boot intro (2.6e9) | (Phase-5AD band) | op_u/op_d open at step 710,392,621; **top-screen corr 0.9970..0.9990**, MAE 2.7..3.0 on f1300..f1700 |

## 6. The barrier that remains (named)

With the stall gone the guest finishes its read burst and **stops**: a PC trace
of steps 545.5M..547M is only the OS idle thread

```
0x0200712C  push {r3,lr}; bl 0x02008D0C; bl 0x02009118 (WFI); b 0x02007134
0x02009118/0x0200911C/0x02009120   (281,568 hits each in 1.5M steps)
```
and stays there for the remaining 2e9 steps: `0x0207CF18` (play a movie),
`0x0207D124` and `0x01FF8540` (the HLE decode callback) are still executed
**0** times, and the pool keeps draining normally (no cycle).  So the next gap
is **not** the message pool: the guest's main thread has no runnable work after
the A press's 94 KB card read completes — some thread that the read completion
should wake (the driver's waiter, i.e. the same class Phase-4U fixed for the
card stream with a timer IRQ) is never woken.  Naming that waiter is the next
bounded step: dump the SDK thread list at 560M in the restored run and compare
it with the cold run at the same step (the walk needs the OS thread
`state`/`next` offsets from the ARM9 image, not the ones Phase-4U guessed).

## 7. Regressions

| check | result |
|---|---|
| `cargo test --release` | **57 passed** |
| prelude audit | `AUDIT OK` (52 required, 2 divergent, 3 obsolete) at every build |
| `SKIP_MELONDS=1 tools/verify_phase2a.sh` | `rc=0`, `RESULT: PASS` (decoder cross-check); melonDS-vs-native **1000/1000** strict common prefix, 1000/1000 window-filtered, 1000/1000 in-order |
| savestate round trip 5M -> 6M | `fnv=0xE013EFB9 crc=0x214965FF` on both save and reload (identical to Phase-5AT) |
| savestate round trip 30M -> 32M | `fnv=0xF72DDE40 crc=0xB029FEA7` on both save and reload (round trip equal; the value differs from 5AT because the ARM7 ack timing after 18.5M is now handshaken) |
| operator's state still loads | yes (`state: loaded ... build=0x0768D9EB`, io region unchanged at 68,648 B) |
| ROM-only runtime, clean dir | 20M steps: `distinct_pcs 13328`, `41 frames presented` (same as 5AT) |
| speed | 700M steps in **64.1 s** (5AT 107.7 s; card band "79 s +-10%" met) |
| notice screen | `f.01100.top/bot.ppm` non-blank in the cold run; 32 of 108 cold captures non-blank |
| movie band (cold, top) | **0.9970..0.9990** vs `work/p5t/op_u.yuv` (op_d.yuv is still absent from the tree, so the bottom row is not scored) |

## 8. What is NOT claimed

* The intro does **not** start from the operator's save: the walk stall is
  fixed and the queue drains, but the guest idles (§6).
* The pre-fix surplus is attributed to the measured 102-acks/101-publishes
  count and the Phase-4S double-arm on the first request; the exact step at
  which the surplus was created was not isolated (the ack word is written by
  the host, so the storewatch `pc` at those writes is the guest PC of the
  moment, not a store instruction).
* `work/p5t/op_d.yuv` is still missing, so the bottom screen is unscored.
