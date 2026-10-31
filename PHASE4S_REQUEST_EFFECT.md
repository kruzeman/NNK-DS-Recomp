# Phase-4S — the ARM7 request EFFECT: the card phase behind the requests, sequence word wakes the queue

Follows Phase-4R (merged `4bf5c9a`).  Scope: measure, with melonDS ground
truth, what the ARM7 *does* behind the class-7 card requests the guest sends,
implement that effect in the scripted ARM7 responder, and push the guest past
the SDK queue wait that Phase-4R left blocked.

## 0. Headline

* **The Phase-4R barrier is one shared-RAM word: the SDK mailbox sequence
  `*(0x0206CAA0)`.**  The wait at `0x0200ACC8` compares the snapshot
  `*(0x0206CA5C)` against `*(*(0x0206E580))`, and the ARM9's own init (pc
  `0x0200AC78`, run right after the reply `0xC0204006`) installs the pointer
  `*(0x0206E580) = 0x0206CAA0`.  **melonDS's ARM7 performs the requested card
  phase and increments that word** (writer pc7 = `0x03802590`, ARM7 WRAM) —
  the wait exits one poll period (~89..109 ts) later.  Native acked the
  request (Phase-4Q) but never did the work, so the sequence stayed 0.
* **Implemented and measured: the sequence now advances** `0x00000000 ->
  0x00000001 -> 0x00000002 -> 0x00000003` at the measured offsets, the wait
  wakes, and `distinct_pcs` grows **11,371 -> 13,828 (+2,457)**; the guest
  leaves the wait for the card-stream load and runs bulk LZ decompression
  (`0x02000984..0x020009D0`) plus the VBlank wait (`0x020078E8..0x020078FC`)
  to the end of a 60M-step budget.
* **Frames are still uniform, honestly** (§4): every `NNK_GX_SHOT` capture is
  single-valued (0x00 at frames 10-20, 0xFF from 30 on) — the backdrop.
  First differing word named in §5: **DISPCNT**.

## 1. Request classes (melonDS send vocabulary of `W32 0x04000188`, p4q_mel_io.txt)

| word | class `(w & 0x1F)` | n | meaning |
|---|---|---|---|
| `0x037A404D` | 13 | 1 | Phase-4C one-shot (scripted) |
| `0x80004046`, `0x40400006` | 6 | 1 pair | driver IPC start + "notify me"; reply `0xC0204006` (Phase-4Q) |
| `0x81B34807`, `0x81B34E07` | **7** | 1 pair | **card request — the effect measured here** |
| `0x81B36C07`, `0x81B39C07` | **7** | 1 each | card request, one per 100 Hz tick |
| `0x81BA702B`, `0x000001AB` x3, `0x000000AB`, `0x0000002B` | 11 | 6 | one-shots, no reply in the capture |

Class-7 words are sent from pc `0x0200BAD4` by the API at `0x0200FA38`
(two words per call), and the guest then calls the blocking SDK wait
`0x0200ACC8` from pc `0x02056624`.

## 2. The full ARM7-side cycle (melonDS, new instruments)

Two probes were added to the local melonDS (`/home/hermes/tools/melonDS`,
`build-ptrace`, env-gated): a MemWatch window on the wait/queue objects that
logs **both CPUs with `ts=`**, and `reg ts=` on the existing register probe.

| event | ts (ARM9 cycles) |
|---|---|
| reply `0xC0204006` read (t0) | 28,709,423 |
| ARM9 `*(0x0206E580) = 0x0206CAA0`, queue init at pc `0x0200B418` | 28,717,247 |
| send `0x81B34807` (class 7) | 28,720,406 |
| send `0x81B34E07` (class 7) | 28,732,873 |
| wait `0x0200ACC8` entered (r0=1 blocking) | 28,732,934 |
| **ARM7 writes `*(0x0206CAA0)` 0 -> 1** (pc7 `0x03802590`) | **28,873,928 (t0+164,505)** |
| wait exits, r0=1 r1=0 | 28,874,017 |
| ARM7 writes 1 -> 2 | 29,213,542 (t0+504,119) |
| ARM7 writes 2 -> 3 | 29,548,696 (t0+839,273) |
| ARM7 writes 3 -> 4 | 29,883,744 (t0+1,174,321) |

* The wait's own delay chunk is **109 ts** (regprobe compares at
  28,733,053 / 28,733,162 / ...), so every exit lands 89..109 ts after the
  write — one poll period, no IRQ and no IPC message in between.
* Increments 2..4 ride the ARM7's 100 Hz tick (335,104 ts, Phase-4R): they
  land +396 / +446 / +390 ts after each `0x00008007`.  The request that
  follows each increment is the guest's *answer* to that tick (`0x81B36C07`
  at 29,218,077 = tick1 + 5,061), so the completion of a given request lands
  on the **next** tick slot — a fixed "send + 330K" delay does not fit
  (measured 330,619 vs 331,041), the next-tick rule does (±57 ts).
* After wait #1 the ARM9 runs the card phase melonDS shows: EXMEMCNT bus
  switch at 29,024,941 (`0xE080` = ARM7 owns, helper pc `0x020064CC`),
  first ROMCTRL command `0xA1416657` (B7, 0x200) at 29,035,001, then the
  `0xA7416000` poll cycle alternating with `0xE880`/`0xE080` every ~2,950 ts.

## 3. The fix (`tools/nnk_dma.inc`, Phase-4S block)

`nnk_ipc_send` now classifies `(v & 0x1F) == 7` as a card request and calls
`nnk_p4s_request`:

1. First class-7 request after the reply: schedule completion **A** at
   `t0 + 164,505` (t0 = the reply delivery cycle, i.e. the measured
   `28,873,928 - 28,709,423`) and completion **B** at the next 100 Hz tick
   slot `stream[1].next + 396`.
2. Later requests: if no completion is pending, schedule one at the next tick
   slot (`stream[1].next + 396`) — the measured C/D rule.
3. Each completion (`nnk_p4s_fire`) does the *real* write the ARM7 does:
   `p = *(0x0206E580); st32(p ? p : 0x0206CAA0, ld32(...) + 1)` — no fake,
   no forced guest state.  The Phase-4R probe now prints `ptr=` and `seqobj=`.
4. Ordering guard: completions are kept sorted, max 4 pending.

## 4. Result (this branch, 30M and 60M step runs)

```
ipc: ARM7 request 0x80004046 -> reply 0xC0204006 in 2076 cycles step=18554801
p4s: class-7 card request 0x81B34807 -> completion at +164505 (measured A/B) step=18568551
p4s: ARM7 card-phase effect seq 0x00000000 -> 0x00000001 obj=0x0206CAA0 ptr=0x0206CAA0 snap=0x00000000 step=18721367
p4s: ARM7 card-phase effect seq 0x00000001 -> 0x00000002 obj=0x0206CAA0 ptr=0x0206CAA0 snap=0x00000001 step=19060949
p4s: ARM7 card-phase effect seq 0x00000002 -> 0x00000003 obj=0x0206CAA0 ptr=0x0206CAA0 snap=0x00000002 step=19396037
done steps=60000001 distinct_pcs=13828 hle_hits=7251 halt_pc=0x02016974
gx: 113 frames presented, 30675 unhandled io writes
```

* **Sequence advances 0 -> 1 -> 2 -> 3 and the SDK queue wait wakes**
  (Phase-4R: `distinct_pcs` frozen at 11,371 from 30M to 60M; now 13,828 and
  the wait region appears only 156 times in the last 2M-step ring, i.e. the
  guest is out of it).
* `ptr=0x0206CAA0` — the native run *does* install the sequence pointer, so
  the getter and the ARM7's write target agree.
* Guest progress after the wake: bulk LZ decompression (`0x02000984..9D0`,
  ~137K hits per group in the tail ring) + VBlank wait + card commands
  (`halt_pc=0x02016974`, r1=`0x040001A4` r2=`0xA7416657`).
* **Frames: uniform, honestly.**  `p4s_sh.*.ppm` (30M, frames 10-50) and
  `p4s6_sh.*.ppm` (60M, frames 20-100) are all single-valued: 0x00 at frames
  10-20, 0xFF from 30 on.  VRAM accounting at the end: all 327,680 stores hit
  **LCDC**, `ABG=0 BBG=0 AOBJ=0 BOBJ=0` — no tile/pixel data has reached a
  display bank yet, so the backdrop is correct-for-now, not a bug by itself.
* `SKIP_MELONDS=1 bash tools/verify_phase2a.sh`: **green, rc=0,
  `RESULT: PASS`, strict common prefix 1,000/1,000** (log `p4s_verify.log`).

## 5. Honest stop-report — first differing word + the next barrier

* **First differing word (exact, measured):** `DISPCNT (0x04000000)`.
  melonDS writes **`0x0021021B` at ts 54,276,936 (pc `0x0207CB10`)** and
  **`0x0021001B` at ts 56,479,983 (pc `0x0207D30C`)**; native's final state at
  the 60M budget is still **`0x0021111B`** (`vrc: DISPCNT A=0x0021111B`), and
  native presents 113 frames — the same frame window in which melonDS makes
  those two writes (54.3M ts / 56.5M ts ≈ frames 97 and 101).
* **Class:** the post-load display transition — the writes come from code at
  `0x0207CB10` / `0x0207D30C`, i.e. **outside the 445,304-byte base image**
  (`work/ghidra_src/arm9.bin` ends at `0x0206C5C8`): overlay-resident code.
  Native never executes it, which is also why no VRAM store ever lands in
  ABG/BBG/AOBJ/BOBJ (all 327,680 go to LCDC).
* **Named follow-up barrier (bounded next step):** native and melonDS are both
  in the card-stream load at this point, but native's `distinct_pcs` freezes
  at 13,828 from the 30M run to the 60M run and the tail ring is dominated by
  the LZ decompressor + VBlank wait, so either (a) the load needs far more
  steps than 60M in this build (measure: run 150M+ and watch `distinct_pcs`
  and `vrc: ABG`), or (b) the overlay that owns `0x0207CB10` is never
  activated (MemWatch both runs on `0x0207C000..0x0207E000` and on the
  overlay-header load, and compare `VCOUNT/DISPCNT` writes in the same frame
  window).  Either way the next card is "the overlay/display transition", not
  the IPC path.

## 6. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
cd <worktree>; ln -s /home/hermes/NiNoKuni/work work     # already linked
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c bash tools/build_split.sh out/split/arm9_gx

NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_GX=headless \
  NNK_STEP_LIMIT=60000000 NNK_GX_SHOT=$S/p4s6_sh NNK_GX_SHOT_EVERY=20 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin   # -> p4s_run60.{log,err}

SKIP_MELONDS=1 bash tools/verify_phase2a.sh               # rc=0, PASS
```

melonDS ground truth (both instruments are env-gated, `build-ptrace` rebuilt):

```sh
# queue/sequence writer, both CPUs, with ts:
MELONDS_MEMWATCH_PATH=$S/p4s_mw2.txt MELONDS_MEMWATCH_N=400000 \
  MELONDS_MEMWATCH_LO=0x0206CA9C MELONDS_MEMWATCH_HI=0x0206CAE0 ...
# the wait itself (entry, compares, exits, with ts):
MELONDS_REGPROBE_PATH=$S/p4s_rp3.txt MELONDS_REGPROBE_N=6000 \
  MELONDS_REGPROBE_PCS=0x0200ACEC,0x0200AD1C,0x0200AD44,0x020064CC ...
```

Captures: `p4s_mw2.txt` (MemWatch + ts), `p4s_rp3.txt` (regprobe + ts),
`p4q_mel_io.txt` (IO trace, Phase-4Q), `p4s_run60.{log,err}` (native),
`p4s_verify.log` (green).
