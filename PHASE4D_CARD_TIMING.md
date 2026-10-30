# Phase-4D — ROMCTRL card-transfer timing + the 0x4E / SDK-shutdown barrier

Follows Phase-4C2 (merged `d44e5a4`, strict prefix 3,638,915).
Scope: (1) implement melonDS's card-bus transfer timing, (2) resolve the
unscripted ARM7 IPCFIFO request `0x0000004E` and the SDK shutdown sequence,
(3) push the boot toward display init.

## 1. New ground-truth instrument: a timestamped IO bus trace

The Phase-3A IO hook (`tools/melonds-io-trace.patch`) logged address, value and
PC of every ARM9 bus access but not *when*, so card transfer timing could only
be inferred from PC counts. `tools/add_iotrace_ts.py` adds `ts=` —
`NDS::ARM9Timestamp >> ARM9ClockShift`, the same unit `NDS::ScheduleEvent`
takes — to every trace line. Rebuild `build-ptrace/`, then:

```sh
MELONDS_IOTRACE_PATH=$S/p4d_io_ts.txt MELONDS_IOTRACE_N=400000 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "<ROM>"
```

Capture used: 400,000 accesses (~9.8M ARM9 PCs), `scratch/p4d_io_ts.txt`.
Analysis tools (committed): `tools/card_polls.py` (per-transfer poll counts),
`tools/card_words.py` (per-word poll counts), `tools/card_timeline.py`
(ts/dts timeline), `tools/card_commands.py` (card command + data stream).

## 2. Measured card timing (melonDS NDSCart.cpp formulas confirmed)

For the boot's steady-state card read (`ROMCnt = 0xA1416657` -> `delay13=0x657`,
`gap2=1`, `xfercycle=5`, `datasize=0x200` = 128 words; all ~170 transfers in the
capture share this shape):

| quantity | melonDS | formula (NDSCart.cpp) |
|---|---|---|
| cmddelay | 1632 | `8 + (ROMCnt & 0x1FFF) + ((ROMCnt>>16)&0x3F)` = 8+1623+1 |
| first word | 8180 cycles | `xfercycle*(cmddelay+4)` = 5*1636 |
| word-to-word | 20 cycles | `xfercycle*(4 [+gap2 at pos%0x200==0])` |
| ROMCNT polls before first DRQ | **909** | 8180 / 9-cycle poll iteration |
| guest poll iteration | **9 cycles** | dts poll->poll 9 |
| DRQ-poll -> ROMDATA read | 6 cycles | dts 6 |
| ROMDATA read -> next poll | 9 cycles | dts 9 |
| per-word poll counts (incl. the DRQ poll) | 2,2,1 repeating | emergent |

The ROMCNT write that starts a transfer is `0xA1416657`; DRQ (bit23) is raised
only by the scheduled `ROMTransfer_ReceiveData`, never at command time — this is
the "premature DRQ" bug of Phase-4C2, confirmed and fixed.

## 3. Implemented in the runtime (`nnktool/src/recompile.rs`)

* `card_*` state machine transcribed from NDSCart.cpp: `WriteROMCnt`
  (bit31 0->1 edge, AUXSPICNT bit15/13 gate, `cmddelay`/`xfercycle`),
  `ROMTransfer_ReceiveData` (2-deep FIFO, `ROMDataLate`, `RaiseDRQ`),
  `ROMAdvanceReceive`, `ReadROMData` (FIFO pop, DRQ clear, resume-or-end),
  `ROMEndTransfer` (busy clear).
* A second clock `card_cycles` (the pre-existing `cycles` clock keeps driving
  VCOUNT/timers, which still trace-match) advanced by a measured
  per-instruction cost: `card_cost()` in the recompiler emits `card_tick(N);`
  at the top of every dispatch case — loads 1, stores 3, branches 3, ALU 1,
  return (`ldm` with pc) 12, from the measured loop costs above.
* `0xB8` (get cart chip id) added: melonDS answers `0x0000FEC2` for this cart
  (measured from the capture, and equal to the cart ID in the seeded BIOS boot
  params).  Before this the runtime answered `ROM[0]`, the guest's card init
  failed and took the SDK shutdown path.
* Unmodelled card opcodes and an unscripted IPCFIFO request still halt-and-log
  instead of fabricating data (`io: card cmd 0x.. (0xB7/0xB8 modelled)`).

Offline model check: `tools/card_sim_model.py` (faithful transcription of the
melonDS state machine + the measured loop costs) reproduces `first = 909`.
Runtime log agrees: `io: card drq after 909 polls card_cycles=6749013`.

## 4. The 0x4E IPC request and the SDK shutdown: NOT a DRQ-timing artefact

Measured facts:

* The melonDS IO capture contains **no** IPCFIFO write after the single measured
  `0x037A404D -> 0x4D` pair within 400k accesses (~9.8M ARM9 PCs), i.e. the
  `0x0000004E` request does not exist on melonDS's path — it was a *symptom*.
* Fixing only the DRQ timing (poll 910) did **not** remove it: the run still
  reached `0x0200BAD4` (`0x4E`), then `0x020090D8`/`0x02009114` (the SDK
  shutdown halt loop) for 20M steps.
* The trigger was the card *data* of `0xB8`: the boot reads the cart chip id
  right after the first `0xB7` block (`tools/card_commands.py`: melonDS
  `ln=1971 start cmd=0xB8 romcnt=0xA7416000 word0=0x0000FEC2`). The runtime
  returned `ROM[0]`, the SDK's card init rejected the cart, and the guest
  emitted `0x4E` + shutdown.

With the measured `0xB8` answer the shutdown path and the `0x4E` request are
**gone**: the run executes 12,213,632 steps across 4,367 distinct PCs
(Phase-4C2: ~20M steps inside a 2,152-PC halt loop) with `hle_hits=7251`.

## 5. Strict prefix (honest numbers, `tools/cmp_pc_traces.py`)

```
melonDS trace : 10,000,000 PCs      native trace : 12,213,631 PCs
strict common prefix        : 3,643,455   (Phase-4C2: 3,638,915, +4,540)
first divergence at 3,643,455: melonDS 0x02016B4C vs native 0x02016B3C
in-order matched after resync: 3,645,532
```

Remaining first divergence, root-caused: still the *same* transfer, but now a
**loop-phase** offset, not a DRQ-semantics error — the runtime raises DRQ after
the measured 909 polls (log above), yet its data read lands 5 PCs (one poll
iteration) earlier in the trace than melonDS's, because the pre-loop stretch
`str ROMCnt; pop {r3,pc}; mov; ldr ROMCnt` costs 23 master cycles in melonDS's
pipeline versus 12 in the static cost table. Making the trace exact requires
melonDS's ARMv5 pipeline cost model for every instruction class, not the
measured four-class table.

## 6. New barrier in front of display init (honest stop-report)

The boot now stops at a *different* and honest barrier:

```
halt=dispatch to untranslated pc   halt_pc=0x0200DA34
```

No display frame yet: `gx_display` is not reached, no screenshot exists.
Barrier order for the next task:
1. translate/seed the trace-gap PC `0x0200DA34` (established method:
   `tools/mk_trace_seeds.py` + `nnktool recompile-all`), then re-measure;
2. exact-prefix work (pipeline cost model) if a strict-prefix number is needed;
3. display init -> `gx_display` frame capture.

## Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# ground truth with timestamps
python3 tools/add_iotrace_ts.py && (cd /home/hermes/tools/melonDS/build-ptrace && make -j12 melonDS)
MELONDS_IOTRACE_PATH=$S/p4d_io_ts.txt MELONDS_IOTRACE_N=400000 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "<ROM>"
python3 tools/card_polls.py  $S/p4d_io_ts.txt | tail -5
python3 tools/card_words.py  $S/p4d_io_ts.txt 350269 8
# runtime
nnktool recompile-all $S/itcm_blob.bin $S/itcm_image_p4d.c --base 0x01FF8000 \
    --entry 0x01FF8000 --seeds $S/seeds_itcm.tsv
nnktool recompile-all work/ghidra_src/arm9.bin work/phase3a/arm9_thumb_p4d.c \
    --base 0x02000000 --entry 0x02000800 --seeds work/phase3a/seeds_phase3a3.tsv
python3 tools/merge_itcm_cases.py work/phase3a/arm9_thumb_p4d.c \
    $S/itcm_image_p4d.c work/phase3a/arm9_thumb_itcm_p4d.c \
    $S/melonds_arm9_trace_itcm.txt
NNK_IMAGE_C=work/phase3a/arm9_thumb_itcm_p4d.c \
    NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=20000000ull" tools/build_split.sh
NNK_ROM_PATH="<ROM>" work/split/arm9_split \
    /home/hermes/NiNoKuni/work/phase3a/arm9_stored_padded.bin \
    > $S/nat_p4d3.txt 2> $S/nat_p4d3_hle.log
python3 tools/cmp_pc_traces.py $S/melonds_irq_pc.txt $S/nat_p4d3.txt \
    --lo 0x01FF8000 --hi 0x02400000
```
