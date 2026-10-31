# Phase-5I — the 0x020D32xx halt branch was a live-decoder bug: the guest's jump-table dispatch lost its index, and #9093 (B7 0x0DA44600) is now issued

Follows Phase-5H (merged `d5f9603`).  Scope was two targets: (a) the wake event
3 cycles before card command #9093, (b) why the `0x020D32xx` Thumb state
machine resolves to the halt case in native but not in melonDS.

**Headline (all measured on this branch):**

1. **(b) is answered, and it was never a missing wake.**  The `0x020D3250`
   halt stub is reached by *falling through a Thumb jump table as code*,
   because three live-decoder (C, `tools/nnk_smc.inc`) instruction classes at
   the dispatch site are wrong.  The SDK state machine at `0x020D24D8` is a
   table dispatch:

   ```
   20d24d8: da01      bge.n 0x20d24de
   20d24da: f000 feb9 bl    0x20d3250          <- the halt stub (Phase-5H's lr)
   20d24de: 1968      adds  r0, r5, r5         ; r5 = state index = 3
   20d24e0: 4478      add   r0, pc             ; -> 0x020D24EA
   20d24e2: 88c0      ldrh  r0, [r0, #6]      ; -> 0x00E8 (halfword case offset)
   20d24e4: 0400      lsls  r0, r0, #16
   20d24e6: 1400      asrs  r0, r0, #16
   20d24e8: 4487      add   pc, r0             ; -> 0x020D24EC + 0xE8 = 0x020D25D4
   ```

   melonDS follows that path (`0x020D24E8 -> 0x020D25D4`, ARM9 trace window
   `0x020D2000..0x020D3300`, 356 PCs, one pass); native used to walk
   `0x020D24EA..0x020D25A6` — the *table data* — as instructions and arrive at
   the unconditional `bl 0x020090D8` at `0x020D3254`, i.e. the "halt branch"
   was a decoder artefact, not a guest decision.

2. **The first differing input is r0 at `0x020D24E8`.**  Native held
   `r0 = 0x00000000`, melonDS `r0 = 0x000000E8` (the table entry for case 3).
   With `r0 = 0`, `add pc, r0` is `pc + 4` and the fall-through starts.

3. **Three decode classes, all in the live C re-decoder** (`tools/nnk_smc.inc`,
   the path every PC outside the pre-compiled bands takes):

   | # | class | site | old behaviour | fix |
   |---|---|---|---|---|
   | 9 | Thumb format 5 `ADD rd, pc` read `r[rs]` directly | `0x4478` at `0x020D24E0` | `r[15]` slot is never maintained (stays 0), so `add r0, pc` returned the stale `r0` instead of `0x020D24EA` | both operands via `smc_reg()` (R15 -> `(pc+4) & ~3`) |
   | 10 | Thumb format 8 (`LDRH/STRH imm5`, `0x8xxx`) was matched by the format-10 SP-relative test `(hw & 0xF000) == 0x8000` | `0x88C0` at `0x020D24E2` | `ldrh r0,[r0,#6]` executed as `ldr r0,[sp,#imm]` -> 0 | new format-8 arm (`imm5 << 1`, `ld16`/`st16`); format 10 now masks `0x9000` |
   | 11 | ARM-state `BLX <imm>` tagged its target ARM | `0xFA01E474` at `0x0207CAC0` | `blx 0x020F5C98` (a Thumb function) ran in ARM state, 4-byte steps through `F754 EAC6` and branched to `0x012B39FC` | `smc_branch_mode(*npc \| 1u)` — ARM BLX(1) sets T (ARM ARM A7.1.4) |

   Class 9 alone was not enough: with only it patched the run still stalled
   (`r0` stayed 0), which is how class 10 was found; class 11 appeared only
   after the state machine got past the dispatch and called further into the
   overlay (`0x020F5C98`).  Each is locked by a cargo test (suite 21 -> 23).

4. **(a) the wake is unchanged and was never the barrier.**  The idle-loop
   wakes Phase-5F/5G/5H measured (VBlank `0x00000001`, Timer1 `0x00000010`,
   cart bit 18 `0x00040000` in `IE=0x00040019`) are delivered by native too;
   the interrupt that leads into the state machine at `ts=103,577,894` runs
   the same handler in both engines once the *dispatch* decodes.  There is no
   separate wake word to deliver: melonDS's "3 cycles before #9093" is the
   IRQ entry that this same `0x020D24D8` block services.

## 1. Result — #9093 is issued natively

```
p4t: start op=0xB7 addr=0x0DA44600 size=512 cyc=69376989 steps=39316599   <- #9093
p4t: blk   addr=0x0DA44600 crc=0x831B7BE1D0C13CE n=128 steps=39322770     <- full 0x200-byte block
...
p4t: last B7 block ... starts=9333                                       <- was 9093
done steps=40768913 distinct_pcs=16694 halt_pc=0x020F5DF4
     halt=dispatch to untranslated pc (the guest returned to 0x00000000)
gx: 78 frames presented
```

| check | before (Phase-5H) | now (this branch) |
|---|---|---|
| card transfer starts | 9,093 | **9,333** (+240) |
| command #9093 `B7 0x0DA44600` | absent | **issued**, 128-word block served |
| `0x020D3250` halt stub | executed at step 39,238,888 | **never reached** |
| state-machine case | fall-through into table data | **`0x020D25D4` (case 3), same as melonDS** |
| distinct guest PCs @60M | 16,447 | **16,694** |
| run end | parked in `0x02009110` halt loop at 39.32M | progresses to **40.77M** |
| frames | 113 uniform | 78 uniform (dies earlier in wall-clock frames) |
| pictures | uniform | still uniform (no content) |

## 2. Verification (branch code)

```
$ cd nnktool && cargo test
test result: ok. 23 passed; 0 failed

$ sh tools/verify_phase2a.sh          # green on the branch code, VERIFY_RC=0
3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
melonDS trace fully reproduced
```

Trace check of the dispatch (native, `NNK_TRACE_FROM=39238780`):

```
0x020d24d6 0x020d24d8 0x020d24de 0x020d24e0 0x020d24e2 0x020d24e4 0x020d24e6
0x020d24e8 0x020d25d4 0x020d25d6 0x020d25d8 0x020462dc ...
```

— identical to melonDS's `0x020d24e8 -> 0x020d25d4` (the ARM9 trace above),
and the register dump at `0x020D24E8` now reads `r0=000000E8 r5=00000003`,
matching melonDS.

Reproduce:

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5i
cd /home/hermes/NiNoKuni/worktrees/wake-9093 && ln -sfn /home/hermes/NiNoKuni/work work
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c SPLIT_DIR=work/split/i \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5i

env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=60000000 NNK_CARDLOG=1 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  /home/hermes/NiNoKuni/out/split/p5i work/phase3a/arm9_stored_padded.bin 2> $S/f3.err
grep 'start op=0xB7 addr=0x0DA44600' $S/f3.err
```

melonDS side (same window, for the dispatch comparison):

```sh
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
  SDL_AUDIODRIVER=dummy MELONDS_ARM9TRACE_PATH=$S/mel_d2xxx.log \
  MELONDS_ARM9TRACE_LO=0x020D2000 MELONDS_ARM9TRACE_HI=0x020D3300 \
  MELONDS_ARM9TRACE_N=20000 MELONDS_ARM9TRACE_EXIT=0 timeout 60 xvfb-run -a \
  /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
```

New diagnostics added: `NNK_SMC_FAILLOG=1` prints the word/state a live-decode
failure happens on (`smcfail pc=... word=... thumb_try=...`) — without it the
run's `halt_pc` (the last *traced* PC) hides the real failing PC
(`untranslated pc=0x012B39FC` in the class-11 case).

## 3. Next barrier (named)

**Follow-up barrier E — the null return out of the `0x020F5C98` frame.**
At step 40,768,913 the guest executes the epilogue at `0x020F5DF4`
(`movs r0,#0; add sp,#16; pop {r3,r4,r5,r6,r7,pc}`) and returns to
`0x00000000` (`untranslated pc=0x00000000`).  The frame was entered at
`0x020F5C98` (`push {r3,r4,r5,r6,r7,lr}`, ARM BLX from `0x0207CAC0`), so the
`add sp,#16` + 6-register pop does not match the push — either a live-decoder
stack-form class (a `push`/`pop` arm reached after class 10 changed the
halfword path) or a genuine coroutine-style frame switch whose saved context
lives elsewhere.  Anchor for the next worker: step 40,768,913, `0x020F5DF4`,
`r13=0x027E3B18` at entry, and the `pop` slot that reads 0.  Compare with
melonDS's `0x020F5C98..0x020F5E00` PC trace at the same point.
