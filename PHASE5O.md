# Phase-5O — barrier I3: the 5th argument is garbage because the CALL never happened

Follows Phase-5N (merged `288515f`), which pinned the mechanism but not the
cause: the movie-player table generator `0x020545B8` "receives" a garbage 5th
argument (destination pointer), and `str r0,[r1,r7,lsl #2]` at `0x020547D4`
then writes to guest address `0x51`, aliasing ITCM and zeroing the IRQ
dispatcher's literal pool `0x01FF8050/4`.

**This phase root-causes I3 by measurement and fixes it.**  The generator is
never *called* at all: the run enters its **middle** (`0x02054764`, prologue
skipped) with a foreign frame, so `ldr r1,[sp,#296]` reads whatever the
unrelated frame happens to hold.  The bogus entry is one instruction earlier:
the compiled `blx <imm>` case dropped the state bit the encoding *forces*.

**Headline**

1. **The first differing word is the Thumb BLX word pair `0xF75D 0xE936` at
   `0x020F6ED6` = `blx 0x02054144`.**  `Kind::BlxImm` (global dispatch path)
   emitted `pc = 0x02054144u;` with no `nnk_smc_note_branch(...)`, so the
   interworking state the BLX switches to (Thumb -> ARM) never reached the live
   re-decoder.  The re-decoder fell back to `smc_t_sticky` = Thumb (the region
   before the BLX is live-decoded Thumb code) and ran the **ARM** word
   `0xE92D4010` (`push {r4,lr}`) as Thumb.
2. **Measured, pre-fix** (native, trace window from step 2,256,601,818):
   ```
   0x020f6ed2 0x020f6ed4 0x020f6ed6     <- Thumb `blx 0x02054144`
   0x02054144 0x02054146 0x0205414a ... <- ARM bytes stepped as Thumb (2 bytes)
   ... 0x020541be -> 0x02054502 -> 0x02054764  <- middle of 0x020545B8
   ```
   At `0x02054764` the frame is not the generator's: `sp = 0x027E3968`,
   `[sp,#0x128] = 1`, `r7 = 0x14`, so `str r0,[r1,r7,lsl #2]` stores to
   `1 + 0x14*4 = 0x51` and `1 + 0x15*4 = 0x55` - exactly the two Phase-5N
   store-watch hits.  (Phase-5N read `r1` as `0x51`; the storewatch line's
   `r1=1` plus `r7` is the same address, the index is the loop counter.)
3. **melonDS executes the same BLX target in ARM state**: trace window
   `0x02054144..0x02054150` gives `0x02054144 0x02054148 0x0205414c` (4-byte
   steps).  The engine difference is the *state*, not the code path.
4. **The fix**: `Kind::BlxImm` notes the raw target like every other
   interworking transfer already did (`bx`/`blx Rm`, `ldm`/`pop {..,pc}`):
   ```c
   nnk_smc_note_branch(0x02054144u);   /* Thumb BLX -> ARM (bit0 clear) */
   pc = 0x02054144u;
   ```
   (ARM-form BLX targets Thumb, so there the note is `target | 1`.)
5. **Measured, post-fix, same site and step:**
   ```
   smc: on-demand step=2256698732 pc=0x02054144 word=0xE92D4010 state=arm sticky=0 -> 0x02054148
   smc: on-demand step=2256698733 pc=0x02054148 word=0xE1A04001 state=arm sticky=0 -> 0x0205414C
   smc: on-demand step=2256698734 pc=0x0205414C word=0xEB0000D6 state=arm sticky=0 -> 0x020544AC
   ```
   and the PC trace is `0x020f6ed2 0x020f6ed4 0x020f6ed6 0x02054144
   0x02054148 0x0205414c 0x020544ac ... 0x02054500 0x02054150 ...`, i.e. the
   real ARM function and its real callee.
6. **The barrier is gone.**  `0x02054764`/`0x020545B8` are never executed
   (`grep -c` = 0 over the whole 4M-PC trace window); the ITCM store watch
   (`NNK_STOREWATCH="0x20:0x60"`, mask `0x7FFF`) logs **only** the crt0 copy
   into `0x01FF8020..0x01FF805C` - no store to `0x51`/`0x55`, so the
   dispatcher's literal pool stays intact.  The run goes from the old halt
   (step 2,256,702,439, `distinct_pcs` 22,704, 4,035 frames) to the step budget
   **2,600,000,001** with **`distinct_pcs` 22,888 and 4,648 frames**.
7. **The picture arrives**: the frame dump shows flat black/white/grey up to
   frame 4,100, then *real* content - frames 4,200/4,300/4,400/4,500/4,600 have
   499/718/651/604/1,833 distinct colours and change by 64/95/85/90/99.7% of
   their pixels from one dump to the next (see `shots/f.04200..04600.top.png`).
   This is the first time any post-boot frame of this run is non-uniform.
   At the step budget the guest sits in the known SDK idle 5-PC cycle
   (`0x02009118 0x0200911c 0x02009120 0x02007138 0x02007134`), i.e. the
   intro has been shown and the engine is waiting (follow-up barrier in §5).

## 1. Why the argument could not be "computed wrong"

Static scan of both recompiler inputs for a transfer to `0x020545B8`:

```
python3 find_callers.py work/ghidra_src/arm9.bin 0x02000000 0x020545B8
  calls: [('ARM_BL', '0x2054e24', ...)]          # exactly one, in arm9.bin
python3 find_callers.py work/phase3a/ovl_p4f4.bin 0x020C0000 0x020545B8
  calls: []                                      # none in the overlay
```
The only call site sets `r2 = 3`, `r3 = 0`, and the 5th argument to
`add r5,sp,#4` (a stack buffer):
```
2054e0c: mov  r4, #3
2054e10: add  r5, sp, #4
2054e14: ldr  r0, [pc, #60]
2054e18: mov  r2, r4
2054e1c: mov  r3, #0
2054e20: str  r5, [sp]        ; 5th argument = &local[0], a DTCM address
2054e24: bl   0x020545B8
```
so *if* the generator were called from there, the destination would be a valid
DTCM pointer.  With `NNK_DEBUG_AT_PC=0x020545B8` armed the whole 2.2567G-step
run logs **zero** hits: the function's entry point was never executed.  The
store therefore came from the middle of the function, which only a wrong
control transfer can produce.

## 2. The class: an interworking transfer that does not carry its state

`blx <imm>` is the one transfer whose target state the encoding determines
without any register (ARM ARM A7.1.4): the Thumb halfword pair (`11110` +
suffix `11101`) targets ARM, the ARM form (`cond=1111`, bits 27..25 = `101`)
targets Thumb.  Phase-5J/5K taught the runtime to carry that state
(`nnk_smc_note_branch` -> one-step `smc_carried_thumb`) for
`bx`/`blx Rm`, `ldm`/`pop {..,pc}` and `ldr pc`; `Kind::BlxImm` was left out,
so the *only* remaining interworking form without a carrier was the one this
run takes at step 2,256,698,732.

The re-decoder's fallback chain (`nnk_pc_is_thumb` -> `smc_state_from_regs` ->
`smc_t_sticky`) has no way to recover it: the map's ARM verdict is conflated
with "unclassified", no register holds the target, and the last re-decoded PC
before the BLX was Thumb - measured `state=thumb` for
`0x020f6ec4..0x020f6ed4`, then `state=thumb` for `0x02054144` pre-fix,
`state=arm` post-fix.

## 3. The fix

`nnktool/src/recompile.rs`, `Kind::BlxImm`, global branch:

```rust
let raw = if thumb { *target & !1u32 } else { *target | 1u32 };
body.push(format!("nnk_smc_note_branch(0x{:08X}u);", raw));
...
body.push(format!("pc = 0x{:08X}u;", target & !1));
```

Locked by `cargo test` class 18 (`compiled_blx_immediate_carries_the_state_it_switches_to`,
suite **31 -> 32**): the Thumb pair decodes to `Kind::BlxImm{0x02054144}`, the
`bl` neighbour (`0xF75D 0xF936`) must NOT be interworking, the emitter block
must derive the raw target from the site state, and the splice tool must decide
the same way from the encoding.

The image under test is the frozen Phase-4F artifact, which cannot be
regenerated byte-for-byte (the Phase-5J/5K precedent), so the same note is
spliced by `tools/p5o_blx_note.py`:

```
blx-imm sites noted: 448  already-noted: 0  shim calls: 0  unknown: 0
```

The site's state comes from the generation-0 word (`0xFA......` = ARM form,
`11110`+`11101` = Thumb pair), never from a guess; 448/448 sites matched an
encoding, so the tool never had to skip one.

## 4. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 32 passed; 0 failed
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh
RESULT: PASS (rc=0) - strict prefix 1000/1000
```

Run (branch code, `p5o1` = p5k image + the 448 spliced notes):

```
done steps=2600000001 traced=2600000000 distinct_pcs=22888 hle_hits=11202
     halt_pc=0x020527FC halt=step budget exhausted
gx: 4648 frames presented
storewatch 0x20..0x60 mask 0x7FFF: 16 lines, all crt0 copies at step 3,574,0xx
                                     -> no store to 0x51 / 0x55
```

Frames (`NNK_GX_SHOT_EVERY=100`) in `/home/hermes/NiNoKuni/shots/`:
`f.04200.top.png` .. `f.04600.top.png` - non-uniform, changing.

## 5. Result and the next barrier (named)

Barrier I3 is closed: the table generator is never mis-entered, the stray
`0x51` store does not happen, the ITCM literal pool is intact, and the run
continues 343M steps further to the step budget with a *picture* on both
screens.

**Barrier I4 (named): the movie path is not fed / the run parks in the SDK idle
5-PC cycle.**  At the step budget the PC is
`0x02009118 0x0200911c 0x02009120 0x02007138 0x02007134` (the same parking the
Phase-5K barrier G described), 4,648 frames have been presented, and no
Mobiclip frame geometry (the 256x192 video blit) has been proven: the frames
from 4,200 on are real game content whose *source* has not been identified as
the movie decoder yet.  The next phase should establish whether the Mobiclip
player is entered at all after this point (ITCM `0x01FF8540`, player entry
`0x0207B218`, Y2R `0x04002300..`) and which IPC/IRQ wakes the parked loop -
i.e. the Phase-5L/5K parking question, now reached with a working movie-path
frame instead of a zeroed dispatcher.

## 6. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5o
python3 tools/p5o_blx_note.py work/phase3a/arm9_thumb_ovl_itcm_p5k.c \
        work/phase3a/arm9_thumb_ovl_itcm_p5o.c \
        --img "0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin"
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5o.c SPLIT_DIR=$S/split \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5o1

env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2600000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_STOREWATCH="0x20:0x60" NNK_STOREWATCH_MASK=0x7FFF \
    NNK_STOREWATCH_PATH=$S/f2/itcmw.log \
    NNK_TRACE_FROM=2256698500 \
    NNK_SMC_TRACE=3000 NNK_SMC_TRACE_FROM=2256698000 \
    NNK_GX_SHOT=$S/f2/frame/f NNK_GX_SHOT_EVERY=100 \
    /home/hermes/NiNoKuni/out/split/p5o1 work/phase3a/arm9_stored_padded.bin

# melonDS: the same BLX target is ARM code there
env XDG_CONFIG_HOME=... SDL_AUDIODRIVER=dummy \
    MELONDS_ARM9TRACE_PATH=$S/mel/t54144.txt \
    MELONDS_ARM9TRACE_LO=0x02054144 MELONDS_ARM9TRACE_HI=0x02054150 \
    MELONDS_ARM9TRACE_N=40 timeout 200 xvfb-run -a .../melonDS "Ni No Kuni.nds"
# -> 0x02054144 0x02054148 0x0205414c   (4-byte steps: ARM state)
```

Note for whoever continues: do **not** "fix" this by forcing the ARM decode in
the re-decoder — the state is a guest register (CPSR T), and the compiled
transfers are the only place the runtime learns it.  Every new interworking
form must note its target (the class is now covered by tests 15..18).
