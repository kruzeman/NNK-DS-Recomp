# Phase-4L — the card-data FIFO drain, measured; the untranslated-PC wall removed

Follows Phase-4K (merged `beb6ac8`).  Scope: model the card-data FIFO drain at
`0x02016B38` (ROMCNT bit23/bit31 as the guest's read loop sees them), get the
run past the card-data phase, and push toward display init.

**Outcome: the drain is measured end to end and the loop provably completes
with melonDS's data-word sequence — but the real wall was never the FIFO.  It
was the untranslated PC `0x0215C034`; with decode-on-demand for unrooted PCs
the run advances from 13,676,011 to 20,000,001 instructions (budget-exhausted,
no halt), and the display powers up: the first NON-BLACK guest frames appear
(frame 32 onward, uniform white).  No picture content yet.**

## 1. Ground truth: a new, longer capture

```sh
MELONDS_ARM9TRACE_PATH=$S/p4l_mel_pc.txt  MELONDS_ARM9TRACE_N=15000000 \
MELONDS_IOTRACE_PATH=$S/p4l_mel_io.txt    MELONDS_IOTRACE_N=1200000 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
```

15,000,000 ARM9 PCs and 1,199,999 IO accesses (`ts=` = `ARM9Timestamp >>
ARM9ClockShift`, the Phase-4D instrument).  It covers 936 complete 128-word
card transfers, i.e. the whole card-data phase, not just its start.

## 2. The drain sequence as the guest's loop sees it (melonDS)

`tools/p4l_io_drain.py` (`scratch/p4l_io_drain.py`, per-transfer view) on the
first transfer, `ROMCnt = 0xA1416657`, `datasize = 0x200`, cmd `0xB7`:

| quantity | measured |
|---|---|
| ROMCNT value at every poll while busy | `0xA1416657` — **bit31 = 1, bit23 = 0** |
| first poll with bit23 set | the **910th** ROMCNT read (0-based 909) |
| polls with bit23 set | **128**, one per data word |
| `ts` poll -> poll | 9 cycles (constant) |
| first word - first poll | 8187 cycles (word scheduled 8180 after the write) |
| per-word polls between data reads | `2,2,1` repeating (period 3) |
| total ROMCNT polls per transfer | **1121** (909 + 212) |
| data words | 128, then bit31 clears on the read that empties the FIFO |

The words are the ROM bytes at the commanded file address: transfer 1 reads
`0x1D126600` and returns `20152253 7A003214 04023052 83A30001 ...`, which is
byte-for-byte the ROM image there (checked against the `.nds`).

## 3. The native model against the same measurements

Native at the Phase-4K divergence transfer (`tools/p4l_drain.py` on the PC
traces, both aligned on the 5th decryptor entry `0x0215B468`):

| quantity | melonDS | native |
|---|---|---|
| data reads | 128 | **128** |
| first-word wait | 909 polls | 910 counted (the DRQ poll included — Phase-4D convention) |
| per-word polls | `2,2,1` repeating | `2,1,2,1,2,2,...` (phase wanders) |
| total polls | 1121 | **1113** (8 fewer) |
| loop-body PCs in the run | 6122 | 6082 |
| copied words | ROM[addr] | **byte-identical** |

The *contents* side is exact: the native guest buffer at `0x0206F120` (where
`0x02016B44` stores each word) is a 512-byte slice that matches the ROM
exactly at `0x1AB800` (`p4l_words.py`: 512/512 bytes equal).  The *cadence*
side carries the Phase-4D residual (per-instruction pipeline cost): native
charges instruction costs at the top of the dispatch while melonDS charges
them when the instruction completes, so the word-arrival phase relative to the
polls drifts until one poll sees bit23 clear where melonDS sees it set.

That is exactly the stop point Phase-4K named: 86,059 instructions after the
5th tap both traces are identical, and then

```
mel 0x02016B3C   (bit23 set -> data read)
nat 0x02016B4C   (bit23 clear -> beq taken)
```

**This is a one-poll slip, not lost data.**  The loop re-synchronises three
iterations later (both at `0x02016B4C`), native reads all 128 words in the
same order, and the run continues.  The drain loop is therefore not a barrier
by itself; the barrier was elsewhere.

## 4. The real wall: `0x0215C034` is not in the compiled image

Both traces were re-compared with the anchor at the 5th and 10th decryptor
entry (`p4l_cmp.py`): from the 10th tap, **17,183 instructions are identical
and then the native trace ends** — at `0x0215C034`, `halt=dispatch to
untranslated pc`.  melonDS executes that address (measured: it is the
`ldmfd sp!,{pc}` tail of the arena routine at `0x0215C014`, reached at
13.2M ARM9 instructions, right after the ten decrypt passes).

Root cause: the generated image carries exactly the recovery roots; a PC that
only the *later* trace reaches has no case, and `nnk_smc_try()` cannot help
because it refuses to re-decode a PC with no generation-0 word
(`smc_img_word`).  So the dispatch default halted on perfectly valid code.

Fix (Phase-4L): `nnk_smc_exec_any()` in `tools/nnk_smc.inc` decodes the LIVE
word of an unrooted PC (ARM first, Thumb fallback, the same decoder the
generation tracker uses) and executes it in place; `tools/split_dispatch.py`
calls it in the dispatch default before the honest halt.  A word that decodes
as neither mode still halts with the unchanged message, so a real
data-as-code jump is not papered over.

## 5. Measured result

Same command as Phase-4K (stored guest image, live gx bridge), 20M-step
budget:

```sh
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless NNK_STEP_LIMIT=20000000 \
NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

| | Phase-4K | Phase-4L |
|---|---|---|
| steps | 13,676,011 | **20,000,001** |
| halt | untranslated `0x0215C034` | **step budget exhausted** (no halt) |
| distinct PCs | 5,217 | 5,332 |
| gx frames presented | 30 (all black) | **41** |
| first non-black frame | never | **frame 32** |

Frames (headless SDL, `NNK_GX_SHOT`, PPM per 4th frame,
`p4l_ppmstat.py`): frames 0-31 are pure black `(0,0,0)`; frames 32, 36, 40 are
pure white `(255,255,255)` on both screens.  The display therefore gets
powered by the guest during this window — but a uniform white screen is the
"LCD on, nothing composed yet" state, not a picture: DISPCNT-driven content
has not been rendered.  Honest reading: display init is *entered*, the first
picture is not there yet.

## 6. Script hygiene (Phase-4L)

* `tools/verify_phase2a.sh` and `tools/verify_phase1a.sh` no longer `cd` to
  `/home/hermes/NiNoKuni`; they `cd "$(dirname "$0")/.."`, so a worktree run
  tests the worktree's code.  `verify_phase2a.sh` also fails early with a
  clear message when `./work` is missing instead of silently using another
  checkout's state.
* `tools/build_split.sh` re-execs itself under bash when started by a POSIX
  shell (`sh tools/build_split.sh` used to die on `set -o pipefail`), and the
  header now documents the two supported invocations.

## 7. Verification status

* `tools/verify_phase2a.sh` (run from THIS worktree, `SKIP_MELONDS=1`):
  see §9 of the task report / `p4l_verify.log`.
* Build: `NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c
  SPLIT_DIR=work/split/c tools/build_split.sh out/split/arm9_gx` — 32 TUs,
  `BUILD OK`.
* All acceptance-relevant runs used `work/phase3a/arm9_stored_padded.bin`
  (the stored image) through the `work` symlink to the main checkout.

## 8. Next frontier

1. **The white screen.**  The run reaches LCD power-up and then keeps
   executing (20M budget, `halt_pc=0x020092A0`).  Next: trace what the guest
   does with DISPCNT/DISPCNT2, VRAMCNT and the BG/OAM writes after frame 32 —
   a uniform white frame means the compositor has no armed layers, so the
   first thing to measure is whether the guest's display-register writes are
   reaching the gx_display bridge and whether VRAM bank mapping
   (`0x04000240..0x04000248`, still store-only in the runtime IO model) is
   being applied: a layer whose tile data lives in an unmapped bank renders
   empty.
2. **The poll-cadence residual** (1121 vs 1113 polls per transfer).  Same
   class as Phase-4D's pre-loop offset: it needs melonDS's per-instruction
   pipeline accounting, not another fitted constant.  It does not change the
   data words and does not block progress.

## 9. Files

* `tools/nnk_smc.inc` — `nnk_smc_exec_any()` (decode-on-demand) + the
  `on_demand` counter in the SMC stats line.
* `tools/split_dispatch.py` — call it in the dispatch default before the halt.
* `tools/verify_phase2a.sh`, `tools/verify_phase1a.sh` — repo-root `cd`
  (Phase-4L hygiene), missing-`work` guard.
* `tools/build_split.sh` — bash re-exec guard + documented invocations.

Guest images, arenas, traces and frames stay under gitignored `work/` and
scratch.  No ROM bytes are committed.
