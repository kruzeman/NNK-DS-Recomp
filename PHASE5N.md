# Phase-5N — barrier I2: the "empty IPC vector slot" is a zeroed ITCM literal pool

Follows Phase-5M (merged `45496f0`).  Phase-5M named barrier I2 as "`bx r0` at
`0x01FF804C` with an EMPTY IRQ vector slot `0x027E0048`" and listed two
candidate roots (A: our IPC model raises an IRQ the game unmasked away from;
B: the slot is 0 in melonDS too and the missing piece is the ARM9 BIOS).

**This phase falsifies BOTH candidates by measurement and re-roots the
barrier.**  The DTCM vector slot is *not* empty in native — it holds exactly
the handler melonDS holds (`0x0200BAE8`).  What is zero is the dispatcher's
**table base**: the ITCM literal pool at `0x01FF8050`/`0x01FF8054` has been
overwritten with zeros, so `ldr r1,[pc,#8]` loads 0 instead of `0x027E0000`,
and `ldr r0,[r1,r0,lsl #2]` then reads `ITCM[0x48]` (also 0) → `bx 0`.

The corruption is a **stray 32-bit store to guest address `0x00000051`**
(`0x00000055` on the next iteration) from the movie player's math-table
generator at `0x020545B8`: its 5th argument — the destination pointer — is
`0x00000051`, a garbage word, not an address.  Because the ARM9 ITCM window is
`addr < itcm_size` (`0x02000000`, melonDS's own rule), address `0x51` aliases
onto `ITCM[0x51]` and zeroes the two literals.  The follow-up barrier is the
garbage pointer itself (named in §6).

**Headline**

1. **The named slot is populated in BOTH engines.**  Native, whole-run
   store-watch on `0x027E0040..0x027E0050`: slot 18 is written exactly three
   times — step 127 (crt0 clear), step 3575678 (table fill), step 3628349
   (`0x02005F48`, `r1 = 0x0200BAE8`, i.e. the IPC handler installed) — and
   never again.  melonDS: DTCM dump at the dispatcher shows
   `0x027E0048 = 0x0200BAE8` from ts 6.1M to the end of an 85 s run.  The
   Phase-5M "empty slot" reading was a *symptom*: the failing dispatch's
   `ldr r0,[r1,r0,lsl #2]` used `r1 = 0`.
2. **The halt state proves the table base is 0.**  `halt state: r0=00000000
   r1=00000000 r2=000C0000 r3=80000000 r14=00000000`.  `r2 = IE&IF` and
   `r3 = 0x80000000` are exactly what `0x01FF8018..0x01FF8024` produce, so the
   dispatcher ran; but `r1` must be `0x027E0000` and `lr` must be `0x01FF8058`
   after `0x01FF8040`/`0x01FF8048`.  Both are 0? the two literals they load
   read 0.
3. **Who zeroed them, measured.**  `NNK_STOREWATCH="0x50:0x58"` with
   `NNK_STOREWATCH_MASK=0x7FFF` (new instrumentation, §5) logs every store
   that lands in the ITCM array regardless of the guest-side alias:
   ```
   W32 0x01FF8050 old=0x00000000 pc=0x02000A2C step=3574072   (crt0 copy)
   W32 0x01FF8054 old=0x00000000 pc=0x02000A2C step=3574076   (crt0 copy)
   W32 0x00000051 old=0x58027E00 pc=0x020547D4 r0=0 r1=1 r2=0x20 r3=0x3FE00000 sp=0x027E3968 step=2256701818
   W32 0x00000055 old=0x4801FF80 pc=0x020547D4 r0=0 r1=1 r2=0x20 r3=0x3FE00000 sp=0x027E3968 step=2256701942
   ```
   The last two are 32-bit stores with value 0 at raw addresses `0x51`/`0x55`,
   i.e. bytes `ITCM[0x51..0x54]` and `ITCM[0x55..0x58]` — the bytes of the two
   literals (`00 00 7E 02 58 80 FF 01`).  That is the whole mechanism.
4. **The store is `str r0,[r1,r7,lsl #2]` at `0x020547D4`** in the table
   generator at `0x020545B8`; `r1` is loaded from `[sp,#0x128]`, the function's
   5th (stack-passed) argument, and is `0x00000051`.  `r7 = 0,1` gives the two
   addresses.  melonDS never executes `0x020547D4` at all (§4) and its ITCM
   literals stay intact to the end of a longer run, so the argument — not the
   store model — is the divergence.
5. The halt step is unchanged (`2,256,702,439`, `distinct_pcs = 22,704`,
   4035 frames): this phase did **not** move the run past barrier I2.  It
   replaces a guessed barrier description with the measured one and names the
   real next barrier (I3, §6).

## 1. The Phase-5M discriminator, run

The doc asked for "melonDS's DTCM word at `0x027E0048` plus its IF at the same
resume PC (`0x020255D0`)", or an IRQ trace.  The same information is obtained
more directly by probing the dispatcher itself, because `r0` at `0x01FF804C`
*is* the table read the dispatcher makes:

```sh
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
    SDL_AUDIODRIVER=dummy \
    MELONDS_REGPROBE_PATH=$S/mel/rp_dispatcher.txt \
    MELONDS_REGPROBE_PCS=0x01FF804C MELONDS_REGPROBE_N=200000 \
    MELONDS_REGPROBE_MEM="0x027E0040:0x20,0x04000208:0x14" \
    MELONDS_REGPROBE_MEM_PATH=$S/mel/rp_mem.txt \
    timeout 85 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
```

* 4,266 dispatches in 85 s (`r2 = IE&IF` reaches `0x002C0000`, i.e. bits
  18/19/21, and `r0` is a real handler every time).
* DTCM `0x027E0040..0x027E005F`, first and last hit, identical:
  ```
  0x027E0040: C4 5D 00 02 C4 5D 00 02 E8 BA 00 02 C4 5D 00 02
  0x027E0050: C4 5D 00 02 C4 5D 00 02 00 00 00 00 00 00 00 00
  ```
  slot 16/17 = `0x02005DC4`, **slot 18 = `0x0200BAE8`**, slot 19 =
  `0x02005DC4` → later `0x02016BC8`, slots 22/23 = 0.
* The dispatcher services the **lowest** pending+enabled bit (the
  `clz`/`bics` loop at `0x01FF8028..0x01FF8030`, not the highest): with
  `IE&IF = 0x00040000` that is slot 18 — whose melonDS value is nonzero.

**Candidate B is falsified: melonDS's slot 18 is populated, not empty.**

The operand `0x04000208` dump reads all zeros in melonDS (the 8-bit data path
does not return IE/IF), so `r2` (the register the dispatcher computed from
`IE&IF`) is the authoritative value; it matches native's `IE&IF` exactly.

## 2. Native: the slot was never cleared

New `NNK_STOREWATCH` (§5) on the DTCM table range, whole 2.2567G-step run:

```
W32 0x027E0048 old=0x00000000 pc=0x02000944 step=127       (crt0 zero-fill loop)
W32 0x027E0048 old=0x00000000 pc=0x02000A2C step=3575678   (SDK table copy)
W32 0x027E0048 old=0x02005DC4 pc=0x02005F48 r1=0x0200BAE8 step=3628349  (IPC handler install)
```

Then nothing for the remaining 2.2563G steps.  `0x02005EE8` is the SDK
handler install/clear routine (`str r1,[r4,r8,lsl #2]` at `0x02005F48`,
`r4 = 0x027E0000`, one slot per bit set in `r0`, `r1 = 0` = clear).
melonDS executes it 7 times up to ts 51.7M with masks `0x00040000, 8, 1, 1,
0x00080000, 1, 1` — bit 18 only in the first call, whose `r1` is
`0x0200BAE8` (an install, not a clear), from `lr = 0x0200B970`.

So both engines hold `0x0200BAE8` in DTCM `0x027E0048` at native's halt step.

## 3. What is actually 0 in native

```
halt state: r0=00000000 r1=00000000 r2=000C0000 r3=80000000 r4=40E00000
             r5=00000000 r6=027E3970 r7=00000019 r13=027E3F60 r14=00000000
untranslated pc=0x00000000
```

```
1ff8038: str  r1, [ip, #4]   ; ack IF          r1 = 0x00040000 (single bit)
1ff803c: rsbs r0, r0, #31    ; bit index       r0 = 18
1ff8040: ldr  r1, [pc, #8]   ; <- 0x01FF8050   r1 = 0  (was 0x027E0000)
1ff8044: ldr  r0, [r1, r0, lsl #2]             r0 = ld32(0x48) = ITCM[0x48] = 0
1ff8048: ldr  lr, [pc, #4]   ; <- 0x01FF8054   lr = 0  (was 0x01FF8058)
1ff804c: bx   r0                               -> PC 0
```

`r2` (= `IE&IF`) and `r3` (= `0x80000000`) are precisely the values the
preceding dispatcher instructions produce, and `r0 = 18` is the correct slot
index; only the two **PC-relative literal loads** came back 0.  The compiled
cases for these PCs are correct (`nnk_part_itcm.c`: `_b = pc + 8; r[1] =
ld32(_b + 8)`), and the loads go through the live ITCM (`ld32` → `itcm[a &
0x7FFF]`).  So the *memory*, not the code, is wrong.

This is why the case is not a decode class: it is a guest-store side effect
that native and melonDS disagree about only because the *argument* differs.

## 4. melonDS does not take that path

```sh
env ... MELONDS_ARM9TRACE_PATH=$S/mel/t547b.txt \
    MELONDS_ARM9TRACE_LO=0x020547D4 MELONDS_ARM9TRACE_HI=0x020547D8 \
    MELONDS_ARM9TRACE_N=100000 MELONDS_ARM9TRACE_EXIT=0 \
    MELONDS_REGPROBE_PATH=$S/mel/r547b.txt \
    MELONDS_REGPROBE_PCS=0x020547D4,0x01FF804C MELONDS_REGPROBE_N=60 \
    MELONDS_REGPROBE_MEM="0x01FF8050:0x8,0x027E3A90:0x10" \
    MELONDS_REGPROBE_MEM_PATH=$S/mel/m547b.txt \
    timeout 230 xvfb-run -a .../melonDS "Ni No Kuni.nds"
```

* `grep -c 0x020547d4 t547b.txt` → **0** over the whole 230 s run (the 85 s
  probe already reached ts 2.6e9; 230 s is far past native's halt point in
  emulated time).
* The last dispatcher probe still shows the literals intact:
  `0x01FF8050: 00 00 7E 02 58 80 FF 01` with `r1 = 0x027E0000`, and the DTCM
  stack slot `0x027E3A90` holds real pointers (`0x021485A8 0x02145FAC
  0x02143158 0x0A`), not `0x51`.

So native takes a movie-player init path melonDS does not take, or takes it
with a pointer melonDS does not have.  Either way the divergence is upstream
of the store.

## 5. New instrumentation (committed)

`tools/nnk_smc.inc`, inside `smc_note()` (the hook every `st8/st16/st32` calls):

| env | meaning |
|---|---|
| `NNK_STOREWATCH="lo:hi"` | watch range of guest store addresses |
| `NNK_STOREWATCH_MASK=<m>` | test `addr & m` (plus `addr < itcm_size`, i.e. only stores that really route into ITCM); `0x7FFF` turns the watch into "any alias of ITCM offsets lo..hi" |
| `NNK_STOREWATCH_PATH=<file>` | output (one line per hit) |
| `NNK_STOREWATCH_N=<n>` | hit cap (default 2000) |

Each line: width, RAW guest address, the value the store replaces (read
through the guest data path, so DTCM/ITCM windows are honoured), the executing
PC, `lr`, `r0..r3`, `sp` and the step index.  Cost when unset: one integer
test per guest store — measured 4.6M steps/s idle-watch vs 4.6M steps/s with
the watch armed and silent (`p5n3`, `p5n5` runs above).

## 6. Result and the next barrier (I3, named)

Nothing in the run moved: the same halt, the same step count.

**Barrier I3 (named): the table generator's destination argument is garbage.**

`0x020545B8` (`push {r3-r9,sl,fp,lr}; sub sp,#256`) loads its 5th
(stack-passed) argument into `[sp,#0x128]` and stores generated table words to
it:
```
20545d0: ldr   r1, [sp, #296]      ; dst = 5th argument  (native: 0x00000051)
20547a4: add   r0, r6, r7, lsl #3
20547a8: ldm   r0, {r2, r3}
20547b4: bl    0x02025C88
20547c8: bl    0x020255A4
20547cc: bl    0x02026B28
20547d0: ldr   r1, [sp, #296]
20547d4: str   r0, [r1, r7, lsl #2]   ; <- native: 0x51, 0x55 (ITCM!); melonDS: never executed
```
The caller's return address is still on the frame at `[sp,#0x124]`
(`push` saves `lr` at `entry_sp-4 = sp+0x124`), so the next step is:
**run native with `NNK_DEBUG_AT_PC=0x020547D4 NNK_DEBUG_MEM="0x027E3A70:0x40"`
and read `[0x027E3A8C]`** to name the caller, then follow the pointer to its
producer.  (The probe command is ready; the 2.26G-step run it needs was not
completed inside this phase's budget — see §5 for the measured speed.)

Also worth checking there: whether melonDS reaches `0x020545B8` at all — it
does not reach the store, so either the caller is not invoked or it is invoked
with a different argument.

Note for whoever continues: the ITCM alias rule itself is *correct* (melonDS's
`addr < ITCMSize` → `ITCM[addr & 0x7FFF]`); a real ARM9 with this CP15 setup
would also scribble on `ITCM[0x51]`.  Do **not** "fix" this by masking or
dropping unaligned ITCM stores, and do **not** model `bx 0` as a no-op.

## 7. Acceptance checks on this branch

```
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh     # branch code, worktree
RESULT: PASS (rc=0) — strict prefix 1000/1000
$ cd nnktool && cargo test
test result: ok. 31 passed; 0 failed
```

No decode class was added: the measured mechanism is a guest store through a
garbage pointer, not an instruction-encoding gap.

## 8. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5n
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5k.c SPLIT_DIR=$S/split \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5n5

# 1) who writes the DTCM IRQ table (native)
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2600000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_STOREWATCH="0x027E0040:0x027E0050" NNK_STOREWATCH_PATH=$S/f/sw4.log \
    /home/hermes/NiNoKuni/out/split/p5n5 work/phase3a/arm9_stored_padded.bin

# 2) who writes the ITCM literal pool (native, alias-masked)
env ... NNK_STOREWATCH="0x50:0x58" NNK_STOREWATCH_MASK=0x7FFF \
    NNK_STOREWATCH_PATH=$S/f/itcmw.log ... p5n5 ...

# 3) melonDS: dispatcher table + slot 18, and the store's PC
env ... MELONDS_REGPROBE_PCS=0x01FF804C MELONDS_REGPROBE_N=200000 \
    MELONDS_REGPROBE_MEM="0x027E0040:0x20" MELONDS_REGPROBE_MEM_PATH=... \
    MELONDS_ARM9TRACE_PATH=... MELONDS_ARM9TRACE_LO=0x020547D4 \
    MELONDS_ARM9TRACE_HI=0x020547D8 MELONDS_ARM9TRACE_N=100000 \
    MELONDS_ARM9TRACE_EXIT=0 timeout 230 xvfb-run -a .../melonDS "Ni No Kuni.nds"
```
