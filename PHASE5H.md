# Phase-5H — there is no 0xB8-vs-0xB7 command divergence; the card feed is an exact 9,093-command prefix and the guest itself leaves the poll loop for the 0x020D3250 halt branch

Follows Phase-5G (merged `6a30c47`). Scope: name the first differing card COMMAND
write (guest request path vs runtime ROMCTRL/DRQ model), feed the 0xB7 block
stream, and reach the melonDS frames-102 content.

**Headline (all measured on this branch):**

1. **The stated first differing command does not exist.** Native and melonDS
   agree on **every one of the first 9,093 card transfer starts** — the full
   `(opcode, ROM address, size)` tuple, in order, with no divergence at all
   (`tools/card_cmdseq.py`, §1). Native issues 9,093 starts (1,876 `0xB7`
   block reads + 7,217 `0xB8` dummy reads); melonDS issues 9,840 in the same
   prefix-plus-continuation (2,461 / 7,379). The premise's "~12 dummy `0xB8`
   reads where melonDS sends `0xB7` + 550 reads" compares native's **tail**
   with melonDS's **middle**: melonDS's 550 `romdata` reads at that point are
   its normal `0xB8` poll cadence (one dummy pair every ~5.9k cycles), which
   native executes identically as far as it gets.
2. **The first differing word is card start #9093.** melonDS's start #9093 is
   `cmd=B70DA44600000000` (`ROMCNT=0xA1416657`, `pc=0x02016930`,
   `ts=103,577,894`), followed by the sequential 0x200-byte stream
   `0x0DA44800, 0x0DA44A00, …`. Native has **no** start #9093: after its
   start #9092 (the same `0xB8` dummy melonDS also emits) the guest leaves the
   card request path and, at **step 39,238,888**, executes the SDK halt helper
   **`0x020090D8` with `lr=0x020D3255`** — i.e. the Thumb state machine at
   `0x020D3255` (the very band the task names, `0x020D3250`) calls it with
   `r0=0x00007FFF r1=0x00000006 r2=0 r3=0x00000003`.
3. **The card model is exonerated, measured.** At the last common block the
   model is clean: `card_next=NEVER`, `romcnt=0x27416000` (busy clear),
   `card_pos=0/0`, `card_count=0`, `card_drq=0` — nothing pending, nothing
   starved. The last `0xB7` block (`0x02CF2C00`) delivered **all 128 words**
   (`n=128`, `crc=0x7B86CFA6CEF624E3`). The class is the **guest's request
   producer / control flow**, not the Phase-4D/4L ROMCTRL interpretation.
4. **Both engines run the same idle loop at the stall.** melonDS's ARM9 trace
   ends at `0x0200911C`, `ts=103,550,618` in exactly the loop native spins in
   (`0x02007134 → 0x02009118 → 0x0200911C → 0x02009120`, 5 instructions), and
   melonDS's `WFI` halts there until an interrupt at `ts=103,577,894` — 3
   cycles before the `0xB7 0x0DA44600` start. `0x02009118` is literally the
   WFI instruction: `mov r0,#0; mcr p15,0,r0,c7,c0,4; bx lr` (CP15 c7,c0,4).
   Being in that helper is *normal*; Phase-5F/5G's "native parks in a
   melonDS-never-reached helper" is true only of the `0x020090D8 →
   0x02009108` variant (CPSR.I=1, IE bits cleared), which is what native
   reaches at step 39.24M and melonDS never does.

## 1. The card command stream, both engines

Native (`p5h/../p5g/f.err`, `p4t: start` lines, uncompressed) vs melonDS
(`p5h/mel_card.log`, `MELONDS_CARDLOG_PATH` from `tools/melonds-cardlog.patch`):

```
$ python3 tools/card_cmdseq.py f.err mel_card.log
native   starts=9093  B7=1876  B8=7217
melonDS  starts=9840  B7=2461  B8=7379
first (op,addr,size) divergence at index none of 9093 compared
  idx | native (op addr size step cyc)          | melonDS (op addr size ts)
 9090 | B8 00000000     4   38978201   68659228 | B8 00000000     4    100946089
 9091 | B8 00000000     4   38978306   68659228 | B8 00000000     4    100946478
 9092 | -                                       | B7 0DA44600   512    103577894
```

(Indices are 0-based in the tool; native's 9,093 starts are indices 0..9092 and
all match melonDS's 0..9092. The next melonDS start is #9093, the
`0x0DA44600` block read.)

So the answer to the task's question "who diverges — the guest's request path
or the runtime's command handling?" is: **neither the ROMCTRL model nor the
command decode.** The guest never writes command #9093. Before it can, the
`0x020D3254` state machine takes its halt branch.

melonDS's continuation after #9093 is a plain sequential 0x200-byte stream in
6 blocks (`0x0DA44600, 0x0DA44800, 0x0DA44A00, 0x0DA44C00, 0x0DA44E00,
0x0DA45000, …`), exactly the `0xB7` block shape (`ROMCNT=0xA1416657`,
`size=0x200`, `irqen=1`) native already serves correctly 1,876 times.

## 2. Where native stops, in its own units

```
p4t: blk addr=0x02CF2C00 crc=0x7B86CFA6CEF624E3 n=128 steps=38931049   <- last full block
p4t: start op=0xB8 … steps=38959362                                    <- 14 dummy B8 (7 pairs)
p4t: start op=0xB8 … steps=38978306                                    <- last start (#9092)
end  step=38978350 cyc=68659300 pc=0x02016984 polls=14                 <- last card event, clean
NNK_HALT_AT_PC=0x020090D8: done steps=39238888 distinct_pcs=16437 halt_pc=0x020090D8
   halt state: r0=00007FFF r1=00000006 r2=00000000 r3=00000003 r4=00000020
               r7=02069688 r13=027E3B30 r14=020D3255
```

`lr=0x020D3255` names the caller: Thumb code at `0x020D3254` inside the
`0x020D32xx` band. That is the same "SDK card-driver state machine at
0x020D3250" Phase-5F/5G named, now pinned to an instruction and an argument
set (`r0=0x7FFF`, `r1=6`).

## 3. Idle-loop state, both engines (the wake question)

Native, `NNK_WFI_PROBE=1 NNK_WFI_LO=39100000` (new step gate, §5) — 20,000
samples at the WFI helper inside the 39.10M…39.20M window, all identical:

```
wfiprobe pc=0x02009118 step=39100003 ime=1 ie=0x00040019 if=0x00080000 cpsr=1F
          cardnext=NEVER romcnt=0x27416000 spicnt=0xC000 dispstat=0x0008
```

* `dispstat=0x0008` → VBlank IRQ **enabled**, `ime=1`, `cpsr=1F` → IRQ
  unmasked, `IE=0x00040019` (VBlank, Timer0/1, cart bit 18) — the same mask
  melonDS runs with at the same loop (`ie=0x00040019` in every `irqwake`).
* `IF=0x00080000` — the cart-transfer-complete bit stays pending (masked),
  exactly as Phase-5G measured. `IE&IF = 0` here, so the spin is expected
  between events; native's own IRQ trace shows delivery continues
  (last `irq enter step=39192472 if=0x00080010` in the 46M run), so the
  wake producer is **not** starved either.
* After step 39,147,313 the same probe returns `ie=0x00040009 if=0x00080000
  cpsr=9F` on `pc=0x02009110/0x02009118` — the `0x02009108` halt loop with
  IRQ disabled, which is the state the `0x020090D8` callee (`0x020090D8 →
  bl 0x02009108`) puts the CPU in.

melonDS wakes from the same loop (`p5h/mel_wake.log`,
`MELONDS_LOOPWATCH_PATH`, `pc9=0x0200911C`):

```
irqwake ts=206021412 ie=0x00040019 if=0x00280010 enabled=0x00000010   (Timer1)
irqwake ts=206155274 …           if=0x002C0000 enabled=0x00040000     (bit 18)
irqwake ts=206967840 …           if=0x00280001 enabled=0x00000001     (VBlank)
irqwake ts=207212430 …           if=0x002C0000 enabled=0x00040000  -> IRQ vector
       0xFFFF0018/0xFFFF06D8 (BIOS) -> 0x01FF8000 (ITCM user vector) -> 0x0200BAE8
```

(ts here is raw `ARM9Timestamp`, 2× the IO-trace ts per Phase-5G, so this is
io-ts 103.1M…103.6M — the stall window.) Native raises and services the same
three sources (`if=0x00080001/0x00080010/0x000C0000` all appear in its IRQ
trace), so the wake *sources* match too. What differs is the guest-side
consumer: in melonDS the woken thread eventually re-arms the card block read;
in native the `0x020D3254` state machine instead calls `0x020090D8`.

## 4. Verification (branch code)

| check | result |
|---|---|
| card command stream | native 9,093 starts = melonDS 0..9092 exactly (op/addr/size); first missing = #9093 `B7 0DA44600` |
| last native block | `0x02CF2C00`, `n=128`, `crc=0x7B86CFA6CEF624E3` (complete, not truncated) |
| card state at the stall | `card_next=NEVER romcnt=0x27416000 count=0 drq=0` — nothing pending |
| halt-path entry | `0x020090D8` at step **39,238,888**, `lr=0x020D3255`, `r0=0x7FFF r1=6` |
| native idle state | `ime=1 ie=0x00040019 if=0x00080000 cpsr=1F dispstat=0x0008` (39.10–39.20M) |
| melonDS idle state | same loop `0x0200911C`, `ie=0x00040019`, halts at `ts=103,550,618`, wakes at `103,577,894` |
| 60M-step run | `distinct_pcs=16447` @60M (`16355` @39.16M) — progress is slow, not frozen |
| `cargo test` (nnktool) | 21 passed (see §6) |
| pictures | still uniform; no new non-uniform frame (stop-report path) |

## 5. Tooling added

1. `tools/card_cmdseq.py` — the command-sequence differ used above (native
   `p4t: start` vs melonDS `start … cmd=`), reporting the first differing
   `(op, addr, size)` tuple and the 0xB7-only address stream.
2. `NNK_WFI_LO=<step>` in `tools/split_dispatch.py` — the Phase-5F WFI state
   probe (which dumps `ime/ie/if/cpsr/cardnext/romcnt/spicnt/dma2/dispstat`)
   consumed its 20,000-hit cap at step 23,490,852, so the *late* idle-loop
   state (the barrier at 39M+) could never be sampled. The probe window is now
   data: `NNK_WFI_PROBE=1 NNK_WFI_LO=39100000`. (Phase-5G's conclusion that the
   20,000-cap probe "needs a time- or frame-gated cap" — this is that cap.)

Reproduce:

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch/p5h
cd /home/hermes/NiNoKuni/worktrees/card-starve && ln -sfn /home/hermes/NiNoKuni/work work
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c SPLIT_DIR=work/split/h \
  bash tools/build_split.sh /home/hermes/NiNoKuni/out/split/p5h          # ~40 s

# native: card events + halt-path entry + late idle state
env NNK_ROM_PATH='/home/hermes/NiNoKuni/Ni No Kuni.nds' NNK_GX=headless \
  NNK_STEP_LIMIT=60000000 NNK_HALT_AT_PC=0x020090D8 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  /home/hermes/NiNoKuni/out/split/p5h work/phase3a/arm9_stored_padded.bin 2> $S/n.err
env … NNK_WFI_PROBE=1 NNK_WFI_LO=39100000 … 2> $S/wfi.err

# melonDS: card log + idle-loop wakes
env XDG_CONFIG_HOME=$HOME/.hermes/profiles/coder/cache/scratch/melonds_cfg SDL_AUDIODRIVER=dummy \
  MELONDS_CARDLOG_PATH=$S/mel_card.log timeout 70 xvfb-run -a \
  /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
env … MELONDS_LOOPWATCH_PATH=$S/mel_wake.log MELONDS_LOOPWATCH_AFTER=80 …  (15 s)

python3 tools/card_cmdseq.py $S/n.err $S/mel_card.log
```

## 6. Next barrier (named)

**Follow-up barrier D — the `0x020D3254` state machine's halt branch.**
The card bus, the ROMCTRL/DRQ model, the IRQ mask and every IRQ producer are
measured correct up to the last common command; the guest's `0x020D32xx`
Thumb dispatcher decides "stop reading" one request before melonDS's. The next
step is to diff the *state word that dispatcher tests* at the last common
`0xB8` (native step 38,978,306, melonDS io-ts 100,946,478) — i.e. a RAM diff
of the `0x020D3250` handler's inputs (a queue/thread-state word, most likely
the only remaining scripted ARM7/IPC effect in play), not another card-clock
change. Exact anchor for the next worker: `lr=0x020D3255`, `r0=0x00007FFF`,
`r1=6` at `0x020090D8`, step 39,238,888; disassemble the Thumb block
`0x020D3200..0x020D3260` from the resident overlay (arena `0x020D0A00`,
overlays 17/18) to name the tested word.
