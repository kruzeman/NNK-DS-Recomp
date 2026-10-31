# Phase-5L — the idle parking is NOT a state divergence: both engines wait 4.0/4.4e9 cycles at the same command boundary and resume with the same card read; the next barrier is the Mobiclip player's on-demand decode at `0x020F6560`

Follows Phase-5K (merged `5713d28`).  Phase-5K named **barrier G**: the guest
parks in the SDK idle loop `0x02007134`/`0x02007138` with IF bit 19 latched and
`cardnext = NEVER`, so "the card/ARM7 event that ends the boot is never
produced".  Phase-5L measured that premise directly and it is **false**.

**Headline**

1. **The parking is real guest behaviour, and melonDS does the same thing for
   the same length of emulated time.**  Both engines stop the card stream at the
   *same* command boundary (a `B8` dummy right after `B7 0x1BC50400`) and resume
   it with the *same* next command (`B7 0x1BC8F400`).  The wait is
   **4,055,728,714 raw ARM9Timestamp** in melonDS and **4,422,210,889 card
   cycles** in native (≈ 3,620 frames ≈ 60 s of emulated time; the 9 %
   difference is native's coarse cycle scale, measured at 1.079× on this run).
   Native's 80M-step budget covers only ~1.4 % of that wait, which is why the
   run looked frozen at the idle loop.
2. Two more waits of the same shape follow (186.4M / 183.3M ts in melonDS,
   203.4M / 199.4M cycles in native), again ending on the same commands
   (`B7 0x1BC96C00`, `B7 0x0004D400`).
3. With the budget raised to 2.3G steps the run goes straight through all three
   waits: **9,691 card starts, an exact `(op, addr, size)` prefix of melonDS's
   9,840** (`tools/card_cmdseq.py`: first divergence "none of 9691"), 21,524
   distinct guest PCs (was 19,470), 4,033 frames presented.
4. **The picture is there:** frame 4000 is non-uniform — the **Mobiclip Video
   Codec** splash (white `0xfbfbfb` background, red `0xfb0000` logo).  PNGs:
   `/home/hermes/NiNoKuni/shots/p5l_f04000_top.png` and `..._bot.png`.
5. The run then stops on a **new** barrier: `dispatch to untranslated pc` at
   `0x020F6560`, step **2,255,768,127** — inside the movie player, after the
   guest has streamed 130 `B7` blocks of a low-ROM mov stream
   (`0x00132400..0x00133600`).  melonDS executes that PC (probe below), so it is
   a legitimate code address and the halt is a recompilation/on-demand-decode
   gap, not a wild jump.

## 1. Who stalls where — measured on both engines

`tools/card_stream_gap.py` (new, §5) reports the largest inter-start gaps of a
card stream, which a command-sequence diff cannot show (the next command
matches, so the stream looks continuous):

```
$ python3 tools/card_stream_gap.py p5l/long.err p5h/mel_card.log --top 3
p5l/long.err: 9691 starts, unit=card cycles, span=4894334317, median gap=3452
  gap 4422210889 (90.4% of span)  after 0xB8@0x00000000 (steps=42034531)
                                        ->  0xB7@0x1BC8F400 (steps=2069381118)
  gap 203360353  (4.2%)           after 0xB8@0x00000000 (steps=2069638877)
                                        ->  0xB7@0x1BC96C00 (steps=2162914908)
  gap 199357938  (4.1%)           after 0xB8@0x00000000 (steps=2163406604)
                                        ->  0xB7@0x0004D400 (steps=2254836626)
p5h/mel_card.log: 9840 starts, unit=raw ARM9Timestamp, span=4543677356, median gap=5513
  gap 4055728714 (89.3% of span)  after 0xB8@0x00000000 (pc=0x02016970)
                                        ->  0xB7@0x1BC8F400 (pc=0x02016930)
  gap 186353560  (4.1%)           after 0xB8@0x00000000 (pc=0x02016970)
                                        ->  0xB7@0x1BC96C00 (pc=0x02016930)
  gap 183260094  (4.0%)           after 0xB8@0x00000000 (pc=0x02016970)
                                        ->  0xB7@0x0004D400 (pc=0x02016930)
```

Same three waits, same durations (up to the measured cycle-scale factor), same
resumption commands, same order.  A missing producer cannot look like this: a
missing producer would leave native parked **forever** while melonDS resumed
immediately.

While native waits, its IF/IE state is exactly the melonDS state recorded by
Phase-5H's loopwatch (which Phase-5L re-ran over the whole gap,
`p5l/mel_loop.log`, 25,408 idle-loop entries):

```
melonDS  enter ts=1615073590 pc=0x02007138 ie=0x00040019 if=0x00280000 ime=1
         irqwake ts=1615139956 pc9=0x0200911C ie=0x00040019 if=0x00280010 enabled=0x00000010  (Timer1)
         irqwake ts=1620644430 pc9=0x0200911C ie=0x00040019 if=0x002C0000 enabled=0x00040000  (IPC recv)
native   wfiprobe pc=0x02009118 step=79000003 ime=1 ie=0x00040019 if=0x00080000
         cpsr=1F cardnext=NEVER romcnt=0x27416000 spicnt=0xC000
```

`ie=0x00040019` is identical; the pending bits differ only because each engine
samples the same rotating set (timer 0x10 / IPC 0x40000 / cart 0x80000) at a
different instant.  **No state word diverges at the park** — the earlier reading
("IF bit 19 latched but nothing scheduled") is a property of the wait itself:
the cart bit stays latched because IE bit 19 is not enabled, and no transfer is
scheduled because the guest has not asked for one yet.

## 2. The long run — through all three waits

```
env NNK_ROM_PATH='Ni No Kuni.nds' NNK_GX=headless NNK_STEP_LIMIT=2300000000 \
    NNK_CARDLOG=$S/card_long.log NNK_GX_SHOT=$S/longf NNK_GX_SHOT_EVERY=400 \
    NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
    out/split/p5k1 work/phase3a/arm9_stored_padded.bin
```

| check | Phase-5K (80M) | now (2.26G) |
|---|---|---|
| run end | 80,000,001 (budget) | **2,255,768,127 — halt `dispatch to untranslated pc` @ `0x020F6560`** |
| distinct guest PCs | 19,470 | **21,524** |
| frames presented | 148 (uniform) | **4,033 (frame 4000 non-uniform)** |
| card starts | 9,542 (stopped in the first wait) | **9,691** |

`tools/card_cmdseq.py long.err p5h/mel_card.log` → *first (op,addr,size)
divergence at index none of 9691 compared*; melonDS's next start after native's
last is `B7 0x0BB33A00` at ts 4,543,150,296.

## 2b. Verification of the format-7 fix (branch code, rebuilt)

Same image/recipe with the patched `tools/nnk_smc.inc` (rebuilt runtime,
`out/split/p5l1`):

```
done steps=2256160152 distinct_pcs=21685 halt_pc=0x0207B218 halt=dispatch to untranslated pc
```

The run **passes `0x020F6560`** (no `smcfail` for it) and continues 392,145
steps further, then stops on the *same class* at the next site:
`0x0207B218`, distinct PCs 21,685 (was 21,524).  So format 7 was a real hole and
the fix is measured, but the Mobiclip player has a chain of unrooted PCs; each
one costs a ~10-minute run to reach, which is why this phase stops here with the
chain named rather than fixed.

## 3. The picture (the goal)

Frame 4000 (top screen) is the **Mobiclip Video Codec** splash:

```
longf.04000.top.ppm  256x192  distinct(sampled)=34  top=[0xfbfbfb x1088, 0xfb0000 x170]
```

PNG (stdlib `gxrender/tools/ppm2png.py`): `/home/hermes/NiNoKuni/shots/p5l_f04000_top.png`
and `p5l_f04000_bot.png`.  Frames 400..3600 are uniform black — they are inside
the 3,620-frame wait, which is exactly what melonDS shows there too (the
`gxrender/traces/2dsmoke1` capture's f00102..f00118 frames are the same
grey→black fade: `0xefefef` then `0x000000`).  Content appears only after the
resume, at the mov stream.

## 4. New barrier H (named): `0x020F6560`

```
halt: dispatch to untranslated pc @ 0x020F6560, step 2,255,768,127
      (last card events: B7 0x00133600 block + two B8 dummies, cyc 4,901,073,967)
```

* **The PC is legitimate guest code.**  melonDS executed it —
  `MELONDS_ARM9TRACE_LO=0x020F6560 MELONDS_ARM9TRACE_HI=0x020F6570
  MELONDS_ARM9TRACE_N=1 MELONDS_ARM9TRACE_EXIT=1` produced `0x020f6560`
  (`p5l/mel_hit.txt`), so the guest really branches there.
* **The exact word/state** (`NNK_SMC_FAILLOG=1`):

  ```
  smcfail pc=0x020F6560 word=0x1C025C41 thumb_try=1 hw0=0x5C41 hw1=0x1C02
  ```

  `0x5C41` is Thumb **format 7** (`0101 L B 0 Ro Rb Rd`) = `ldrb r1,[r0,r1]`
  (binutils and `nnktool/src/decode.rs` both say so).  The live C re-decoder
  in `tools/nnk_smc.inc` refused **every** `0x5xxx` word:

  ```c
  if ((hw & 0xF000u) == 0x5000u) {          /* formats 7/8/9  */
      unsigned L = (hw >> 11) & 1u;
      unsigned B = (hw >> 12) & 1u;         /* bit12 is the 0101 prefix — ALWAYS 1 */
      ...
      if (hw & 0x1000u) return 0;           /* "format 8" — true for the whole block */
  ```

  bit12 is part of the `0101` prefix, so the guard always fired and the arm was
  dead code; the format 7/8 discriminator is bit 9 (`0x0200`) and B is bit 10.
  The ARM fallback then failed on `0x1C025C41` (cond=NE with bits 27-25 = 111 is
  not a valid ARM form), so `nnk_smc_exec_any` returned 0 and the run halted.

**Fix (measured scope).**  `tools/nnk_smc.inc` `smc_exec_thumb` now implements
format 7 exactly as the Rust decoder always did — split on bit 9, `B = bit10`,
`ea = R[Rb] + R[Ro]` — and leaves format 8 returning 0 so an unimplemented
halfword register-offset form still halts honestly instead of executing as a
word.  Class 16 in `nnktool/src/decoder_regression_tests.rs` locks it: a Rust
decode of `0x5C41`/`0x5801` plus a source-level assertion that the C arm no
longer derives B from bit12 and still splits on `0x0200` (suite 28 → **29**).

**Follow-up barrier I (named): the Mobiclip movie path.**  With format 7
decoding, the run passes `0x020F6560` and continues into the player (see §2b) —
the remaining work is the mov stream itself (`B7 0x0BB33A00` onward in
melonDS's log, low-ROM addresses `0x0013xxxx`) and its video path (Mobiclip
decoder → Y2R/convert → display), i.e. where the real content after the splash
lives.

## 5. Tooling added

`tools/card_stream_gap.py` — the missing measurement: the largest gaps between
consecutive card transfer starts, with the op/address on both sides, for native
`p4t: start ...` lines and melonDS `start ts=... cmd=...` lines.  Reads either
format from one log.  This is what turned "the run freezes at 42M steps" into
"the run is inside a 4.4e9-cycle wait that melonDS also has".

## 6. Reproduce

```sh
ln -sfn /home/hermes/NiNoKuni/work work
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5l
# long run through the waits (~10 min wall, ~4.3 GB of stdout if not redirected)
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=2300000000 NNK_CARDLOG=$S/card_long.log \
  NNK_GX_SHOT=$S/longf NNK_GX_SHOT_EVERY=400 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  /home/hermes/NiNoKuni/out/split/p5k1 work/phase3a/arm9_stored_padded.bin > /dev/null 2> $S/long.err

python3 tools/card_stream_gap.py $S/long.err <melonDS card log> --top 3
python3 tools/card_cmdseq.py $S/long.err <melonDS card log>

# melonDS: does the guest really execute the halt PC?
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg \
  SDL_AUDIODRIVER=dummy MELONDS_ARM9TRACE_PATH=$S/mel_hit.txt \
  MELONDS_ARM9TRACE_LO=0x020F6560 MELONDS_ARM9TRACE_HI=0x020F6570 \
  MELONDS_ARM9TRACE_N=1 MELONDS_ARM9TRACE_EXIT=1 \
  timeout 260 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"

# the halt word (10 min wall)
env NNK_SMC_FAILLOG=1 NNK_STEP_LIMIT=2300000000 ... /out/split/p5k1 ... 2> $S/fail.err
```
