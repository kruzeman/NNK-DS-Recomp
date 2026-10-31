# Phase-5G — the IE-mask divergence does not exist; both runs program IE bit 19 identically for exactly one card-transfer burst

Follows Phase-5F (merged `2218eee`). Scope: root-cause the IE
(`0x04000210`) mask divergence native `0x00040019` vs melonDS `0x000C0009`,
root-cause the wrong-path halt helper `0x020090D8`, and reach the frames-102
content.

**Headline (all measured on this branch):**

1. **The stated first differing word is falsified.** Native *does* program IE
   bit 19, exactly as melonDS does, and the card IRQ *does* fire: native
   delivers **24** IRQs with `ie=0x000C0009 if=0x00080000` at steps
   18,389,760 … 18,391,807, in a **2,302-step** window that is closed by the
   same `irqDisable(0x00080000)` the reference executes. There is no missing
   `os_irqEnable(1<<19)` call and no lost RMW bit.
2. **Why Phase-5F saw otherwise: a clock-unit mix-up, not guest behaviour.**
   The melonDS `LoopWatch` hook prints `ts=(u32)NDS.ARM9Timestamp` (raw), while
   the IO trace prints `ts=ARM9Timestamp >> ARM9ClockShift` (=/2 on this boot).
   The "`IE=0x000C0009` at ts=56,548,252" reference sample is therefore
   **io-ts 28,274,126** — inside the same ~261k-cycle burst. Phase-5F compared
   melonDS's *bit-19 window* with native's *post-window* state and read the
   difference as a mask bug.
3. **Both runs execute the identical driver sequence** (instruction-level
   window comparison, §2): card-IRQ setup `0x02016C68` → 25 transfer ends →
   teardown `0x02016C1x` → `IE=0x00040019`. melonDS's 25,232 WFI-wake list is
   dominated by `ie=0x00040019` wakes (VBlank/timer/IPC); only the first 25 are
   the card burst, i.e. the same 25 native takes.
4. **The real barrier stays the halt path** (`0x020090D8`, named in §4/§5):
   after the last common card block the guest stops being fed (native: ~12
   dummy `0xB8` reads; melonDS: `cmd=0xB70DA446` + 550 dummy reads) and native
   walks into the SDK halt helper that the reference never executes.

## 1. Every IE write, both runs

### native (branch binary, 60M steps)

| step | event | source |
|---|---|---|
| 18,389,577 | `os_irqEnable(0x00080000)` → `IE = 0x00040009 \| 0x80000 = 0x000C0009` | `NNK_DEBUG_AT_PC=0x020060B4`: `r0=00080000 r14=02016CA0` |
| 18,389,760 … 18,391,807 | **24** `irq enter … ie=0x000C0009 if=0x00080000 resume=0x02016CD0` | `f.err` (exactly 24 lines with `ie=0x000C0009`) |
| 18,391,879 | `os_irqDisable(0x00080000)` → `IE = 0x00040009` | `NNK_DEBUG_AT_PC=0x020060E4`: `r0=00080000 r14=02016C30` |
| 18,846,664 … | `os_irqEnable(0x00000010)` (Timer 1) → `IE = 0x00040019` | same probe, 136 calls |

`NNK_CARDLOG` (`n_card.log`) shows **25** card transfer-end raises
(`iflags=0x00080000`) inside the same window — 24 are delivered, the 25th
arrives after the teardown and stays pending, which is exactly why the WFI
helper at step 23,490,852 still has `IF=0x00080000` with bit 19 masked. That
pending bit is the *tail of a correctly serviced burst*, not evidence of a
starved producer.

### melonDS (60 s, ts to 1.95B)

66,104 IE accesses captured with the new address-window filter
(`MELONDS_IOTRACE_LO=HI=0x04000210`). Bit 19 is written **once in the whole
run**:

```
R32 0x04000210 0x00040009 pc=0x020060C4 ts=28263163     <- SDK IE read (RMW)
W32 0x04000210 0x000C0009 pc=0x020060CC ts=28263167     <- IE |= 0x80000
W32 0x04000210 0x00040009 pc=0x02006100 ts=28524002     <- IE &= ~0x80000
```

Register probe at the three SDK IE helpers (`0x02006098/0x020060B4/0x020060E4`,
20,000 hits) shows one `r0=0x00080000` call among 13,483 `irqEnable` calls
(13,056 `0x10`, 423 `0x08`, 2 `0x40000`, 1 `0x80000`, 1 `0x01`):

```
reg pc=0x020060B4 cpsr=0x6000009F r0=0x00080000 r14=0x02016CA0 ts=28263152
```

Same caller (`0x02016CA0`, the ARM-state card-driver function `0x02016C68`),
same relative position, same value.

### Why `p5f/wfi3.txt` reads `ie=0x000C0009` at "ts=56,548,252"

`LoopWatch::IRQHit` is called with `(u32)NDS.ARM9Timestamp` (ARM.cpp:979) —
unshifted; `ARM9IOTrace::Log` receives `ARM9Timestamp >> ARM9ClockShift`
(NDS.cpp, all six call sites). With `ARM9ClockShift=1` on this boot the two
`ts=` fields differ by 2×: 56,548,252 / 2 = **28,274,126**, i.e. the first card
IRQ of the burst above. The 25-bit-19 wakes are the whole story; the following
25,207 wakes carry `ie=0x00040019` — the same mask native settles on.

## 2. Instruction-level window comparison

- melonDS: `MELONDS_ARM9TRACE_LO=0x01FF8000 HI=0x02400000 TSLO=28260000
  TSHI=28530000` (9,813 PCs, includes ITCM/IRQ vector).
- native: `NNK_TRACE_FROM=18388000`, `NNK_STEP_LIMIT=18396000` (8,004 PCs).

Aligned at the `os_irqEnable` call (`0x020060B4`), the two sequences agree for
**183 instructions** — the whole call, the `0x02009700` polling loop, the
`0x0200A6D0` path, the ITCM helper `0x01FF845C`. First difference:

```
native step 18,389,760 :  0x01FF8000  (ARM9 IRQ vector entry; resume=0x02016CD0)
melonDS  ts 28,263,404 :  0x02016CD0  (returns from the setup call)
```

melonDS takes its first card IRQ 616 instructions after the enable
(`ts=28,274,139`), native 183 — the transfer-end event lands at a different
phase of the same 10,845-cycle transfer. After that both engines run the same
path at ~66 (melonDS) vs ~92 (native) instructions per transfer, deliver 24/25
card IRQs, and call the same teardown `0x02016C2C` → `0x02016C30`. This is a
card-clock phase/timing difference of the runtime's `card_tick(cost)` model
(≈2× more guest instructions elapse per card completion in native), **not** an
IE or IRQ-delivery difference, and it does not change the guest's sequence.

## 3. The WFI "starvation" was also an artefact

`NNK_WFI_PROBE` at `0x02009110/0x02009118` logs every poll; the 20,000-cap is
reached after **99,995 steps** (5 steps per poll, step 23,490,852 →
23,590,847) — under one 60 fps frame at this run's step rate. All 20,000 lines
having `IE&IF=0` is therefore expected even with a perfectly healthy VBlank
producer; the run does take IRQs later (`irq enter step=23754440 … if=0x000C0000`).
The probe needs a time- or frame-gated cap before it can support any
starvation claim.

## 4. The halt helper (unchanged, still the barrier)

Native ends in `0x020090D8 → 0x02009108 → 0x02009110/0x02009118`
(`cpsr=0x9F`, `CPSR.I=1`, WFI loop) after `0x020D3250`, and that is where the
60M-step run parks (`halt_pc=0x02009110`). PHASE5F's melonDS measurement
(ARM9 trace window `0x020090D8..0x020090E0`, 90 s, 0 hits) stands: the
reference never executes that helper.

Entry into it is *not* IRQ-mask related (proved above: the card burst is
identical). The control-flow input that differs is the card-request feed after
the last common block: native issues ~12 dummy `0xB8` reads and stops, while
melonDS continues with `cmd=0xB70DA446…` and 550 dummy reads. The guest state
machine at `0x020D3250` selects the halt case only after that feed ends.

## 5. Verification (branch code)

| check | result |
|---|---|
| 60M-step run (`p5g/f.err`) | `steps=60000001 distinct_pcs=16447 halt_pc=0x02009110`, 113 frames, 114,448 unhandled io writes |
| card IRQ burst | 24 deliveries with `ie=0x000C0009 if=0x00080000` (steps 18,389,760 … 18,391,807) |
| melonDS IE write history | 66,104 accesses; bit 19 written exactly once (`ts=28,263,167`) and cleared once (`ts=28,524,002`) |
| window trace comparison | 183-instruction common prefix after the enable; first difference = IRQ-vector entry phase |
| `cargo test` (nnktool) | **21 passed** |
| `SKIP_MELONDS=1 bash tools/verify_phase2a.sh` | see §7 |
| pictures | still uniform; `shots/p5g_f113_{top,bot}.png` (443 B each) |

## 6. Tooling added

`tools/add_iotrace_window.py` — idempotent patch that adds
`MELONDS_IOTRACE_LO/HI` to the melonDS ARM9 IO trace (`ARM9IOTrace::Log`), so
a single register (IE) can be traced for a whole boot without the DISPSTAT
poll noise consuming the line budget. Rebuild with
`make -j -C /home/hermes/tools/melonDS/build-ptrace melonDS`.

Reproduce:

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5g
cd /home/hermes/NiNoKuni/worktrees/ie-mask && ln -sfn /home/hermes/NiNoKuni/work work
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c SPLIT_DIR=work/split/f \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5g

# native IE programming
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=60000000 NNK_DEBUG_AT_PC=0x020060B4 NNK_DEBUG_N=20000 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  /home/hermes/NiNoKuni/out/split/p5g work/phase3a/arm9_stored_padded.bin 2> $S/n1.err

# melonDS IE write history (needs tools/add_iotrace_window.py in the tree)
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg SDL_AUDIODRIVER=dummy \
  MELONDS_IOTRACE_PATH=$S/ie_writes.txt MELONDS_IOTRACE_LO=0x04000210 \
  MELONDS_IOTRACE_HI=0x04000210 MELONDS_IOTRACE_N=200000 \
  timeout 60 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"

# window comparison (see tools/ add_* patches for the trace hooks)
python3 $S/cmpwin.py $S/native_win.txt $S/mel_win2.txt --anchor 0x020060b4
```

## 7. verify_phase2a.sh result

`SKIP_MELONDS=1 bash tools/verify_phase2a.sh` — **PASS**:

```
1) strict common prefix        : 1000 / 1000 melonDS PCs (100.0%)
   melonDS trace fully reproduced
2) window-filtered prefix      : 1000 / 1000 (window 0x02000000..0x02400000)
3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
```

## 8. Next barrier (named)

**Follow-up barrier C — the card-request feed after the last common block.**
Native stops issuing card blocks (12 dummy `0xB8` reads) where melonDS sends
`0xB70DA446` + 550 dummy reads; the guest's Thumb state machine at `0x020D3250`
then takes the halt case (`0x020090D8`) that melonDS never reaches. Next step:
diff the guest's request queue / block index at the last common `0xB8` block
(native step 38,924,880, `op=0xB7 addr=0x02CF2C00`) against melonDS's 1,876th
B7 (`ts=100,800,219`) and identify the state variable that decides whether a
further block is issued — the answer sits in the ARM9 driver at `0x02016F34`
(the only caller of the card-IRQ setup) and `0x02016930` (the transfer-end
path), not in the IRQ mask.
