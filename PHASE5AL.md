# Phase-5AL — melonDS enters 0x02044868 and takes the NORMAL path; the first difference is allocation #412

Follows Phase-5AK (`bad5a1e`, merged into this worktree as `e61bafc`).  5AK named
the field: `[stream+0x124]` is uninitialised memory that inherits the movie audio
driver's 8-bit-silence fill (`memset(right[ch], 0x80, 0x6000)`), and our guest
self-terminates on it at step 334,058,027.  The open question was the reference:
does melonDS reach that guard at all, and with what arguments?

**Answered, measured, and branch (c) of the card is REFUTED: melonDS reaches
0x02044868 many times in the same scripted session and takes the normal path —
its stream object's `[obj+0x124]` is 0, ours is 0x80808080.**  The shutdown is
therefore *not* real game behaviour.  The divergence is located exactly: the
guest heap allocator answers identically for the first **411** allocations
(same size, same call site, both emulators) and diverges at **#412**, where our
runtime runs an ov17 allocation loop (0x020E1B6B, payload 0x1A8) that melonDS
never executes.  The remaining gap is the branch between #411 and that loop —
named below with the exact method to close it.  No runtime behaviour was faked;
no game bytes are committed.

## 1. The instrument (new, env-gated, in `tools/melonds-p5al-probe.patch`)

* **`StreamProbe`** (`src/ARM.cpp`): entry probe for a comma list of ARM9 PCs
  (`MELONDS_STREAMPROBE_PC`), logging ts + r0..r12/sp/lr, a dynamically
  addressed field (`MELONDS_STREAMPROBE_DEREF="0:0x124,0:0x9C"` — the guard's
  own `ldr r0,[r5,#0x124]`), absolute watched words (`_WATCH`), and an
  env-gated object hexdump.  All probe reads go through a new
  `ARMv5::ProbeBusRead32/16/8` helper so a probe can never trip the protection
  unit: my first attempt used `DataRead32`, which for the guard's object
  (r5=0 at the entry snapshot) faulted on page 0x124 and hijacked the CPU into
  `0xFFFF0010` — the "data abort (02044870)" in the first run was the probe,
  not the game.
* **`ShotDump`** (`src/NDS.cpp`): headless PPM dump of the two composited
  screens from `GPU.GetFramebuffers` (`MELONDS_SHOT_DIR/_FROM/_TO/_EVERY`).
* **Input**: no new code — the existing `MELONDS_GXLOG_PRESS` schedule, fed by
  `tools/nnk_to_melonds.py tools/nnk_operator_session.nnk 1`, i.e. the
  operator's session transcribed 1:1 (the `.nnk` masks are already melonDS's
  active-low key masks; +1 frame for the GXLog unit counter).
* Run: `xvfb-run -a build/melonDS "<ROM>"`, `MELONDS_GXLOG_FRAMES=2600`,
  interpreter (JIT is off in the operator's `melonDS.toml`).

## 2. The measurement

### 2.1 melonDS does enter the guard, and returns normally

```
# streamprobe pc=0x02044868 deref=3 watch=2
sp hit=8  pc=0x02044868 ts=342947764 cpsr=0x2000001F r0=0x022CF74C r1=0x00000000
          ... r4=0x022CF74C r5=0x00000000 sp=0x027E3A58 lr=0x020DA880
  deref r0+0x124 -> 0x022CF870 w=0x00000000 h=0x0000
  deref r0+0x9C  -> 0x022CF7E8 w=0x0A0B0000 h=0x0000
  deref r0+0x0   -> 0x022CF74C w=0x0213D114        <- vptr
sp hit=12 pc=0x02044868 ts=343350336 cpsr=0x2000001F r0=0x022D26B8 r1=0x00000000
          ... r3=0x020D9C80 r5=0x02278658 r12=0x00000008 sp=0x027E3A60 lr=0x020D9CEC
  deref r0+0x124 -> 0x022D27DC w=0x00000000 h=0x0000   <- beq 0x020448A4
  deref r0+0x9C  -> 0x022D2754 w=0x05050000 h=0x0000
  deref r0+0x0   -> 0x022D26B8 w=0x0213B70C h=0xB70C   <- the stream vptr
sp hit=13 pc=0x02044868 ts=343351655 ... r0=0x022D2870 lr=0x020DA03C   (same, normal)
sp hit=15..85 pc=0x02044868 ts=348352690 .. 1136882135  r0=0x022DDED0/0x02342BB4
  r1=0/2/3, [+0x124] = 0 every time, [+0x9C] = 0x0000/0x8080/0x3846/0x8080
```

85 guard entries were logged before the per-PC cap; **every one** had
`[obj+0x124] == 0`.  The caller is the same vtable dispatch as ours —
ov17 `0x020D9CB8`: `ldr r1,[r4,#0x15c]; tst [r4+0x12c],#1; ldrh r1,[r4+0x130];
ldr r2,[vptr+0x50]; blx r2` — i.e. slot **+0x50** of `0x0213B70C`, the ROM's own
code and the ROM's own object class (vptr `0x0213B70C`).

Side-by-side with our runtime (5AI/5AK data):

| | melonDS | native |
|---|---|---|
| guard entry | ts **343,350,336** | step **334,058,027** |
| object | `0x022D26B8` | `0x022CC8CC` |
| vptr | `0x0213B70C` | `0x0213B70C` |
| `[obj+0x9C]` | `0x0505` | (state 0x80 at the abort) |
| `[obj+0x124]` | **0x00000000** | **0x80808080** |
| guard decision | `beq 0x020448A4` → **normal** | `ld16(0x80808080)=0 ≤ 0x80` → `bl 0x020090D8` |

melonDS then runs on: the last guard entries are at ts 1,136,882,135 (~frame
1230 of 2600), i.e. the guest is alive and interactive long after the point our
runtime parks.  Screenshot evidence (`shots/p5al/`): frame 340 top = the
Level-5 logo, frame 1200 **bottom = the game's own "Skip the cutscene?
Yes / No" confirmation dialog with "No" highlighted** — see §4, it matters.

Also measured: the movie audio fills are **2 players × 6 channels = 12**
`memset(…,0x80,0x6000)` calls at `0x02284810 + k*0x12020` (ts 327,198,571 …
327,662,243), and the loop bound at `0x0207AB64` is `r0 = 6` (`r9 = 6` at the
`cmp r9,#6` guard).  The five fills 5AK saw were its probe's hit cap
(`dbg78..dbg82`), not a five-channel player: the allocator A/B of §3 proves the
two heaps are identical through those buffers.

## 3. The heap A/B — first differing allocation

Instrumented the guest allocator core, reached from both `bl 0x020462FC`
(default-heap wrapper, `ldr r0,[0x020774B4+4]; mov r1,size; blx 0x0202A13C`)
and the Thumb wrapper `blx 0x020462DC`:

* native: `NNK_DEBUG_AT_PC=0x0202A13C` → 476 entries up to the halt at
  334.2 M steps;
* melonDS: `MELONDS_STREAMPROBE_PC=0x0202A13C` → 972 entries up to frame 2600.

`tools/p5al_alloc_ab.py` reduces both to the ordered `(size, call site)`
sequence (call sites compared with the Thumb bit masked — the wrapper is entered
with `lr&1` set from Thumb callers):

```
native entries: 476   melonds entries: 972
FIRST DIFFERING ALLOCATION at index 412
  #408 native step=332347082 size=0x3AD64 lr=0x020D25C7 | melonds ts=337053150 size=0x3AD64 lr=0x020D25C7
  #409 native step=332805411 size=0x4     lr=0x020F6B4F | melonds ts=337715489 size=0x4     lr=0x020F6B4F
  #410 native step=332876816 size=0x126FC lr=0x020DDA20 | melonds ts=338238136 size=0x126FC lr=0x020DDA20
  #411 native step=333870729 size=0x3D8   lr=0x020DDA20 | melonds ts=339990062 size=0x3D8   lr=0x020DDA20
  #412 native step=333970271 size=0x1A8   lr=0x020E1B6B | melonds ts=340115333 size=0x54    lr=0x020FB934
  #413 native step=333971341 size=0x1A8   lr=0x020E1B6B | melonds ts=340121063 size=0x228   lr=0x020463D8
  #414 native step=333972697 size=0x1A8   lr=0x020E1B6B | melonds ts=340137301 size=0x81AE  lr=0x020463D8
  #415 native step=333974053 size=0x1A8   lr=0x020E1B6B | melonds ts=340923159 size=0xC030  lr=0x020463D8
```

411 identical requests, then our runtime starts a **0x1A8-byte allocation loop**
in overlay 17 that melonDS does not run (65 iterations before the halt).  That
loop is what puts the post-movie stream object on top of the audio buffer: the
0x1A8 payload + the 0x10 allocator header is exactly the 0x1B8 object stride of
5AK, and the object we abort on (`0x022CC8CC`) is the third of those blocks.

The loop and its gate (ov17, Thumb):

```
020E1B14  ldr  r0,[sp,#4] ; ...             ; r5 = 0x06006000 (VRAM scratch)
020E1B1C  movs r0,#0x25 ; lsls r0,r0,#4     ; 0x250
020E1B20  str  r6,[r5,r0]                   ; [0x06006250] = r6   (loop count)
020E1B24  str  r1,[r5,r0+16]                ; [0x06006260] = r1
020E1B38  ldr  r0,[r5,r0]                   ; read it back
020E1B3A  movs r4,#0
020E1B3C  cmp  r0,#0
020E1B3E  ble.n 0x020E1BBE                  ; <= 0 -> skip the whole loop
020E1B54  movs r0,#0x25 ; lsls r0,#4 ; subs r0,#0xA8   ; 0x1A8 payload
020E1B6A  blx  0x020462DC                   ; -> guest heap
```

Native at `0x020E1B38`, step 333,970,241: `r6=0x0000449F`,
`[0x06006250]=9F 44 00 00` → the gate passes and the loop allocates.
**melonDS never executes `0x020E1B38` at all** (zero hits over 2600 frames ≈
2.4 e9 ARM9 cycles): the divergence is a *control-flow* difference upstream of
that instruction, not a different value at the gate.

## 4. The new barrier, named

Two measured facts bracket the remaining gap:

1. **Flow**: after the last identical allocation (#411, native step
   333,870,729 ↔ melonDS ts 339,990,062) our runtime takes a branch that leads
   to ov17 `0x020E1B14..0x020E1B6C`; melonDS goes to `0x020FB934`.  The first
   differing ARM9 PC is in that window — capture it with
   `MELONDS_ARM9TRACE_PATH` (window `MELONDS_ARM9TRACE_TSLO/TSHI` around
   ts 339–340 M) versus `NNK_TRACE` around steps 333.87–334.0 M and diff with
   `tools/cmp_pc_traces.py`.  That is the single remaining unknown.
2. **Input**: with the operator's press schedule, melonDS's own session ends at
   the guest's **"Skip the cutscene? Yes / No"** dialog (bottom screen, "No"
   selected — `shots/p5al/md_f1200_bot.png`), while our runtime consumes the
   A|Start edge directly in the movie player (`0x0207D230..0x0207D248`, 5AD)
   and exits the movie.  So the reference behaviour for "the skip tap" is a
   *confirmation dialog*, and our runtime's movie exit — the state from which
   the poisoned stream objects are then created — is itself a divergence from
   the reference.  This is the most likely root of (1) and the place to start.

Nothing was faked to move the halt: the guard, the stream objects and the
allocator are untouched guest code, and the runtime's shipped behaviour is
unchanged in this phase.

## 5. Evidence and tools

```
work/phase5al/probe1.txt       guard + movie-loop entries (first run, probe faulting)
work/phase5al/probe2.txt       85 guard entries with [obj+0x124] (the §2.1 table)
work/phase5al/probe3.txt       allocator + ctor + memset + guard (the A/B source)
work/phase5al/probe4.txt       the audio-fill loop bound (r0=6, r9=6)
work/phase5al/probe5.txt       the gate PC 0x020E1B38/0x020E1B3E/0x020E1B6C (0 hits)
work/phase5al/native_alloc.log native allocator entries (476)
work/phase5al/native_gate.log|.txt  native gate hit + [0x06006250] = 0x449F
shots/p5al/md_f0340_top.png    melonDS frame 340 (Level-5 logo, alive)
shots/p5al/md_f1200_bot.png    melonDS frame 1200: "Skip the cutscene? Yes/No"
tools/melonds-p5al-probe.patch the instrumentation above
tools/nnk_to_melonds.py        .nnk -> MELONDS_GXLOG_PRESS
tools/p5al_alloc_ab.py         allocator A/B -> first differing allocation
tools/ppm2png.py               melonDS PPM -> PNG
```

Regression lock: `phase_5al_the_guard_is_benign_on_the_melonds_object_and_the_payload_is_0x1a8`
(class 47) — the guard's NULL test is the whole difference, and the 0x1A8
payload is the ROM's own arithmetic (`(0x25<<4)-0xA8`, +0x10 header = the
measured 0x1B8 stride).
