# Phase-5X — the post-skip halt: a PIC base register decoded as an ARM target

Follows Phase-5V (`9b620ab`).  The operator's live session skipped the intro with
a key press and ran to step 409,074,823, where the runtime halted with
`untranslated pc=0x00000000`, `halt_pc=0x020DDF08` and a DESTROYED register file
(`r13=0 r14=0`, `r3=0x020DDD9D`).  This phase reproduced that halt headlessly,
named its writer, fixed it, and moved the frontier — and in doing so found the
next barrier (VRAM was write-only to the guest).

## 1. Reproduction (headless, exact register match)

The operator's exact injection (`z` = DS B at 126,000,000, `x` = DS A at
510,000,000) does **not** reproduce it headlessly; neither does a single early
press.  What reproduces it is a *repeated* frame-timed press: the guest polls
KEYINPUT about once per display frame (~556 k steps), so a 20 k-step tap is
usually invisible, and each press advances one prompt.

Canonical repro (deterministic, identical register state to the operator's):

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5x
INJ="$(bash -c 's=5000000; o=""; while [ $s -lt 510000000 ]; do \
  o="$o$s:Return:down;$((s+10000000)):Return:up;"; s=$((s+20000000)); done; echo "$o"')"
cd /tmp/nnk_movie
NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
NNK_STEP_LIMIT=520000000 \
NNK_SMC_IMAGE="0x02000000:arm9.bin,0x020C0000:ovl_p4f4.bin" \
NNK_INPUT_INJECT="$INJ" ./nnk_runner arm9_stored_padded.bin
```

Result (baseline runner = master `9b620ab`):

```
untranslated pc=0x00000000
halt state: r0=00000000 r1=00000000 r2=00000020 r3=020DDD9D r4..r7=0
            r7=00000018 r13=00000000 r14=00000000 nzcv=0000
done steps=331706950 distinct_pcs=23065 halt_pc=0x020DDF08
```

That is the operator's halt word for word (`r3=020DDD9D`, `r2=0x20`, `r7=0x18`,
DISPCNT A `0x80211810`).  `NNK_INPUT_TRACE` confirms the guest reads
`0x03F7`/`0x03FF` and that presses 20 M steps apart are each seen.

## 2. The writer: `smc_state_from_regs()` read a PIC base as a target

`NNK_SMC_TRACE` on the same run names the whole divergence (276 on-demand
decodes):

```
step=331706921 pc=0x020DDE9A word=0x88DB447B state=thumb -> 0x020DDE9C
step=331706922 pc=0x020DDE9C word=0x041B88DB state=arm   -> 0x020DDEA0   <-- ARM!
...
step=331706949 pc=0x020DDF08 word=0xE838F753 state=arm   -> 0x00000000
```

and `NNK_DEBUG_AT_PC=0x020DDE9C` gives the cause:

```
dbg1 step=331706922 pc=020DDE9C r3=020DDE9C r13=027E3A78 r14=0207A23C
```

`0x020DDE9A` is Thumb `add r3, pc` (hw `0x447B`), which writes
`Align(pc+4,4) = 0x020DDE9C` into r3.  The very next instruction *is*
0x020DDE9C, so `smc_state_from_regs()` found "a register whose value equals
this pc" (its interworking-target test), saw an EVEN value and forced ARM
state.  The ARM reading of the Thumb jump-table prologue walked 4-byte steps
down the halfword table (`0x88DB 0x041B 0x141B 0x449F 0x0006 ...`) until the
ARM decode of `f753 e838` at 0x020DDF08 branched to 0 — i.e. the destroyed
register file was a SYMPTOM, the mode misclassification was the cause.

## 3. Fix (class 35) and the measured effect

`tools/nnk_smc.inc`:

* a one-dispatch "PIC base register" tag (`smc_note_pcbase`) set by the Thumb
  `add rd, pc` / `mov rd, pc` forms, honoured in `smc_state_from_regs()`;
* the Thumb `bx/blx rM` arm now carries the interworking state it transfers to
  (`bx` -> bit0, `blx` -> ARM), and the `add pc,rn` / `mov pc,rn` forms carry
  Thumb — they carried nothing before.

Measured effect at the same step:

| run | halt | register file |
|-----|------|---------------|
| before | 331,706,950 `pc=0x020DDF08` -> 0 | destroyed (r13=r14=0) |
| after  | 331,707,217 `blx r4` -> `0xE3A03102` | healthy (r13=027E3A30, r14=020E1B35) |

267 more instructions, and the Thumb stream past 0x020DDF08 now executes in
Thumb state (`NNK_SMC_TRACE`: `state=thumb` for every decode in the window).

## 4. The next barrier: VRAM is write-only to the guest

`blx r4` at 0x020E1B32 with `r4 = 0xE3A03102` is the *next* independent defect.
The constructor at 0x020E1AC4 keeps a C++ object at 0x06006000 and stores its
vptr with `str r0,[r5]`; the virtual call reads `[0x06006004]` back and gets 0.

Cause (measured): `gxd_io_write` routes palette/VRAM/OAM into its VRAMCNT
banks, but `gxd_io_read` serves only KEYINPUT/KEYCNT/VRAMSTAT and returns 0 for
every window byte, and the runtime's `ld32/ld16/ld8` never looked at the window
at all — so a guest read of VRAM always returned 0.

Fix: a lazily allocated read shadow for the three windows in the guest accessor
template (`nnktool/src/recompile.rs`), written on every store next to
`gxd_io_write` (rendering path untouched) and consulted by the loads.
`tools/p5x_runtime_splice.py` applies the identical transform to a frozen image
for measurement without a recompile-all round trip.

## 5. `NNK_INPUT_FILE` (frame-timed input, DQ8 analogue)

`tools/nnk_input.inc` gained `NNK_INPUT_FILE`: the host writes a desired
active-low mask, re-read once per DISPLAY FRAME (gated on `gxd_frames`),
edge-logged under `NNK_INPUT_TRACE`.  Bits: 0-9 = KEYINPUT (idle `0x3FF`,
A=`0x3FE`, B=`0x3FD`, Start=`0x3F7`), 10/11 = DS X/Y.  This is the reproducible
form of a human session: the operator taps when he SEES a prompt, which
step-timed injection cannot express.

**Still to do (operator's scripted form):** accept a SCRIPT as well as a single
value — one `<frame> <mask>` entry per line (frame decimal, mask hex, `#`
comments, `end`/EOF holds), parsed once per file change and applied in order as
the frame counter passes each entry, with a delta trace per entry and a
one-line exit summary (entries applied, last frame).

## 6. Verification

```
cd nnktool && cargo test        -> 36 passed; 0 failed    (class 35 + input-file lock)
SKIP_MELONDS=1 bash tools/verify_phase2a.sh
```

The three measured facts class 35 locks: the one-dispatch PIC-base tag, its
consultation inside `smc_state_from_regs`, the Thumb `bx/blx` state carry, and
the geometry `Align(0x020DDE9E,4) == 0x020DDE9C`.  Class 36 locks the
`NNK_INPUT_FILE` contract (frame gate, mask trim, edge-only publish, mask
arithmetic).

## 7. Open frontier after this phase

1. Measure the run with the VRAM shadow (spliced image) to the previous
   frontier and beyond — the harness is
   `scratch/p5x/verify2.sh`; its first run halts at the *next* untranslated PC.
2. Implement the scripted `NNK_INPUT_FILE` (section 5) and replay the
   operator's session: B on the notice screen, A to skip the movie, with the
   frame numbers measured from `NNK_INPUT_TRACE`.
3. Frames keep presenting on the post-skip path (598 presented at step
   331.7 M), so screenshots are available once the frontier passes 409 M.
