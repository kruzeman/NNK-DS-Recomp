# Phase-5AX — the intro from the operator's savestate: what actually happens

Stop-report (the card's second acceptance branch).  Nothing in the runtime was
changed: the measurement below shows the defect is upstream of the movie path,
so a movie fix could not have produced the banner numbers.

## 0. Headline

1. **The card's "decoder nearly idle" number does not measure the decoder.**
   `hle_hits` is incremented in `hle_swi()` (`nnktool/src/recompile.rs:3989`),
   i.e. it counts SWI-HLE calls.  8063 -> 8066 over 190M steps is three SWI
   calls, not three decode steps.  The decoder really is never driven, but that
   number is not the evidence for it.
2. **190M steps after the restore is far too early for this scene.**  Display
   frames advance every ~558k steps; the restore is at frame 913, so 190M steps
   ends at frame ~1256, while the intro in a cold boot starts at step
   2,256,256,944 = frame 4033 (`PHASE5AD.md` §5).  Frame 1000 in both the
   scripted and the window run is still the boot notice.
3. **The real failure is a guest stall, and it reproduces in BOTH paths.**
   With the A press the guest enters the SDK message-queue append at
   `0x0200ACC8..0x0200ADBC` and never leaves the tail walk at
   `0x0200AD74..0x0200AD80`; it is still there 1.9e9 steps later.  The movie is
   never reached at all: `0x0207CF18` (play a movie), `0x0207D124` (its frame
   loop) and `0x01FF8540` (the HLE decode callback) are each executed **0**
   times (`NNK_HALT_AT_PC` on each, 700M steps).
4. **The stall is a property of the operator's guest RAM, not of the restore
   machinery.**  A state saved by this build at the same step and frame
   (508,782,837 / 913, build fp 0x0768D9EB — the *same* fingerprint as the
   operator's file) differs from the operator's state in 268,412 RAM words
   (4.27 %), and a cold boot at 510M differs from the restored run at 510M in
   the same 268,413 words.  The restore is faithful; the RAM image it restores
   is a different guest trajectory.

## 1. Reproduced numbers (headless, `-O2`, 700M step limit)

```
NNK_SAVESTATE=work/repro_5ax/operator_pre_intro.nnks NNK_SAVESTATE_IGNORE_BUILD=1 \
NNK_INPUT_FILE=work/repro_5ax/press_a_at_frame920.nnk NNK_STEP_LIMIT=700000000 \
NNK_GX=headless <runner> 'Ni No Kuni.nds'
-> done steps=700000001 distinct_pcs=24154 hle_hits=8066 halt_pc=0x0200AD78
```

* top screen: mean luma 0.0 at f0950, f1000, f1200, f1250 (fully black);
  bottom: mean 202 (the fan-translation notice) -- identical at every capture;
* `movie:` lines: only the two `stream N ... source=rom:data/movie/op_u.mods /
  op_d.mods` lines the **state load** itself prints; **zero** `movie: present`
  lines (the HLE plane copy never runs);
* with no press, the same state ends in the SDK WFI idle loop
  (`halt_pc=0x0200911C`, `hle_hits=8314`) -- the press is what moves the guest
  into the failing path, so it *is* delivered.

2.4e9 steps (frames 913 -> 4291, the window where a cold boot shows `op_u`
f16/f27/f37 at corr 0.9986..0.9990): every captured top frame is still
mean 0.0 (`f.04125`, `f.04200`, `f.04275` scored 0.0000 against `op_u.yuv`).
So from this state the intro does not start at all, let alone distorted.

## 2. Where the guest stops

Trace (`NNK_TRACE_FROM=600000000`) is a four-instruction loop forever:

```
0x0200AD74  ldr r1,[r1]     ; r1 = node->next
0x0200AD78  ldr r0,[r1]     ; r0 = next->next
0x0200AD7C  cmp r0,#0
0x0200AD80  bne 0x0200AD74
```

Disassembly of the enclosing routine (`work/arm9_dec.bin`, ARM):

```
0x0200AD44  ldr r2,[r6,#0x14]      ; slot index (0..8)
0x0200AD48  ldr r1,=0x0206CA7C     ; the 9 slot list heads
0x0200AD50  ldr r5,[r1,r2,lsl #2]  ; r5 = slot[idx]  (the list to move)
0x0200AD54..0xAD60                 ; idx = (idx+1) % 9
0x0200AD64..0xAD80                 ; walk r5's chain to its LAST node
0x0200AD84  ldr r0,[r6,#0x10]      ; r0 = queue->tail
0x0200AD8C  strne r5,[r0]          ; tail->next = r5      <-- the link store
0x0200AD94  str r1,[r6,#0x10]      ; queue->tail = last node
```

`r6 = 0x0206CA58` is the SDK message queue (`PHASE4R_IPC_RHYTHM.md` §0): head
`+0x00`, sequence `+0x04`, tail `+0x10`, slot index `+0x14`, and the 9 slot
lists at `0x0206CA7C`.

Measured at the last link store (`NNK_STOREWATCH=0x0206D2D8:0x0206D474`):

```
W32 0x0206D470 old=0x00000000 pc=0x0200AD8C lr=0x0200B408
    r0=0x0206D470 r1=0x0206D470 r2=0x00000006 step=546142437
```

`r0` (queue->tail) equals `r5` (the head of the list being appended): the
append links the node to **itself**.  From step ~563M the walk therefore never
terminates, and after 620M steps the whole node pool is a closed ring:

```
0x0206D308 -> 0x0206D320 -> ... -> 0x0206D470 -> 0x0206D2D8 -> ... -> 0x0206D308
```

(18 nodes, `{next, 0x14, 0x400|0x800, tag, 0, 0}` = 0x18 bytes each, the pool
`0x0206D2D8..0x0206D488`).

## 3. Why the press reaches this code and the state matters

| run | queue at 0x0206CA58 (head / seq / tail / idx) | outcome at 700M |
|---|---|---|
| operator's state, at save (step 508,782,837) | `0206D950 / 0x3B / 0206D938 / 5` | -- |
| restored + A press | same, then seq grows to 0xA0 by step 620M | stall at 0x0200AD78 |
| restored, no press | same | WFI idle 0x0200911C |
| cold boot + same A script | `0206CF00 / 0x04 / 0206CEE8 / 4` at 510M | WFI idle 0x02009118..0x91C, distinct_pcs 21313 |
| fresh save by this build at 508,782,837 | `0206CF00 / 0x04 / ...` | -- |

The operator's save already carries **59 queued messages** (seq 0x3B) and — the
decisive detail — its queue tail is a node that is still a *pending slot list*:

```
state 0x0206CA7C (the 9 slot heads): 0206D860 0206D890 0206D8C0 0206D8F0 0206D920 ...
slot[4] = 0x0206D920 -> 0x0206D938 -> NULL          (2 nodes, tag 0x7F)
queue 0x0206CA58: head 0206D950, seq 0x3B, tail 0206D938, idx 5
```

`tail (0x0206D938) ==` the LAST node of slot[4]'s pending list.  The append path
in §2 takes `r5 = slot[4] = 0x0206D920`, walks to the last node (0x0206D938),
then does `tail->next = r5`: `0x0206D938->next = 0x0206D920`, turning the
two-node list into the 2-cycle `0x0206D920 -> 0x0206D938 -> 0x0206D920`.  That
is exactly the measured link store of §2 and the ring of §2.  The image is
internally inconsistent for its own append operation: a queue whose tail is
inside an unconsumed slot list can only be extended into a cycle.

A fresh save by this build at the same step/frame has `seq = 4` and head/tail
`0x0206CF00 / 0x0206CEE8` (a different pool).  So the operator's image comes
from a session whose message queue had **backed up to 59 pending entries** —
the append then cannot succeed, whatever the movie path does.

The restore machinery itself is not the suspect: the state container, the
movie-stream resync and the RAM/ITCM/DTCM regions load and hash clean (the
5AR/5AS round trip is unchanged), and a state this build writes at the same
step differs from the operator's by exactly the words the post-restore run then
diverges in.  The operator's image is a *different guest trajectory* — most
plausibly a session whose earlier input (window keypresses before the save)
changed the boot/queue timeline; its two restored movie streams at frame 913
(index 24) are themselves impossible in a cold boot at that frame.

## 4. The window path (operator's way) — same stall, no moving picture

`tools/p5ax_win.sh` (new, `xvfb-run` + `xdotool key a`, real SDL key event,
`NNK_GX=window`, `NNK_GX_WINDOW_PPM`):

```
done steps=2600000001 distinct_pcs=24218 hle_hits=11666 halt_pc=0x0200AD78
movie: present: 0
window capture (1024x384 at the end): left/top mean luma 247.6, right/bottom 248.8
```

Same halt PC as headless, no decode callback, no present.  So on this build the
operator's report ("moving but distorted") is **not reproducible**: the guest
stops before the movie in both paths, and the last window pixels are uniform,
not a moving distorted image.  The distortion therefore cannot be attributed
from the evidence available here; the two candidates the card lists (plane
destination from stale object fields, `0xC010` stride vs the VRAM `0x200`
stride) are only reachable **after** the decode callback runs, which never
happens from this state.

## 5. Exact remaining gap

1. **The missing state is the SDK message-queue/card-IPC bookkeeping.**  The
   restored process appends to `0x0206CA58`'s queue whose tail is inside the
   list being appended; the live session that wrote this save did not.  To fix
   this, measure the guest's queue producer/consumer pair in the operator's
   session (which event drains the queue: card transfer done, the per-frame
   `0xC0240006` heartbeat, or the 100 Hz tick) and find which of those is not
   carried by (or not re-armed after) `NNK_SAVESTATE` — i.e. restore
   *equivalence* of the IPC producer phase, not just RAM bytes.
2. Only after the guest passes `0x0207CF18` can the movie HLE be measured from
   a restored state at all; the acceptance band (>=6 frames corr >= 0.97 on
   top, bottom reported) is then a rerun of the Phase-5AD harness with
   `NNK_SAVESTATE` in front of it.
3. `work/p5t/op_d.yuv` is missing from the tree (only `op_u.yuv` is present);
   the bottom-screen score of §5.2 needs it regenerated with
   `tools/p5t_extract.py` + the local `mobiclip` decoder.

## 6. What is NOT claimed

* No fix, no class test, no `verify_phase2a` run: no runtime code changed in
  this phase (only this document and `tools/p5ax_win.sh`, the window driver).
* The card's "window is distorted / scripted is idle" dichotomy is falsified:
  both paths stall identically before the player is entered.
