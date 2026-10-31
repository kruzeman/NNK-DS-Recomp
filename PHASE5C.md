# Phase-5C — the 0x0202A15C pop: a call-site stack word, and the BLX-state bug that owns the excursion

Follows Phase-5B (merged `3c3516c`).  Scope: root-cause the stack-content
divergence at `0x0202A15C` (`pop {r3, pc}` returning `0x020FC2A4` native vs
`0x02029350` melonDS), fix it with measured semantics, or stop honestly.

**Result: the pop is not a stray stack word — it is the return address of the
allocation call site that is currently innermost, and both values are correct
for their own run (the two runs are in different allocation loops of the same
shared helper).  The real defect one layer down is a *processor-state* bug in
the live re-decoder: a Thumb `blx <imm>` did not switch its target to ARM
state.  Fixed and locked (17 cargo tests).  With the fix the whole
`0x0214xxxx`-`0x0217xxxx` excursion and the `0x02178E9E` halt disappear
(25,284 garbage PCs -> 0).**

## 0. Headline

* `0x0202A15C` is the epilogue of the SDK helper at `0x0202A13C`
  (`push {r3, lr}` … `pop {r3, pc}`).  At the divergence point
  **r13 = 0x027E3A98 — a DTCM address, not main RAM** (this is why the Phase-5B
  main-RAM dump at `0x3E3A9C` read zero; the ARM9's stack lives in the 16 KiB
  DTCM the game has mapped over `0x027E0000`).  The popped word is
  `[r13+4] = [0x027E3A9C]`.
* **The last writer of that slot** (new `NNK_ST_WATCH` history, `p5c_stw.txt`)
  is the push itself: `st4 addr=0x027E3A9C val=0x020FC2A4 pc=0x0202A13C
  step=38986960`.  So the value is `lr` at entry to `0x0202A13C`.
* That `lr` came from a **thunk/PIC-stub tail call**: `0x020462DC` is
  `ldr r3,=0x020774B4; mov r1,r0; ldrsb r2,[r3]; ldr r0,[r3,#4];
  ldr ip,=0x0202A13C; bx ip` — it forwards to the helper *without touching lr*.
  Native called that stub with `bl 0x020462DC` at **0x020FC2A0** (overlay
  code), melonDS called the sibling stub `0x020462FC` with `bl 0x020462FC` at
  **0x0202934C** (main code).  Hence `0x020FC2A4` vs `0x02029350`.
* Both call sites are legitimate ROM code — they are the two halves of the
  game's object-allocation loops (overlay `0x020FC1xx` loop, main `0x020293xx`
  loop).  Both runs execute **both** loops (melonDS reaches `0x020FC1E4` and
  `0x020FC2A0` at trace index 1,257,690 of its 49M..55M window).  The PC
  streams agree for the **195-PC shared allocation helper** that both call
  sites enter, and break exactly at the stub tail
  (native `0x020462F0` / melonDS `0x02046310`).
* **Class: not a link-address class, not stack-slot reuse** — a *call-site*
  difference inside a shared helper; the pop value is a faithful consequence.
* The defect that actually costs the picture is below it: the run reached
  `0x0214`-`0x0217` because the live re-decoder executed
  **ARM code in Thumb state** (see §2).  Fixed in §3; measured in §4.

## 1. The instrument work this phase did

* `p5c_ramdiff.py` — word diff of the native `NNK_DUMP_MEM` gmem against a
  melonDS 4 MiB main-RAM dump taken **at the same PC**
  (`MELONDS_RAMDUMP_PATH` + ts gate 49,081,780..49,081,790 -> the pop at
  ts 49,081,783).  64,606 differing words at that instant (heap/strings/stack),
  which is why a whole-RAM diff cannot name a class — the stack slot had to be
  traced, not compared.
* `NNK_DUMP_MEM` halted exactly *at* the pop (`NNK_STEP_LIMIT=38987154`
  -> `halt_pc=0x0202A15C`, pop not yet executed) so the frame was intact.
* `p5c_align.py` / `p5c_align2.py` — longest common suffix of the native trace
  against melonDS's ts-gated trace, keyed on the pop that returns to
  `0x02029350` (12 candidates in the window, all with the same 195-PC suffix).
* `p5c_melctx.py` — locate a melonDS PC in the trace and print its context
  (this is what proved melonDS also runs `0x020FC1E4` / `0x020FC2A0`).

## 2. What really diverges: the state of a Thumb `blx`

First in-excursion PC (`p5c_halt_trace2.out`, `NNK_TRACE_FROM=39234000`):

```
step 39,237,11x ... 0x020d2414 0x020d2416 0x020d2418 | 0x020e7968 0x020e796a
                    0x020e796e 0x020e7972 0x020e7976 | 0x02160394 ...
```

`0x020D2418` is `blx 0x020E7968` (halfwords `F015 EAA6`, Thumb).  The target
holds the **ARM** word `0xE92D4038` = `push {r3, r4, r5, lr}` — but the trace
advances in Thumb strides (`+2`, `+4`) through it and then falls into the
pointer table at `0x02160394` (`02160394 02160384 021603A4 …` as raw words),
walking 25,284 PCs through `0x0214`-`0x0217` until it halts on the **data**
word `0x1C36F000` at `0x02178E9E`.

The live decoder's own log (`NNK_SMC_TRACE`) is the measurement:

```
smc: on-demand step=39237110 pc=0x020D2418 word=0xEAA6F015 state=thumb -> 0x020E7968
smc: on-demand step=39237111 pc=0x020E7968 word=0xE92D4038 state=thumb -> 0x020E796A   <- ARM word, Thumb state
```

Cause: the T bit is part of the ARM9's *state*, but `nnk_smc_try` /
`nnk_smc_exec_any` derived it per PC from the compiled map (`nnk_pc_is_thumb`)
and a "sticky last state" guess.  A `blx <imm>` target that has no compiled
case therefore inherited Thumb.  (The compiled emitter is right — `nnkktool`'s
`thumb_flow` already returns `Flow::Call(target, false)` — only the C mirror
was wrong.)

## 3. The fix (one file, `tools/nnk_smc.inc`)

```c
static uint32_t smc_t_pc; static int smc_t_val; static unsigned long long smc_t_step;
static void smc_branch_mode(uint32_t raw_target) {          /* at the branch */
    smc_t_pc = raw_target & ~1u; smc_t_val = (int)(raw_target & 1u);
    smc_t_step = steps;
}
static int smc_carried_thumb(uint32_t pc, int *state) {     /* at the next dispatch */
    if (smc_t_val < 0 || smc_t_pc != pc || steps != smc_t_step + 1ull) return 0;
    *state = smc_t_val; return 1;
}
```

* Thumb BL/BLX arm: suffix bit 12 separates the forms — BL (`11111`) stays in
  Thumb, BLX (`11101`) forces **ARM**:
  `smc_branch_mode(*npc | ((hw2 & 0x1000u) ? 1u : 0u));`
* ARM `blx <imm>`: `smc_branch_mode(*npc);` (BLX is ARM state).
* Both dispatch hooks consume it (`if (smc_carried_thumb(pc, &st)) thumb = st;`)
  and `nnk_smc_exec_any` skips its map/sticky fallback when a state was carried.
* The **step stamp** is what keeps it exact: a loop that later revisits the same
  target without going through the branch must not inherit a stale state.

Rejected variant (recorded, not shipped): carrying the state for `pop {pc}` /
`bx rM` / `ldr pc` / `ldm {..pc}` as well.  Per the ARM ARM those are equally
authoritative, but on this build the very first affected `pop {pc}` (step
13,146,753, DTCM frame returning an odd value `0x0215AC21`) exposes an *earlier*
data walk in the band the game is reloading at that moment, and the run halts at
13.1M — a regression of the observable frontier.  That variant is the natural
follow-up (it needs the stale-mode-map problem below solved first).

## 4. Verification

```
build   NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_p5b_kp.c bash tools/build_split.sh \
        out/split/arm9_p5c_fix5

60M-step run (headless, NNK_SMC_IMAGE as in Phase-5B)
  before (Phase-5B): steps 39,262,401  pc PCs in 0x0214..0x0217: 25,284
                     halt 0x02178E9E "smc: unsupported live instruction
                     img=0x00000000 word=0x1C36F000 state=arm band_gen=8192"
  after  (Phase-5C): steps 39,237,994  PC PCs in 0x0214..0x0217: 0
                     halt 0x020D2420  (budget consumed inside the boot wait
                     loop 0x020078F4..0x020078FC); no smc halt at all
                     (smc: probes=35,850,990 stale=130,972 redecoded=132,424
                      halts=0 state_fallbacks=0 on_demand=1,452)
  distinct_pcs 16,331 -> 16,331;  the 0x020E7968 helper now executes as ARM and
  returns normally (halt state r14=0x020E79C0, r4=0x02143174)

cargo test         17 passed (14 Phase-4x/5A + 3 new Phase-5C: BLX-imm state,
                   BL-imm stays Thumb, C-half source lock)
```

The three new tests (`nnktool/src/decoder_regression_tests.rs`, class 6):
`thumb_blx_immediate_target_is_arm_state` (fixture `F015 EAA6` at
`0x020D2418` -> `Flow::Call(0x020E7968, false)`),
`thumb_bl_immediate_stays_in_thumb_state` (the `F015 FAA6` neighbour, which a
lazy fix would wrongly break) and `runtime_smc_redecoder_carries_the_blx_state`
(source lock on `nnk_smc.inc`).

## 5. The picture (honest)

Frames are still **uniform white** — the DISPCNT content switch is at
`0x0207CB10` / melonDS frame ~102, and this run stops at step 39.24M in the
wait loop long before it.  No new PNG is claimed this phase; the display path
is untouched (Phase-5B measured 0 differing pixels vs `f00070.scr`).

Operator telemetry folded in (shipped Phase-5B binary): all VRAM stores land in
LCDC at this stage (`ABG=0 BBG=0 AOBJ=0 BOBJ=0 LCDC=491520`), and the display
bridge leaves ~114k IO writes unhandled — both are boot-phase facts, not this
barrier's cause (no VRAM/palette consumer exists yet: melonDS's frames 0..101
are also blank).

## 6. The next barrier (named, measured)

**The boot wait loop at `0x020078F4`..`0x020078FC`.**  After the fixed
`0x020E7968` helper returns, the ARM9 spins there until the step budget runs
out (39,237,994 steps, ~75 frames presented) — i.e. the guest is waiting for an
interrupt source the current ARM7/IRQ model never raises at this phase.  The
follow-up should (a) identify what that loop waits on (a flag/timer/card
completion at `0x02069688`/`0x02143EB4`-adjacent state, `r7=0x02069688`), and
(b) confirm which IRQ line melonDS takes at ts ~15.9M (its 0x020078xx
equivalent) with `MELONDS_IRQTRACE`.

Secondary, lower-priority (found while shrinking the diff): the compiled
per-PC state map (`nnk_pc_is_thumb`) describes the **generation-0 image**, so
for a band the game has reloaded it is stale — measured at `0x0215AC32`, where
a swapped overlay's ARM word is classified Thumb.  That is what blocks the
rejected variant in §3.

## 7. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/stack-div     # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
python3 tools/p5c_blx_interwork.py               # idempotent .inc patch
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_p5b_kp.c bash tools/build_split.sh out/split/arm9_p5c_fix
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 \
  NNK_GX=headless NNK_TRACE_FROM=39234000 NNK_SMC_TRACE=400000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_p5c_fix work/phase3a/arm9_stored_padded.bin
# the stack slot's writer (pop-frame evidence)
NNK_ST_WATCH="0x027E3A80:0x027E3AC0" NNK_ST_WATCH_PATH=$S/p5c_stw.txt \
  NNK_ST_WATCH_MAX=3000000 ...                     # -> p5c_stw.txt
(cd nnktool && cargo test)                         # 17 tests
SKIP_MELONDS=1 bash tools/verify_phase2a.sh        # branch code
```

Evidence (`$S`): `p5c_stw.txt` (frame writers, 5,806 stores in the window),
`p5c_pop.txt` (registers at the pop: r13=0x027E3A98), `p5c_natram_pop.bin` /
`p5c_melram_pop.bin` (same-PC RAM dumps), `p5c_ramdiff.py`, `p5c_align2.py`,
`p5c_halt_trace2.out`, `p5c_fix13_mem.bin`, `p5c_smctrace.err` (the two
`state=thumb` lines at 0x020D2418/0x020E7968), `p5c_fix5_60.{out,err}`.

Changed in this phase: `tools/nnk_smc.inc` (+33 lines: carried interworking
state), `tools/p5c_blx_interwork.py` (idempotent applier),
`nnktool/src/decoder_regression_tests.rs` (+3 tests), `PHASE5C.md`.
