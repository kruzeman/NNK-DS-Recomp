# Phase-5AO — the ov17 data jump at 0x0213D2AC was a STALE Thumb sticky state on an unrooted fall-through

Follows Phase-5AN (merged as master `295e4e5`, class 49).  5AN cleared the
559,830,681 halt and left the guest at step **560,128,158**, pc
**0x0213D2AC**, halted with `hle swi imm24=0x746E65 num=0x74` — the ARM word
`0xEF746E65` is the ASCII string `"ent"` + `0xEF`, i.e. the guest was executing
overlay 17's read-only data tail.

**5AO clears that barrier for a measured reason.**  The branch into data was the
`pop {r4,r5,r6,pc}` at arm9 **0x0202B50C**, and the frame it popped was wrong
because the unrooted PC **0x0202A8F8** was re-decoded in the **Thumb** state —
the state of the last *live* decode 190k steps earlier — instead of ARM (the
state of the instruction that fell through into it).  With the state taken from
the predecessor's stride the helper runs as ARM, returns to its real caller
`0x0202C1A0`, and the guest runs **64,352 steps further**: new frontier
**step 560,192,510**, pc **0x02043C04**, `ldmfd sp!,{pc}` with a corrupted sp.

## 1. What 0x0213D2AC is (and is not)

`0x0213D2AC = ov17 base 0x020D0A00 + 0x6C8AC`, i.e. **inside overlay 17's
declared code size** (`0x72700`) but in its **read-only data tail** — the last
part of the module the loader copies verbatim from the ROM (no runtime load
*wrote* it: it is the same bytes as `work/dsd_extract/arm9_overlays/ov017.bin`
at that offset, which is 100 % identical to our own decompressor's output per
PHASE5AF).

```
0x0213D24C  50 83 04 02  00 00 00 00  00 00 00 00  29 e0 0f 02   <- pointer table
0x0213D25C  dd e1 0f 02  ed eb 0f 02  95 eb 0f 02  10 d8 04 02
0x0213D29C  f5 32 0d 02  44 ec 0f 02  2f 64 61 74  2f 45 76   .2..D.../dat/Ev
0x0213D2AC  65 6e 74 2f 44 65 6d 6f 2f 00 00 00 2f 00 00 00   "ent/Demo/..."
0x0213D2BC  2e 74 78 74  00 00 00 00  2f 64 61 74 61 2f 66 6f   ".txt".../data/fo
0x0213D2CC  6e 74 2f 66 6f 6e 74 31 32 2e 4e 46 54 52        "nt/font12.NFTR"
0x0213D2DC  2f 64 61 74 61 2f 66 6f 6e 74 2f 72 75 62 79 2e 4e 46 54 52  "/data/font/ruby.NFTR"
```

A pointer/string pool: file-path strings (`/data/Event/Demo/`, `.txt`,
`/data/font/font12.NFTR`, `/data/font/ruby.NFTR`, `/data/font/font10.NFTR`)
interleaved with 32-bit pointers into arm9 (`0x02044868` — the Phase-5AL
stream guard — `0x02043A40`, `0x02044BB4`, …) and into ov17
(`0x020FEBED`, `0x020D3381`, …).  It is **never a legitimate code target**:
the literal `0x0213D2AC` (and `0x0213D114`) does not occur anywhere in
`work/ghidra_src/arm9.bin` or in any of the 21 extracted overlays.

## 2. Who branched into it — `pop {r4,r5,r6,pc}` at 0x0202B50C

Native operator-session run (`NNK_GX=headless NNK_INPUT_FILE=tools/nnk_operator_session.nnk`,
`NNK_TRACE_FROM=560127000`), pc trace (`work/phase5ao/tr4.out`, 1158 pcs):

```
step 560128043  0x0202A8F8            <- the first wrong state (see §3)
step 560128044  0x0202A8FA            <- Thumb stride
step 560128045  0x0202AC9E            <- fabricated Thumb branch chain
step 560128046  0x0202AEAE
step 560128047  0x0202B1B2
step 560128048  0x0202B4F6
step 560128049  0x0202B4F8            <- falls into COMPILED ARM cases
        ...     0x0202B4FC 0x0202B500 0x0202B504 0x0202B508
step 560128057  0x0202B50C  pop {r4,r5,r6,pc}   (0xE8BD8070)
step 560128058  0x0213D114            <- popped a DATA pointer (ARM, stride 4)
        ...     +4 each step through the table/strings ...
step 560128158  0x0213D2AC  0x2F746E65 = "/dat"   -> next word 0xEF746E65 = "ent"+0xEF
halt            step 560128158, hle swi imm24=0x746E65 num=0x74
```

The halted register file (`halt state:`) is `r0=1 r1=0xDF72 r2=2 r3=2
r4=0x020774B4 r5=0x02295304 r6=0x022CD028 r7=0x02143158 r13=0x027E39D8
r14=0x0202C1A0` — `r14 = 0x0202C1A0` is the **real return address of the
helper** (see §4); the stack no longer held it.

## 3. Root cause: `smc_t_sticky` was stale, and it was consulted instead of the predecessor's stride

`0x0202A8E8` is an ARM table-lookup helper:

```
0x0202A8E8  ldrh r2,[r0]
0x0202A8EC  cmp  r1, r2
0x0202A8F0  movcs r0,#0
0x0202A8F4  bxcs lr            @ compiled case, ARM; NOT taken on this path
0x0202A8F8  ldrh r2,[r0,#2]    @ 0xE1D020B2 — NO compiled case
0x0202A8FC  ldr  r0,[r0,#4]
0x0202A900  tst  r2,#1
0x0202A904  addne r0,r0,r1,lsl#4
0x0202A908  addeq r0,r0,r1,lsl#3
0x0202A90C  bx   lr            @ -> 0x0202C1A0
```

The live re-decoder's state ladder (`tools/nnk_smc.inc`) was: compiled Thumb
map → interworking note (Phase-5C) → a register still carrying the tagged
target (Phase-4Z) → **`smc_t_sticky`**.  For 0x0202A8F8 the map says "not
Thumb" (it is ARM), `smc_state_from_regs` finds nothing, and the branch note
does not apply — so the ladder used `smc_t_sticky`, which is the state of the
**last live-decoded pc** and is never updated by the compiled cases.  It had
been Thumb since a decode 190k steps earlier.

Instrumented (`NNK_SMC_TRACE=5000 NNK_SMC_TRACE_FROM=560128000`,
`work/phase5ao/tr5.err`):

```
smc: on-demand step=560128043 pc=0x0202A8F8 word=0xE1D020B2 state=thumb sticky=1 -> 0x0202A8FA
smc: on-demand step=560128044 pc=0x0202A8FA word=0x0004E1D0 state=thumb sticky=1 -> 0x0202AC9E
smc: on-demand step=560128045 pc=0x0202AC9E word=0xD038E506 state=thumb sticky=1 -> 0x0202AEAE
smc: on-demand step=560128046 pc=0x0202AEAE word=0xCF4EE180 state=thumb sticky=1 -> 0x0202B1B2
smc: on-demand step=560128047 pc=0x0202B1B2 word=0x0005E1A0 state=thumb sticky=1 -> 0x0202B4F6
smc: on-demand step=560128048 pc=0x0202B4F6 word=0x10001183 state=thumb sticky=1 -> 0x0202B4F8
```

## 4. The fix — the state of a fall-through is the predecessor's stride

`tools/nnk_smc.inc`:

* new `smc_state_from_stride(pc, prev_pc)`, consulted **before** the sticky
  guess in *both* ladders (`nnk_smc_try`, `nnk_smc_exec_any`):
  * if `smc_t_sticky_step + 1 == steps`, the sticky state is FRESH (the
    instruction that just ran was itself live-decoded) → use it;
  * else `pc == prev_pc + 2` → Thumb, `pc == prev_pc + 4` → ARM unless the
    predecessor is a known Thumb case (a 32-bit Thumb-2 instruction);
  * else no opinion (`-1`) → the old sticky fallback.
  Both hooks snapshot `uint32_t prev_pc = last_pc;` **before** `trace_pc(pc)`
  (trace_pc is what advances `last_pc`).
* `smc_t_sticky_step = steps;` next to both `smc_t_sticky = thumb;`
  assignments, so "fresh" is decidable.

This is the same 2-byte/4-byte rule `tools/split_dispatch.py` already uses to
build `nnk_pc_is_thumb` from the emitted fall-throughs, applied locally instead
of from the compiled case set.

Post-fix trace (`work/phase5ao/tr8.out`) at the same site:

```
0x0202A8F8 -> 0x0202A8FC 0x0202A900 0x0202A904 0x0202A908 0x0202A90C -> 0x0202C1A0
```

i.e. the ARM reading, and `bx lr` returns to **0x0202C1A0** — the caller the
halted register file had in `r14`.

Class-50 test: `nnktool/src/decoder_regression_tests.rs`
`phase_5ao_unrooted_fallthrough_state_comes_from_the_stride_not_stale_sticky`
pins the encodings (`0x212FFF1E` `bxcs lr`, `0xE1D020B2` `ldrh r2,[r0,#2]`,
`0xE12FFF1E` `bx lr`), both strides (A8F8+4 = A8FC fixed, A8F8+2 = A8FA bug),
and the source invariants (the stride rule exists, both ladders call it before
`smc_t_sticky`, both sticky assignments stamp the step, both hooks snapshot
`prev_pc`).  `cargo test` = **54 passed**.

## 5. melonDS cross-check — it is ARM there, and it returns to 0x0202C1A0

Same operator session, melonDS 1.1 with the StreamProbe
(`work/phase5ao/md_probe.txt`, `MELONDS_STREAMPROBE_PC=0x0202A8F8,0x0202A8F4,
0x02043C00,0x02043C04`, 1200 frames):

```
sp hit=1 pc=0x0202A8F4 ts=56471321 r0=0x022919A4 r1=0 lr=0x0202A948
sp hit=2 pc=0x0202A8F8 ts=56471321 cpsr=0x8000001F ... sp=0x027E38E8 lr=0x0202A948
sp hit=8 pc=0x0202A8F8 ts=56479704 cpsr=0x8000001F ... sp=0x027E3A00 lr=0x0202C1A0
sp hit=9 pc=0x02043C00 ts=56479926 lr=0x02043C00
sp hit=10 pc=0x02043C04 ts=56479927 lr=0x02043C00
```

`cpsr=0x8000001F` has **T clear** → melonDS executes 0x0202A8F8 in **ARM**,
and hit 8's `lr=0x0202C1A0` is exactly the return address our fixed run
reaches.  melonDS never jumps into `0x0213Dxxx`; the data execution was our
state-inference bug, not a guest behaviour.  (melonDS also executes the pc of
the *next* barrier, 0x02043C04, normally with `lr=0x02043C00`.)

## 6. The new frontier (honest, not investigated)

```
done steps=560192510 halt_pc=0x02043C04 halt=dispatch to untranslated pc
untranslated pc=0x00000000
halt state: r0=022CCDB8 r1=0 r2=0 r3=0 r4=0 r5=0000FFFF r6=000007E4
            r7=000007E0 r13=00000000 r14=02043C01 nzcv=0010
```

`0x02043C04` is `ldmfd sp!,{pc}` (0xE8BD8000) in a virtual-dispatch stub
(`ldr r2,[r0]; ldr r2,[r2,#20]; blx r2; add sp,sp,#12; ldmfd sp!,{pc}` at
0x02043BF0..C04) that the guest re-enters from the ov17 callback chain
0x020E3394 → 0x020E33C8.  The `pop`/`ldmfd` popped 0 because **sp was
0xFFFFFFFC** (`r13=0` after the write-back), i.e. the frame was already broken
before 0x02043C00 — a stack-pointer corruption, a *different* class from this
phase's state bug, and it is 64,352 steps *further* than 5AN's barrier.
Trace: `work/phase5ao/tr6.out` (last 210 pcs), `NNK_MODE_TRACE=1` run
`work/phase5ao/tr7.err` shows no mode switch near the halt (last one at step
560,187,031), so the zero sp came from a decoded store, not a bank switch.

The "Skip the cutscene? Yes/No" dialog is still not reached (GOAL 5 not
attempted: budget spent on the frontier).

## 7. Regression checks

| check | measurement |
|---|---|
| trace identity pre/post fix | the pc traces of the same window are **identical for the first 1046 steps** and diverge exactly at `0x0202A8F8` (tr4.out vs tr8.out) |
| movie band, unscripted (`NNK_STEP_LIMIT=2400000000`) | 4291 frames presented, 171 captures every 25 frames, **all byte-identical** (luma MAE 0.000, corr 1.0000 where not constant) to the Phase-5AN build's captures, f4125..f4275 included |
| notice screen | fix2 frame 100 bottom: pirate art + "We do not condone piracy" (PNG, byte-identical to 5AN) |
| Mobiclip splash | fix2 frame 500 top: "mobiclip VIDEO CODEC" (PNG, byte-identical to 5AN) |
| boot order | fix2 frames 300/700/900 byte-identical to 5AN |
| ROM-only | every run above used `./work/split/arm9_5ao "Ni No Kuni.nds"` with no derived files (`smc: generation-0 ... derived from the ROM`, `io: card rom 536870912 bytes attached from argv`) |
| speed | 200M steps: **32.61 s** (this build) vs **31.99 s** (5AN build, same box) = +1.9 %, inside the 20 % band |
| `cargo test` | **54 passed** (53 + class 50) |
| `verify_phase2a.sh` | strict common prefix **1000 / 1000**, window-filtered 1000/1000, resync 1000/1000, rc=0 |

## 8. Evidence

```
work/phase5ao/tr4.out            pre-fix pc trace 560127000..560128158 (the data jump)
work/phase5ao/tr5.err            NNK_SMC_TRACE: 0x0202A8F8 decoded state=thumb sticky=1
work/phase5ao/tr8.out            post-fix pc trace: 0x0202A8F8 -> 0x0202A8FC (ARM) -> 0x0202C1A0
work/phase5ao/prefix_pre/post.txt the identical 1046-step prefix
work/phase5ao/fix1.err           post-fix run: halt 560192510 at 0x02043C04
work/phase5ao/tr6.out / tr7.err   the new frontier's trace / mode trace
work/phase5ao/md_probe.txt       melonDS: 0x0202A8F8 ARM (cpsr T clear), lr=0x0202C1A0
work/phase5ao/bbshots/           unscripted 2.4e9-step run, 342 captures (every 25)
work/phase5ao/fix2.*.ppm(.png)   operator-session shots (100/500/700/900/1000)
work/phase5ao/verify2a.log       verify_phase2a: 1000/1000, rc=0
work/phase5ao/speed_*.err        200M-step wall clock, both builds
```

No game bytes were committed; the only guest constants in the test are the
three instruction encodings quoted above and their measured addresses.
