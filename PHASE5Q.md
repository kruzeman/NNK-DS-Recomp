# Phase-5Q — barrier I5 executed: the ITCM decode loop is NOT mis-decoded

Follows Phase-5P (merged `7682156`), which localized the corrupted movie frames
to the *decode* and prescribed this phase's measurement: a PC-triggered ITCM
dump at the Mobiclip callback `0x01FF8540` plus a form audit of the
live-re-decoded ITCM code.  This phase ran that measurement in **both**
engines and falsifies the barrier as stated: the decoder blob is
byte-identical, its executed control flow is identical, and the callback's
registers are identical.  The first real divergence is a **0x8A fill of
plane-sized buffers** that native performs and melonDS does not, measured
word-exactly, one stage earlier than Phase-5P could look.

**Headline**

1. **The prescribed measurement now exists and is reproducible.**
   `tools/p5q_itcm_dump_at_pc.py` splices a PC-triggered ITCM/RAM dump into a
   frozen image (`nnktool/src/recompile.rs` carries the emitter source of
   truth), and melonDS's `ITCMDump` gained the same trigger — including the
   **Thumb branch**, which never sampled ITCM before:

   ```
   native : NNK_ITCM_DUMP_AT_PC=<hex> NNK_ITCM_DUMP_AT_PC_N=<n>
            NNK_ITCM_DUMP_AT_PC_PATH=<file> NNK_ITCM_DUMP_AT_PC_MEM=<file>
            NNK_TRACE_AT_PC=1 NNK_HALT_AFTER_TRIGGER=<n>
   melonDS: MELONDS_ITCM_DUMP_PC=<hex> MELONDS_ITCM_DUMP_PC_N=<n>
            MELONDS_ITCM_DUMP_PC_PATH=<file>
            MELONDS_ITCM_DUMP_PC_RAMDUMP=<file>
   ```

2. **The callback is an ARM trampoline, not the decoder body.**
   Both engines execute `0x01FF8540` in ARM state (melonDS mode trace
   `thumb=0`, CPSR `0x6000001F`), and the first word there is
   `0xEA0005BA` = `b 0x01FF9C30`; the real decoder runs
   `0x01FF9C30..0x01FFDB00`.  The 32 KiB ITCM blobs captured at the callback
   entry are **byte-identical, 0 / 32,768 differing bytes**.

3. **The form audit: every form the loop executes is implemented correctly.**
   melonDS's trace from the callback contains **977 distinct ITCM PCs**; the
   decode loop's hot body is

   ```
   0x01FFCF34  ldm  r0, {r3,r4,r7,r8}      0xE8900198   (non-contiguous list)
   0x01FFCF38  add  r0, r0, #256           0xE2800C01
   0x01FFCF3C  stm  r1, {r3,r4,r7,r8}      0xE8810198
   0x01FFCF40  add  r1, r1, #256           0xE2811C01
   0x01FFCF44  subs r2, r2, #1             0xE2522001
   0x01FFCF48  bne  0x01FFCF34             0x1AFFFFF9
   ```

   and the decoder body uses post-indexed byte/word transfers with a `#256`
   offset (`0xE4DB4100 ldrb r4,[fp],#0x100`, `0xE48B6100 str r6,[fp],#0x100`),
   register-shifted loads (`0xE795610C ldr r6,[r5,ip,lsl #2]`),
   register-specified shifts (`0xE1A03A13 mov r3,r3,lsl sl`), computed PC
   writes (`0xE086F005/0xE086F007 add pc,r6,r5/r7`), and `mov pc,lr`
   returns.  All of them are implemented in `tools/nnk_smc.inc`
   (`smc_exec_arm`: single-data-transfer post-index at :578-605, LDM/STM at
   :606-652, data-processing incl. `rd==15` at :681-719) with the melonDS
   semantics already locked by the Phase-5D/5J/5O classes.  No executed form
   falls through to the honest-halt path, and none is decoded as another
   instruction class.

4. **Execution-trace equality from the callback entry.**
   Native (`NNK_TRACE_AT_PC=1`, 400,000 steps) vs melonDS
   (`MELONDS_ARM9TRACE_TSLO=2272000000`, ITCM window):
   **193,586 identical in-window ITCM PCs** out of 193,586 compared, with
   the IRQ-vector region (`0x01FF8000..0x01FF83FF`) filtered so that an
   interrupt-timing difference cannot mask a decode difference.  The raw
   sequences first differ at index 143,474 only because native takes an IRQ
   there and melonDS takes it later.

5. **Register equality at the callback and its returns.**  Native and melonDS
   agree word for word on `r0..r14` at the callback entry and at the first
   four returns of the call site (`0x0207B21C`), e.g. return 1:

   ```
   r0=000000DE r1=022E49C8 r2=022E48E8 r3=002FF040 r4=00000004
   r5=02278264 r6=022E8A8C r7=0 r8=0 r12=0 r13=027E3A18 r14=0207B21C
   ```

6. **Main-RAM equality up to the first plane divergence.**  Full 4 MiB
   snapshots at the callback entry and at returns 1..4 (melonDS
   `MELONDS_ITCM_DUMP_PC_RAMDUMP`; native the same trigger).  Excluding the
   known-unmodelled ARM7 image (`0x02380000..0x0240FFFF`, the bulk of the
   diff) the two engines differ in only a few hundred words around
   `0x0206C000..0x0207BFFF` (stack/tables) — and **the whole 6-slot 8-bit
   plane ring is word-identical at returns 1 and 2**:

   ```
   hit 1: ring words 0x02284800..0x022D28FF differing: 0 / 79,936
   hit 2: ring words 0x02284800..0x022D28FF differing: 0 / 79,936
   hit 3: ring words 0x02284800..0x022D28FF differing: 12,288 / 79,936
   ```

   Phase-5P's headline ("the first differing word is the decoded plane,
   0/12,288 words equal") compared *different ring slots at different movie
   positions*; at the same program point the first frames agree exactly.

7. **The first real divergence: an 0x8A fill of plane-sized buffers.**
   Between the callback entry and return 1, native writes `0x8A8A8A8A` over
   `0x0227C000..0x022847FF` (0x8800 bytes, i.e. a whole plane buffer below the
   ring) while melonDS leaves it zero.  At return 3 two more plane-sized runs
   appear:

   ```
   0x0227C000..0x022847FC  4,608 words   0x8A8A8A8A  vs  0x00000000
   0x0228A820..0x0229681C 12,288 words   0x8A8A8A8A  vs  0x00000000   (one full 0xC000-byte plane)
   ```

   `0x8A` (138) is the "flat, over-bright, low-contrast" value Phase-5P
   measured across the native planes, so the corruption is **produced by that
   fill**, not by the inverse transform or the entropy loop.

8. **Store-watch on the fill (see §3 for the result).**
   `NNK_STOREWATCH="0x0227C000:0x0227C004"` names the writer by measurement.

## 1. Result

Barrier I5 as stated (the live-re-decoded ITCM loop's form set) is
**falsified**: identical code, identical control flow over two full decode
callbacks, identical registers.  The corrupted frames come from a plane-sized
`0x8A` fill executed *between* the callback entry and its first return.

**Named follow-up barrier (I6): the intra predictor's DC seed.**  Native fills
`0x0227C000..0x022847FF` and (by return 3) `0x0228A820..0x0229681F` with
`0x8A8A8A8A` because the ITCM intra predictor
(`0x01FFAE84..0x01FFAEDC`, `stm fp,{r6,r7}`) computes a mean of the eight
bytes **above** the block row, and that row is already `0x8A` in native while
it is zero in melonDS — the value therefore self-propagates over the whole
plane.  The next measurement is (a) the first store that puts `0x8A` into the
seed row (`fp-256`, e.g. `0x0227BF00`) and its source operand, and (b) the
container field `0x02278500` (native `0x00000280` vs melonDS `0xFFFFDF80`),
which is the earliest RAM difference at the same program point.

## 2. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5q
python3 tools/p5q_itcm_dump_at_pc.py work/phase3a/arm9_thumb_ovl_itcm_p5o.c \
        work/phase3a/arm9_thumb_ovl_itcm_p5q.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5q.c SPLIT_DIR=$S/split \
  bash tools/build_split.sh $S/p5q1

# native: ITCM + registers + RAM at the decode callback (and at its returns)
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
    NNK_STEP_LIMIT=2300000000 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    NNK_ITCM_DUMP_AT_PC=0x01FF8540 NNK_ITCM_DUMP_AT_PC_PATH=$S/nat/itcm_cb.bin \
    NNK_ITCM_DUMP_AT_PC_MEM=$S/nat/ram_cb.bin \
    $S/p5q1 work/phase3a/arm9_stored_padded.bin

# melonDS: same program point
env XDG_CONFIG_HOME=... SDL_AUDIODRIVER=dummy \
    MELONDS_ITCM_DUMP_PC=0x01FF8540 MELONDS_ITCM_DUMP_PC_PATH=$S/mel/itcm_cb.bin \
    MELONDS_ITCM_DUMP_PC_RAMDUMP=$S/mel/ram_cb.bin \
    MELONDS_ARM9TRACE_PATH=$S/mel/trace_itcm.txt \
    MELONDS_ARM9TRACE_LO=0x01FF8000 MELONDS_ARM9TRACE_HI=0x02000000 \
    MELONDS_ARM9TRACE_N=400000 MELONDS_ARM9TRACE_TSLO=2272000000 \
    MELONDS_ARM9TRACE_TSHI=2290000000 \
    timeout 300 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS 'Ni No Kuni.nds'

cmp -l $S/nat/itcm_cb.bin $S/mel/itcm_cb.bin | wc -l      # -> 0
```

Analysis scripts (scratch, not committed): `p5q/{cmp_traces,cmp2,ramdiff,
ringcmp,census,span,excerpt,audit,ctx}.py`.

## 3. Store-watch on the fill — the writer is the INTRA PREDICTOR

`NNK_STOREWATCH="0x0227C000:0x0227C004"` (40-entry ring, same run recipe)
names every store to the first word of the fill:

```
W32 0x0227C000 old=0x00000000 pc=0x02016B44 ... step=40957403      (boot card read)
W32 0x0227C000 old=0x51894A57 pc=0x02009BB4 lr=0207AB40 r0=0 r1=0227C000 step=2255802354
W32 0x0227C000 old=0x00000000 pc=0x01FFAEDC lr=01FF9F74 r2=5 r3=8FC7E000
                                                     step=2256185493   (inside callback 1)
W32 0x0227C000 old=0x8A8A8A8A pc=0x01FFCF3C lr=01FF863C r0=022D61A0
                                     r1=0227C000 r3=8A8A8A8A step=2262145273
```

`0x01FFAEDC` is the ITCM decoder's **intra predictor**:

```
0x01FFAE84  mov r6,#0
0x01FFAE88  sub r7, fp, #256        ; row ABOVE the current block row
0x01FFAE8C  ldm r7, {r4,r5}         ; 8 luma bytes above
0x01FFAE90..0x01FFAEB8  add r6,r6,rX,lsr #24 ; lsl rX,rX,#8  (x8)
0x01FFAEC8  add r6,r6,#4
0x01FFAECC  asr r6,r6,#3            ; rounded MEAN of the 8 bytes above
0x01FFAED0  orr r6,r6,r6,lsl #8
0x01FFAED4  orr r6,r6,r6,lsl #16    ; replicate the mean byte
0x01FFAED8  mov r7,r6
0x01FFAEDC  stm fp, {r6,r7}         ; 0xE88B00C0: 8-byte DC fill, then fp+=#256 (x8 rows)
```

So the `0x8A8A8A8A` plane is the predictor's DC fill, and it **self-propagates**:
each block row takes the mean of the row above, which the previous iteration
just filled with the same value.  The seed is the first row-above read
(`fp-256`, i.e. `0x0227BF00` for the plane at `0x0227C000`); native has
`0x8A8A8A8A` there and melonDS `0x00000000`, and every plane-sized run in
§0/7 is this one value propagating.

The **earliest** RAM difference at return 1 (scanning upward from
`0x02270000`) is one word *before* the planes:

```
0x02278500  native 0x00000280   melonDS 0xFFFFDF80   (container base 0x02278264 + 0x29C)
```

i.e. a signed field of the movie container/player struct already disagrees
(+640 vs -8320).  That word and the predictor seed are the two candidates for
the true first cause; both are one level *below* the decode loop this phase
was asked to audit.

## 4. Acceptance checks on this branch

```
$ cd nnktool && cargo test
test result: ok. 32 passed; 0 failed
$ SKIP_MELONDS=1 bash tools/verify_phase2a.sh
RESULT: PASS
```
