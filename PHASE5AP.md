# Phase-5AP — a ROM-derived `pc -> form` map as the state authority (class 51)

Follows Phase-5AO (merged as master `4761bf3`).  5AO left the guest at step
**560,192,510**, pc **0x02043C04** (`ldmfd sp!,{pc}`), `halt = dispatch to
untranslated pc 0x00000000`, **sp = 0xFFFFFFFC**.

5AP does three things: it builds the map that answers "which processor state
does this pc execute in?" from the ROM image instead of from a local guess,
it wires that map into both ladders with a conflict/hole detector, and with it
the 560,192,510 barrier is gone: the guest runs to **step 561,438,881**
(class 51 fixed, see §5/§6).

## 1. The map (GOAL 1)

Generator (committed): `tools/gen_pcform.py`
Engine    (committed): `nnktool/src/pcform.rs`, subcommand `nnktool pcform`
Artifact  (never committed, no game bytes): `work/phase5ap/nnk_pcform_map.h`
Summary   of the entry list and sizes:      `work/phase5ap/pcform_summary.json`
Manifest  the generator fed the engine:     `work/phase5ap/pcform.manifest`

`gen_pcform.py` reads the `.nds` **alone**, decompresses the ARM9 image
(Nintendo code compression via ndspy) and all 21 ARM9 overlays named by the
OVT, and writes them under `work/phase5ap/img/`.  The seeds are the ROM's own
entry points:

| image | seed | form |
|---|---|---|
| arm9 | header entry `0x02000800` | ARM |
| each overlay (21) | its OVT ram base | from the module layout |
| each overlay (21) | every `sinit`/ctor pointer in `[init_start, init_end)` | bit0 of the pointer (the `pop {..,pc}` / `blx rM` interworking rule) |

Then `nnktool pcform` walks each image with the recompiler's own CFG recovery
(`recover.rs`: direct branches, `bx`/`blx`, `pop {..,pc}`/`ldm` into pc,
ARM `mov pc,lr`, veneers, and `expand_jump_table` for `ldr pc,[pc,#imm]` /
`add pc,pc,rN,lsl#2` tables), and each round adds **every 32-bit word in every
image that targets code not covered yet**, with bit0 selecting the state —
i.e. the route the guest really uses for overlay exports and vtables.  Runs
that collide are clipped first-claim-wins and counted (`ambig`), so the emitted
list is a strict function: disjoint `[lo,hi)` runs, Thumb and ARM separately.

Current artifact: **22 images, 12,569 seeds, 2 rounds, 4,565 runs
(1,253 Thumb / 3,312 ARM), 109,166 Thumb bytes, 821,750 ARM bytes,
19 ambiguous, 12,097,588 clipped bytes.**

Both implementations consume it:

* **runtime** — `tools/nnk_smc.inc` includes `nnk_pcform_map.h` and resolves an
  unclassified pc through `smc_pick_state()`: the map decides when it covers
  the pc, the runtime inference (branch register → predecessor stride → sticky,
  5Z/5C/5AO) is computed anyway and only *reported* against it.  Consult:
  `pf = smc_map_live_ok(pc) ? nnk_pcform_lookup(pc) : -1;`
* **kind inference** — `nnktool/src/recompile.rs` carries the identical class
  fix for the emitted `nnk_state` markers (the same descent), and the map is
  the cross-check artifact the conflict run is compared against.

**`smc_map_live_ok()` is the honesty rule that makes this safe:** the map
describes the ROM image, so it only speaks for bands whose generation counter
is still 0.  The arena window is decrypted in place by the guest's own
rolling-key routine (0x0215B468, ten passes), so generation-0 bytes there are
*ciphertext*: decoding them yields a confident but meaningless form.  Measured
without the rule: the map forced Thumb at 0x0215A7C4 (a decrypted ov17 routine)
and the run halted at **step 13,643,665** instead of 560,192,510.

## 2. The conflict report (GOAL 2)

`NNK_STATE_CONFLICTS=1` reports, per run:

```
stateconf MAP pc=... map=thumb runtime=arm(predecessor-stride) step=...
stateconf FLIP pc=... thumb->arm step=...        (a pc executed in BOTH forms)
stateconf FALLBACK pc=... chose=arm decoded_as=thumb step=...  (wrong form, retried)
stateconf: map_decisions=N conflicts=... flips=... hole_executions=...
           hole_pcs=... live_band_skips=...
stateconf: hole pc=0x...            (bounded list, 64 entries)
stateconf: live-band pc=0x... (guest-rewritten, map skipped)
```

600M-step target run (operator session, `NNK_STEP_LIMIT=6e8`), measured at the
561,438,881 frontier:

```
smc: probes=550028933 stale=218898 redecoded=3091472 halts=0
     state_fallbacks=6 on_demand=2872574
stateconf: map_decisions=0 conflicts=6 flips=0 hole_executions=0 hole_pcs=0
           live_band_skips=2977792
```

**flips = 0** (no address executed in both Thumb and ARM form).  The 6
conflicts are all `FALLBACK` at live-decoded pcs inside guest-rewritten bands
(0x020DDE54 ×3, 0x0211068E, 0x02004DC6, 0x02004E82) — the map cannot speak
there by construction, and the old inference recovered by retrying the other
decoder.  They are the honest residue, not silent wins.

**Why `map_decisions=0`:** every executed pc is either covered by the compiled
map (exact, `nnk_pc_is_thumb`) or lives in a guest-rewritten band.  Since the
compiled image is a full recompilation of arm9, live decoding happens only in
the overlays/decrypted arena, which is exactly where the map is invalid.  The
map's authority therefore applies to arm9/overlay *generation-0* pcs — and it
is proven by the negative control in §1 (map without `smc_map_live_ok()`:
halt at 13,643,665).

Design decision recorded here rather than silently: a pc→form map keyed by
address alone cannot describe overlays that share an arena (4 arenas, 21
modules) or guest-decrypted bytes; both are handled explicitly (first-claim
wins + the generation test) and both are visible in the counters.

## 3. GOAL 3 — the 560,192,510 frontier: a Thumb `blx` return tagged as ARM

Measured chain (operator session, `NNK_DEBUG_AT_PC=0x02043BF0`):

```
step 560192469  entry 0x02043BF0  sp=0x027E39D0  lr=0x020E33B1   <- healthy
step 560192497  entry 0x02043BF0  sp=0xFFFFFFF0  lr=0x020E33CD   <- corrupted
```

ov17's Thumb function at 0x020E3328 calls the ARM stub with an interworking
call, and the stub returns with `ldmfd sp!,{pc}`:

```
0x020E3390  f760 ec1e    blx 0x02043BD0        <- Thumb -> ARM, LR = 0x020E3395
0x02043BD0  e92d4000     push {lr}             <- saves 0x020E3395 (bit0 SET)
0x02043BF0  e5902000     ldr  r2,[r0]          <- vtable dispatch
0x02043C00  e28dd00c     add  sp,sp,#12
0x02043C04  e8bd8000     ldmfd sp!,{pc}        <- returns to the Thumb caller
```

The emitted case for 0x02043C04 handed the runtime `_t & ~1u`:

```c
uint32_t _t = ld32(_a + 0u);
pc = _t & ~1u;
nnk_smc_note_branch(_t & ~1u);      /* <-- the state is destroyed here */
```

so the state note said **ARM** and 0x020E3394 (Thumb `adds r1,r4,#0`) was
re-decoded as the ARM word `0x1C221C21` = `stmiapl sl!,{r0,r3,r5,r6,fp,ip,lr}`,
which *stored* over the frame (`stmiapl` is conditional on PL and writes back
sl): sp went 0x027E39D0 → 0xFFFFFFF0, the next `ldmfd sp!,{pc}` popped 0 and
the run halted with `dispatch to untranslated pc 0x00000000`.

`ldmfd sp!,{pc}` is the A_POP form: the loaded word **is** the hardware's
interworking target, bit0 included (a Thumb `blx` leaves `LR = ret | 1`).  The
Phase-5J forced tag (melonDS `if (Num==1) pc &= ~0x1`) belongs to the *non-SP*
single-register LDM form.

### The fix (class 51)

* `nnktool/src/recompile.rs` (canonical): the forced tag now applies only when
  `regs.len() == 1 && *rn != 13`; the pop/ldm-into-pc path hands the RAW word.
* `tools/p5ap_pop_note_splice.py` (build splice, wired into
  `tools/build_split.sh` behind the `5ap_pop_note_interwork` manifest row):
  applies the identical transform to the frozen image — **25 SP-base notes
  fixed, 0 kept masked** — so the documented build command reproduces it.
* `tools/nnk_smc.inc` (live decoder, same rule):
  `raw = (cnt == 1u && rn != 13u) ? (lv & ~1u) : lv;`

## 4. The new frontier (honest, not investigated)

```
done steps=561438881 traced=561438881 distinct_pcs=24732 hle_hits=8136
halt_pc=0x02062C48 halt=undefined instruction 0xF94AF000
halt state: r0=00000000 r1=80000000 r2=00000000 r3=80000000 r4=00004000
            r5=00000033 r6=0000001D r7=FFFFFFFF r13=027E3A04 r14=02002088
```

`0x02062C48` is inside the arm9 image and is a valid **Thumb** instruction —
`f000 f94a` = `bl` — but the compiled case for it carries no decode
(`/* 0x02062C48: */`) and traps with the raw word, and the ROM map is a **hole**
there: the enclosing function is not reachable from any ROM-declared entry or
pointer (its callers hold the address in runtime-built structures).  So the
next barrier is the *same family* with the map's coverage, not its authority,
as the gap: arm9 has statically unreachable functions that the compiled image
therefore did not root either.  The next step is to widen the seed set (Ghidra
function entries for arm9, which are a static analysis of the same image) and
re-run — the conflict report already shows where the holes are.

## 5. Regression checks

| check | measurement |
|---|---|
| `cargo test` | **55 passed** (54 + class 51 `phase_5ap_thumb_blx_return_keeps_its_state_in_the_arm_pop_note`) |
| 560,192,510 barrier | cleared: the same operator session now runs to the 561,438,881 step's next barrier |
| conflict run (6e8 steps) | flips **0**, map-vs-inference conflicts **0**, 6 FALLBACKs in guest-rewritten bands, 0 hole executions, 2,977,792 live-band skips |
| speed | 200M steps: **33.40 s** vs 5AN's 29.40 s = **+13.6 %** (inside the 20 % band) |
| ROM-only | every run used `./work/split/arm9_5ap "Ni No Kuni.nds"`; `smc: generation-0 arm9 ... derived from the ROM`, `io: card rom ... attached from argv` |
| negative control (map honesty) | with `smc_map_live_ok()` bypassed the map forces Thumb at 0x0215A7C4 and the run dies at step 13,643,665 — the generation test is what makes the map safe |
| map determinism | the generator is a pure function of the ROM: 22 images, same run counts on regeneration |

## 6. Evidence

```
work/phase5ap/nnk_pcform_map.h          the artifact (4,565 runs)
work/phase5ap/pcform.manifest           images + 60 ROM-declared seeds
work/phase5ap/pcform_summary.json       entry list, sizes
work/phase5ap/ev/run560m_op.err         5AO frontier reproduced (halt 560192510)
work/phase5ap/ev/run560m_op2.err        after class 51: 560,192,510 passed
work/phase5ap/ev/run1g.err              561,438,881 frontier + conflict report
work/phase5ap/ev/dbg_43bf0.err          sp 0x027E39D0 -> 0xFFFFFFF0 at 0x02043BF0
work/phase5ap/ev/tr_halt.out            pc trace across the barrier
work/phase5ap/ev/verify2a.log           verify_phase2a
work/phase5ap/ev/fr.*.ppm(.png)         frame captures from the final run
```

No game bytes were committed; the tests quote only single instruction
encodings and the measured addresses above.
