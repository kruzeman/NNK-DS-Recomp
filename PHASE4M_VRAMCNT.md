# Phase-4M — VRAMCNT bank mapping + the MMIO readback class

Follows Phase-4L (merged `1320077`).  Scope: model `VRAMCNT_A..I`
(`0x04000240..0x04000248`) per melonDS, wire the mapped banks into the live
`gx_display` present path, and get the first real picture — or an honest
stop-report naming the exact first differing word and its class.

## 0. Headline

* **`VRAMCNT_A..I` is now a real register file**: width-correct stores
  (`W8`/`W16`/`W32`), readback at all three widths, and `VRAMSTAT`
  (`0x0400024E`) derived from the ARM7-mapped banks.  The mapping EFFECT was
  already owned by the merged `gx_display` API (`gxd_io_write` forwards every
  store with the guest's width and `gx_display.c` implements melonDS's
  `MapVRAM_A..I` table + flat-view rebuild that the 2D compositor reads).
* **The measured native bank programming is byte-identical to melonDS**: 51
  `VRAMCNT` stores, same addresses, same widths, same order, and the same final
  map — `A=82 B=83 C=8B D=84 E=81 F=83 G=8B H=03 I=81` (melonDS
  `p4l_mel_io.txt`, ts 19.909M..20.865M).  **No bank-mapping bug remains.**
* **Two real barriers fell out of the measurement**, both in the same class:
  a display register the guest READS through a read-modify-write sequence had
  no readback.
  1. **`POWCNT1` (`0x04000304`)** — the guest's display-init at
     `0x02002A3C..0x02002A64` reads it back; with no readback it read 0 and the
     final store was **`0x0001` where melonDS writes `0x820F`**, so the screen
     and both 2D engines stayed OFF.  Result: every frame uniform **black**.
  2. **The engine B 2D block (`0x04001000..0x0400106F`)** — no readback at all
     (those registers live only in `gx_display`, which is write-only from the
     runtime's side).  The RMW at `0x02041710` wrote
     **`DISPCNT_B = 0x00200010`** where melonDS writes `0x80211013`, leaving
     engine B in DS display mode 0 ("display off" = white).
* **After both fixes the native register state matches melonDS exactly**:
  `POWCNT1 = 0x020F`, `DISPCNT_A = 0x0021111B`, `DISPCNT_B = 0x80211013`,
  final `VRAMCNT` map as above.  Frames move black → **white** (display
  powered, engines enabled, backdrop only — see §5).
* **No picture yet, and the remaining barrier is not graphics**: the guest
  enters the SDK terminal loop at `0x0200712C` (`bl 0x02008d0c`, then
  `bl 0x02009118` / `b 0x02007134` forever) at **step 18,387,059**, before any
  BG layer has content.  melonDS is in the *same* register/IO state at that
  point (`BG0CNT=0x0002`, `BG1CNT=0x0003`: char and screen base 0, i.e. an
  empty layer), so a uniform frame is the honest output of this boot point.

## 1. What the VRAMCNT model does (melonDS `GPU::Write8/16/32` + `MapVRAM_A..I`)

| register | before (image prelude) | now |
|---|---|---|
| `0x04000240..0x04000248` | `io_write8_sw` only: one byte per store; `io_write16_sw`/`io_write32_sw` had **no case** (wide store dropped); `io_read16`/`io_read32` had **no case** (read 0) | width-correct byte file (`vrc_write`), readback (`vrc_read`) at 1/2/4 bytes, `VRAMSTAT` from banks C/D |
| `0x0400024E` `VRAMSTAT` | not modelled | `bit0 = VRAM_C mapped to ARM7`, `bit1 = VRAM_D` (`(cnt & 0x87) == 0x82`) |
| `0x04000304` `POWCNT1` | store-only | readback of the written 16 bits |

The guest really uses the wide forms first (melonDS trace, all `pc`-matched in
native):

```
W32 0x04000240 0x00000000        ; clear A..D in one store   (native step 13,908,194)
W8  0x04000244..246 0x00         ; (native 13,908,195..197)
W16 0x04000248 0x00000000        ; clear H..I                (native 13,908,198)
W8  0x04000240..249 0x80         ; enable all banks -> LCDC  (native 13,908,673..697)
...  (clear, re-enable, then the real map)
W8  0x04000242 0x8B / 0x04000241 0x83 / 0x04000246 0x8B / 0x04000245 0x83
W8  0x04000240 0x82 / 0x04000244 0x81 / 0x04000243 0x84 / 0x04000249 0x83
W8  0x04000248 0x81              ; final map, native step 14,071,986
```

`0x04000249` writes target the byte after bank I; both melonDS and this model
ignore it (it does not exist), which the trace shows (A..I unchanged).

## 2. Where the guest's framebuffers come from (measured)

* `327,680` VRAM stores, **all** through the **LCDC window**
  (`0x06800000..0x0689FFFF`, raw bank layout), steps 13,908,717..14,059,910 —
  i.e. exactly while the guest has every bank temporarily enabled as LCDC
  (`cnt = 0x80`).  `0` stores through the ABG/BBG/AOBJ/BOBJ windows.
* palette `1,792` stores (up to step 14,062,000), OAM `1,792` (up to
  14,061,761).
* The compositor therefore reads the content from the raw banks through the
  final `VRAMCNT` map, which is what §0 says now matches melonDS.

## 3. Implementation

* `nnktool/src/recompile.rs` (authoritative, for the next image generation):
  `vrc_in/vrc_write/vrc_read/vrc_stat` (VRAMCNT + VRAMSTAT), `io2d_sh[2][0x70]`
  with `io2d_write/io2d_read` (2D register-block readback, `DISPSTAT`/`VCOUNT`
  excluded — they stay live), `POWCNT1` readback in `io_read32`, hooks in
  `io_read8/16/32` and `io_write8/16/32`.  The dead `a == 0x04000247`
  `WRAMCNT` branches are gone: on ARM9 that address *is* `VRAMCNT_H`, which is
  what melonDS serves.
* `tools/nnk_vramcnt.inc` (new) — the same model for the **already-generated**
  image (`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`), plus the measurement
  surface: `NNK_VRC_TRACE=1` logs every register write with the resulting
  `A..I`, and one report block is printed before the run summary
  (`vrc: writes=… stores: palette/vram/oam … window split … DISPCNT A/B …
  A=..->ABG[0..3] …`).
* `tools/split_dispatch.py` — splices the fragment and hooks it into the IO
  accessors by exact-once line substitution (a missing anchor aborts the
  split, so the store-only class cannot silently come back): 7 register hooks,
  3 window-store counters, 3 read-path and 3 write-path shadow hooks, and the
  report call.
* `tools/build_split.sh` — `nnk_dma.inc` / `nnk_vramcnt.inc` are now part of
  the object-cache key.  Without this, editing an `.inc` did not invalidate
  `nnk_main.o` and a stale binary kept the old model (hit and fixed here).

## 4. Numbers (all measured, `NNK_STEP_LIMIT=25000000`, stored image)

| metric | before | after |
|---|---|---|
| steps / halt | 25,000,001 / budget | 25,000,001 / budget |
| distinct PCs | 9,029 | 9,019 (same code set; state differs after 13.9M) |
| `POWCNT1` (runtime) | `0x0001` | **`0x020F`** (= melonDS) |
| `DISPCNT_A` | `0x0021111B` (= melonDS) | `0x0021111B` |
| `DISPCNT_B` | `0x00200010` | **`0x80211013`** (= melonDS) |
| final bank map | (see §1) | `82 83 8B 84 81 83 8B 03 81` (= melonDS) |
| frames 0..29 (before the display enable) | black | black |
| frames 30..49 | black | **white** (`255,255,255`, backdrop only) |
| frames presented | 50 | 50 |
| SDK terminal loop entry | step 18,387,067 | step 18,387,059 (+8 from the corrected state) |

Evidence files (scratch):
`p4m_base.*.ppm` (before, all black), `p4m_fix.*.ppm` (POWCNT1 fix only),
`p4m_sh.*.ppm` (both fixes), `p4m_vrc.err` (VRAMCNT write trace),
`p4m_verify.log`.

## 5. Honest stop-report — exact first differing word + class

Given the acceptance's alternative ("honest stop-report naming the exact first
differing word + class"), the chain is fully named:

1. **class: missing MMIO readback (write-only register model)**
   — first differing word: `POWCNT1` (`0x04000304`) at the store from
   `pc=0x02002A64`: native `0x0001`, melonDS `0x820F`.  Fixed.
2. **class: same** — first differing word: `DISPCNT_B` (`0x04001000`) at the
   store from `pc=0x02041738`: native `0x00200010`, melonDS `0x80211013`.
   Fixed.
3. **remaining barrier: guest control flow, not graphics.**  At
   `step=18,387,059` the guest calls `0x0200712C` (`lr=0x02006AF0`, all
   argument registers 0) which is the SDK terminal loop
   (`bl 0x02008d0c` then `bl 0x02009118` / `b 0x02007134`, forever).  At that
   moment the native register state equals melonDS's documented values and no
   BG layer has content yet (`BG0CNT=0x0002`, `BG1CNT=0x0003` in the melonDS
   IO trace, both bases 0), so the composite is the backdrop — uniform, and
   correctly so.  No non-uniform frame exists at this boot point in either
   implementation; the next picture is behind the abort.

Next (bounded): identify why `0x0200712C` is reached — melonDS never executes
it in its 1.2M-line IO capture (the tail is still card `0xB7` reads at
ts 23.1M).  The two candidates are an ARM7/IPC service result the runtime
HLEs and a card/overlay step the runtime models approximately; a register
probe at the caller (`0x02006AE8..0x02006AF0`) plus the melonDS state at the
equivalent ts decides it.

## 6. Verification

* `SKIP_MELONDS=1 bash tools/verify_phase2a.sh` run **from this worktree**
  (`p4m_verify.log`): `RESULT: PASS`, `rc=0`.  Steps A/A2/B/C1/C2 pass, step D
  builds and runs the mono image from the branch code (`recompile-all: 1943
  seeds -> 2020 functions, 72959 cases, 25632712 bytes C`), and step F reports

  ```
  1) strict common prefix        : 1000 / 1000 melonDS PCs (100.0%)
  2) window-filtered prefix      : 1000 / 1000 (window 0x02000000..0x02400000)
  3) in-order matched after resync: 1000 / 1000 melonDS PCs (100.0%)
  ```

* The recompiled-image acceptance runs use `work/phase3a/arm9_stored_padded.bin`
  (the stored image) through the `work` symlink to the main checkout; the
  `recompile.rs` model is authoritative but not yet in that pre-generated
  image, so `tools/nnk_vramcnt.inc` carries it for this branch's runs.

## 7. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# build (the fragment is spliced by split_dispatch.py)
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  SPLIT_DIR=work/split/c tools/build_split.sh out/split/arm9_gx

# run with the VRAMCNT trace and per-frame PPMs
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless NNK_VRC_TRACE=1 \
  NNK_STEP_LIMIT=25000000 NNK_GX_SHOT=$S/p4m_sh NNK_GX_SHOT_EVERY=6 \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin

python3 $S/p4m_ppm.py $S/p4m_sh.*.ppm          # per-frame stats
grep -E "^vrc: (writes|store|VRAM window|runtime|DISPCNT|A=)" $S/p4m_vrc.err
```

Ground truth: `/home/hermes/.hermes/profiles/coder/cache/scratch/p4l_mel_io.txt`
(1.2M-line melonDS IO capture, `grep 0400024` for the bank writes).
