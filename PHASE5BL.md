# Phase-5BL — GXSTAT is now modelled from our own GXFIFO; the "Yes" path leaves the poll and reaches the submission path, but the post-skip screen is still black

Branch `wt/t_093ee276` (commit `7df65b6`, on master `8e896a5`).
**Partial deliverable**: items 1 and 2 of the card are done and measured, item 3
(the post-skip *picture*) is **not** reached, and the regression gates of item 5
were **not run** — this file records exactly what was proven, what was measured,
and the next bounded step.

## 1. GXSTAT (`0x04000600`) is now a modelled register (card item 1)

`gxrender/src/gx_fifo.[ch]` + `gxrender/src/gx_display.c`:

* `Gx` gains `gxstat` — only the bits the CPU owns (30-31 IRQ condition, 15
  stack error, 14/0 pending push-pop/test, 27 commands queued).
* `gx_fifo_level()` returns the decoder's **own** occupancy: the packed command
  bytes still buffered in the current word plus one for a multi-parameter
  command still collecting parameters.  It is 0 after a completed submission and
  <= 4 in between — derived, never a constant, and never >= 128 (our renderer
  executes a command when its last parameter word arrives).
* `gxstat_word()` composes the word exactly as melonDS `GPU3D::Read32`
  case `0x04000600`: `GXStat | (pos_sp&0x1F)<<8 | (proj_sp&1)<<13 | level<<16 |
  (level<128)<<25 | (level==0)<<26`.
* `gxstat_write()` latches bits 30-31 and implements the bit-15 stack reset.
* `gxd_io_read()` serves `0x04000600..3` at widths 1/2/4 with melonDS
  `Read8/Read16` byte semantics.
* Honest gaps, documented in the source: bit 1 (box/pos/vec test result) stays 0
  because those commands are stubs in `gx_fifo.c`; bit 15 is only cleared by the
  guest's write (our push/pop does not flag overflow).

The prelude (`nnktool/src/recompile.rs`, `io_read32`) gains the register case and
`tools/p5bl_gxstat_splice.py` installs the identical transform in the frozen
`work/phase3a/...p5u.c` (wired into `tools/build_split.sh` as `5bl_gxstat`,
two `required` manifest rows).  `prelude: AUDIT OK` with the splice; the audit
**fails** with `NNK_PRELUDE_DISABLE_SPLICE=5bl_gxstat` (negative control run).
The prelude case also handles a 16/8-bit access by byte offset, so a read of
`0x04000602` returns melonDS's `Read16` composition.

## 2. What the guest does next (card item 2): GXFIFO IRQ, DMA mode 7, DMA IRQ

`nnk_gx_irq_eval()` mirrors melonDS `GPU3D::CheckFIFOIRQ` into IF bit 21 after
every 32/16-bit IO write — the three places melonDS re-evaluates the level line
(WriteToGXFIFO, the GXSTAT write, the IF acknowledge).

Measured with only that in place: the guest leaves the `0x0200985C..6C` poll and
enters the SDK's submission path, then **storms**: a 4 M-step pc trace at
3.696e9 is 114,053/4,000,000 steps inside `0x02009910..0x02009920`, the head of
the chunk feeder that runs on the GXFIFO IRQ, which reads its queue counter
`[0x0206CA1C+12]` as 0 and returns without acking.  Two further defects were
found with `arm-none-eabi-objdump` and the ITCM dump (`NNK_ITCM_DUMP` from a
same-build savestate):

* `0x01FF845C` (ITCM) programs `DMAxCNT = 0xFC000000 | (len>>2)` — bit31 enable,
  **bit30 IRQ on completion**, bits 27-29 = **7 (GXFIFO)**, bit26 32-bit.  Our
  DMA model executed only start mode 0, so every GXFIFO block was **silently
  dropped** and the feeder queue never drained.
* No DMA completion IRQ was ever raised, so the SDK's async queue could never
  retire (its callback is what clears `0x02074EB4`, the flag Phase-5BK §5 saw
  never written).

Fixes: `tools/nnk_dma.inc` executes mode 7 like an immediate block (our FIFO
never stalls) and raises `IRQ_DMA0+i` when Cnt bit30 is set; `io_do_dma` in the
prelude and the same transform in the splice do the same for the immediate path.

Result (cold boot, full Phase-5BK script B120/A4300/A4550/START 5900+6050+6200+
6350, Right 6400, A 6420, budget 4.2e9): IRQ entries drop from **4,907,276 to
55,643**, `distinct_pcs` rises to 33,813, and the run's halt is
`halt_pc=0x02009120` — inside the ARM9 idle loop, not a poll.

## 3. What is still wrong (card item 3 NOT met)

The picture after the skip is still black: bottom-screen mean 0.0 from display
frame 6450 to the end of the run (7509 frames), top screen black throughout.
The trace at 3.80e9..3.83e9 is 633,706/4,000,000 steps in

```
0x020090F8  blx r2                  @ (submission helper)
0x02009108  push {r3,lr}
0x0200910C  bl 0x02008D20           @ CPSR.I = 1  (disable IRQ)
0x02009110  bl 0x02009118
0x02009114  b  0x02009110           @ forever
0x02009118  mov r0,#0
0x0200911C  mcr p15,0,r0,cr7,cr0,{4} @ ARM9 "wait for interrupt"
0x02009120  bx lr
```

i.e. the guest has finished the submission and waits for an interrupt **with
IRQs disabled by its own code** (`0x02008D20`), spinning in WFI.  So the post-skip
halt is no longer a register model: either our modelled WFI does not wake on the
interrupt that this thread waits for (VBlank/DMA), or the submission completes
without ever reaching SWAP_BUFFERS so nothing is drawn.  This is the next
bounded step and it needs its own phase.

## 4. What was measured, and with what

| run | script | budget | result |
|---|---|---|---|
| `work/p5bl/yes.log` | card's short script, **no NNK_GX=1** | 4.2e9 | invalid input: no display model, frames 4600..5925 are the fan-translation notice; kept only as the "why the script needs the START presses" record |
| `work/p5bl/yes2.*` | full script + Right/A | 3.8e9 | GXSTAT-only build: skip accepted (s01 stops at frame 7), screens black, IRQ storm at `0x02009910` |
| `work/p5bl/yes3.*` | full script + Right/A | 4.2e9 | final build: skip accepted, storm gone (55,643 IRQs), guest idles in WFI, picture still black |

Evidence under `work/p5bl/` (never committed): `yes*.log`, `tr3700.pc`/`tr3800.pc`
pc traces, `itcm.bin` (ITCM dump), `state3700.nnks`/`state3800.nnks` (same-build
savestates used for the fast resumes), `shots2/` (frames 6100..6930 for the
GXSTAT-only build), `shots3/` (frames up to 7509 for the final build),
`p5bl_boxscan.py`.

Scripts used: `work/p5bl/yes2.nnk` (B 120, A 4300..4306/4550..4570, START
5900/6050/6200/6350 each held 12 frames, Right 6400..6406, A 6420..6426) — the
same shape as `work/repro_5bi/start_cutscene.nnk` plus the answer.  The card's
shorter script is **not** sufficient: without the three later START presses the
run never reaches the prompt (it stops on the notice screen).

## 5. Build / verification state

* `cargo build --release` OK; `bash tools/build_split.sh` OK with
  `NNK_GX=1`, `NNK_SPLIT_CFLAGS="-std=c11 -O2 -g0 -w"`, 32 TUs, `BUILD OK`.
* `prelude: AUDIT OK` (63 required features, 2 added here); the negative control
  `NNK_PRELUDE_DISABLE_SPLICE=5bl_gxstat` is rejected by the audit (proved).
* **Not run**: `cargo test` (class suite), `tools/verify_phase2a.sh`, the intro
  correlation, the prompt-text gate, the title artwork, `NNK_EXC_TRACE`
  signature, savestate round trips, the 600 M-step speed check, and the "No"
  path's frame-8125 scene.  The changes touch the DMA/IRQ path that every one of
  those gates exercises, so they must be re-run before this lands.
