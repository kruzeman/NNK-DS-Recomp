# Phase-5BK — answering the skip prompt: A = "No" (keep watching), Right+A = "Yes" (skip); the post-skip picture is black and the guest parks in a GXSTAT poll

Follows Phase-5BJ (master `bdb4c5d`).  Branch `wt/t_ceb304af`.
**Measurement phase: no committed code change.**  The runtime tree is unmodified;
everything below is measured on a fresh build of `bdb4c5d` plus one *private,
uncommitted* instrumented image for the barrier experiment (§5).

Build: `SPLIT_DIR=work/p5bk/split NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w"
NNK_GX=1 BUILD_JOBS=20 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p5u.c
bash tools/build_split.sh work/p5bk/nnk` → `rc=0`, splice chain incl. `5bj_dsp_mul`,
prelude audit OK.  `cargo build --release` in `nnktool/` first (2.7 s, cached).

## 1. The card's premise is inverted: the highlighted option is **No**

Two cold boots of the ROM (headless, same script as `work/repro_5bi/start_cutscene.nnk`
plus one extra entry each):

| run | script | extra entry |
|---|---|---|
| `yes` | `work/p5bk/yes.nnk` | A (`0x3FE`) held frames **6420..6430** |
| `no`  | `work/p5bk/no.nnk`  | **Right** (`0x3EF`) 6400..6412, then A 6420..6432 |

The prompt is up at display frames **6375..6425** in both runs (identical shot means,
`work/p5bk/{yes,no}_list.txt`), i.e. the card's window is right but the press timing
is only just inside it.

`NNK_INPUT_TRACE` proves the guest itself saw both edges (not just the host shim):

```
step=3579168994 guest KEYINPUT read #7707 val=0x03EF frame=6400   <- Right, guest read it
step=3590367726 guest KEYINPUT read #7877 val=0x03FE frame=6420   <- A, guest read it
```

**Measured outcome of the two answers:**

* **A alone (the highlighted option) = "No" — keep watching.**  The box is gone by
  frame 6425 (`mean` 115.6 → 103.5, delta 27.52 = box removed) and the s01 stream
  **keeps presenting**: movie frames 300 @ step 4.3025e9, 360 @ 4.4664e9, … right
  through to its last frame **580/581 @ step 5.0836e9**.  The cutscene then ends by
  itself and the game continues: at display frame **8125** the picture changes to the
  next scene (delta 38.75, mean 78.6) — a full-colour animated scene of the boy with
  a grocery bag and the red-haired girl in a shop, with a subtitle line
  (`work/p5bk/shots_yes/f.08125.bot.ppm.png`) — and keeps rendering past frame 8300.
  That scene in turn ends around frame 11000, and the run's own halt is
  `halt_pc=0x02009864` — **the same GXSTAT poll as the skip path** (§3): this barrier is
  the first GX command submission, whichever route reaches it.
* **Right, then A = "Yes" — skip.**  The s01 stream **stops at movie frame 45/581**
  (step 3,593,263,396 — four display frames after the A press at 3,590,365,984) and
  no further `movie: present` line is emitted for the remaining 1.6e9 steps of the
  run.  So 536 of 581 frames (92 %) are skipped: the skip request is accepted **by
  the guest**, not by the emulator.

This also corrects the card: Phase-5AL had already recorded the melonDS reference
screenshot of this dialog as **"No selected"** (`PHASE5AL.md` §4,
`shots/p5al/md_f1200_bot.png`).  Our render agrees (green brackets on the *left*
button, `work/p5bk/shots_yes/f.06400.bot.ppm.png`); the card read them as being on
"Yes".  A cursor move is therefore required before A means "skip".

## 2. What happens after "Yes": the screens go black and stay black

`no` run, `NNK_STEP_LIMIT=5.2e9`, `NNK_GX_SHOT_EVERY=25`:

```
frame 6400  mean 115.7 (prompt up)
frame 6425  mean 115.6 (prompt still up — cursor is on Yes now)
frame 6450  mean   0.0 dist 1 delta 115.64   <- both screens fully black
frame 6475 .. 9275  mean 0.0 dist 1          <- black for the rest of the run
```

The **top** screen is black from the start of this whole cutscene and stays black
(`work/p5bk/no_top.txt`).  No halt: `done steps=5200000001 … distinct_pcs=31902
hle_hits=13277 halt_pc=0x02009860 halt=step budget exhausted`.  The guest is alive
and executing guest code — it just never submits anything to the renderer.

## 3. Cause, measured: the guest polls **GXSTAT** and our IO model returns 0

A pc trace of the post-skip machine (savestate `work/p5bk/no_pre.nnks`, written at
step 4.1e9, resumed in seconds; `NNK_TRACE_FROM=4100000000`,
`work/p5bk/postskip.log`):

```
traced pcs 3,999,946   top: 0x0200985C, 0x02009860, 0x02009864, 0x02009868, 0x0200986C
                       795,481 hits each  (99.6 % of the window)
loop runs 34, total 3,977,405 insn, longest 333,532; every exit is an IRQ
(0x01FF8000); 0x02009870 — the instruction after the loop — executes **0** times
```

The loop, disassembled from the decoded ARM9 (`work/arm9_dec.bin`):

```
0x02009858  ldr  r5,[pc,#168]        @ r5 = 0x04000600   (GXSTAT)
0x0200985C  ldr  r0,[r5]
0x02009860  and  r0,r0,#0x07000000
0x02009864  lsr  r0,r0,#24
0x02009868  tst  r0,#2
0x0200986C  beq  0x0200985C          @ wait until GXSTAT bit 25 is set
```

State witness (`NNK_DEBUG_AT_PC=0x0200986C`, `work/p5bk/gxstat_dbg.log`), 64 samples
spanning 40 M steps, **all identical**:

```
dbg1 step=4100000001 pc=0200986C r0=00000000 r5=04000600 r14=02036484 nzcv=0110
```

`0x04000600` never yields bit 25, so the poll can never end.

Ground truth (melonDS, `/home/hermes/tools/melonDS/src/GPU3D.cpp:2976`,
`GPU3D::Read32` case `0x04000600`):

```c
u32 fifolevel = CmdFIFO.Level();
return GXStat | ((PosMatrixStackPointer & 0x1F) << 8) | ((ProjMatrixStackPointer & 1) << 13)
     | (fifolevel << 16) | (fifolevel < 128 ? (1<<25) : 0) | (fifolevel == 0 ? (1<<26) : 0);
```

bit 25 = **"command FIFO less than half full"** — the "there is room for the next
command batch" flag the guest waits on before submitting GX commands.

Our model has no such read: `gxrender/src/gx_display.c:1144 gxd_io_read()` serves only
KEYINPUT/KEYCNT/VRAMSTAT, and the runtime's `io_read32` (generated prelude,
`nnktool/src/recompile.rs`) ends in `default: return 0`.  Writes to 0x04000600 are
handled (stack-reset only, `gx_display.c:935`); **reads were never modelled**.  This
is a device-model defect of exactly the class the card asks for, not a guest bug: the
guest's own code is correct, and it never gets a picture because it never gets a
"FIFO has room".

## 4. Why the cutscene path hid it

While the cutscene plays, the guest waits for VCOUNT instead
(`0x020078F4 ldrh r0,[0x04000006]; cmp r0,#0` — the loop seen at the end of
`work/repro_5bi/run_4e9.log`), and the film is presented by the movie HLE, so no GX
command has to be submitted.  Only after the skip does the guest switch to the 3D
scene-submission path — whose very first act is the GXSTAT poll.

## 5. The barrier after that one (measured with a private, uncommitted experiment)

To prove the causal chain rather than argue it, a **private copy** of the frozen image
(`work/p5bk/img_gxstat.c`, gitignored build state) got one `case` in `io_read32`:

```c
case 0x04000600: return (1u << 25) | (1u << 26);   /* level 0: our renderer executes commands at write */
```

Built as `work/p5bk/nnk_gx` (`SPLIT_DIR=work/p5bk/split_gx`), then resumed from the
*same* stuck post-skip state (`NNK_SAVESTATE=work/p5bk/no_pre.nnks`,
`NNK_SAVESTATE_STRICT=0`).  Result: the poll passes (0x02009870 is reached), the guest
runs on and the run reaches its budget (`done steps=4500000001 … halt_pc=0x020363C4`).
The 4-M-step window from 4.1e9 then shows the **next** wait, 99.4 % of the window:

```
0x020363BC  ldr r0,[pc,#12]     @ r0 = 0x02074EB0
0x020363C0  ldr r1,[r0,#4]      @ [0x02074EB4]
0x020363C4  cmp r1,#0
0x020363C8  bne 0x020363C0      @ wait until it is 0
...
0x02036550  bl  0x020363BC      @ then
0x02036554  ldr r1,[pc,#20]     @ 0x04000400  (GX command FIFO)
0x02036560  str r6,[r1]
```

`NNK_STOREWATCH=0x02074EB0:0x02074EB8` over 100 M steps logged **0 writes**: the
guest-side queue flag at 0x02074EB4 is never cleared, so the submission path stops
just before the FIFO write.  Both screens stay black through frame 7950
(`work/p5bk/shots_fix/`).  This is the next barrier to name in a follow-up card — most
plausibly the GXSTAT **IRQ** bits (`0x04000602`, `GXStat` bits 30-31, which our model
ignores) that the guest uses to retire the queue.

## 6. Background behind the prompt box: real s01 content, not a ghost

Reference: `data/movie/s01.mods` extracted from the card (nitro id 9244, 1,191,972 B)
and decoded with the project's own decoder (`tools/mobiclip_selftest.c` → 581-frame
`s01.yuv`, 0.45 ms/frame — `work/p5bk/s01.yuv`).  Correlation is Pearson over 4×4
box-filtered luma (64×48 blocks) with the dialog box excluded:

| capture | mask | best s01 frame | corr |
|---|---|---|---|
| frame 6400 bottom (`work/repro_5bi/f.06400.bot.ppm`) | top band only (y < 70) | f044 | **0.9960** |
| same | box rect (10,34)-(246,176) excluded | f044 | 0.9865 |
| same | full frame (box occludes the film) | f054 | 0.8918 |

0.996 against the s01 decode (Phase-5BD measured 0.9984 with the same method) — the
scene behind the box is the real film, not a decode ghost.  Nothing to fix here.

## 7. Evidence (under `work/p5bk/`, never committed)

| file | what |
|---|---|
| `yes.log` / `no.log` | cold boots: movie/exc traces, `done steps=…` summary (6.2e9 / 5.2e9) |
| `{yes,no}_input.log` | `NNK_INPUT_TRACE` — the guest's own KEYINPUT reads of Right/A |
| `{yes,no}_list.txt`, `no_top.txt` | per-frame shot inventory (mean/dist/delta) |
| `postskip.log`, `postskip_hist.txt`, `postskip_loop.txt` | the post-skip GXSTAT poll trace |
| `gxstat_dbg.log` | `NNK_DEBUG_AT_PC=0x0200986C` witness (r0=0, r5=0x04000600) |
| `fix_resume.log`, `fix_trace.log`, `fix_hist.txt`, `sw_74eb0.log` | the private GXSTAT experiment and the next wait |
| `s01.mods`, `s01.yuv`, `analyze.py`, `corr_probe.py` | reference decode + correlation |
| `shots_yes/f.06400.bot.ppm.png`, `shots_no/f.06450.bot.ppm.png`, `shots_yes/f.08125.bot.ppm.png` | prompt, first black frame, next scene |

## 8. Frontier / next card

* Step budget, halt reason, display mode: headless; `no` run 5.2e9 steps
  (`halt=step budget exhausted`, `halt_pc=0x02009860`, 31,902 distinct PCs);
  `yes` run 6.2e9 steps, no halt; resumed states seconds.
* Script: `work/p5bk/no.nnk` (Right 6400..6412, A 6420..6432) is the skip reproducer.
* Next barrier: **GXSTAT reads** (name + model, §3) and then the **guest queue flag at
  0x02074EB4** (§5).  Fix in `nnktool/src/recompile.rs` (+ a splice for the frozen
  image, same policy as 5X/5AE/5BJ) and re-verify with a cold boot of `no.nnk`.
