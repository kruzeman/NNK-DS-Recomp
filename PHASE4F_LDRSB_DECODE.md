# Phase-4F — the 0x0210E388 abort is gone: LDRSB was decoded as LDRD

Follows Phase-4E (merged `0890875`).  Scope: capture melonDS state at the
`0x0200D4D0` call, diff it against the native run, fix the gap, and re-run for
the first real picture.

## 0. Headline

* **Root cause found and fixed: it was an ARM *decoder* bug, not an IO/data
  model gap.**  `E19A80D7 ldrsb r8,[sl,r7]` at `0x0200C7C8` was decoded as
  `ldrd r8,[sl,r7]` (`nnktool/src/decode.rs`, extra-load/store table).  The
  emitted LDRD loads **eight** bytes, so the pair's second register (`r9` =
  the SDK path-parse output-buffer pointer `0x027E3968`) was overwritten with
  the four path-string bytes at `0x0213DD0D` ("tem/") -> `0x2F6D6574`.  The
  SDK path parser then copies into garbage, trips its own "buffer too small"
  flag, returns 0 -> `0x0200D0E8` returns 0 -> the guest takes the abort arm
  at `0x0210E388`.
* **The abort is gone.**  With the fix the guest executes
  `0x0210E380 cmp r0,#0` -> `0x0210E384 popne` (melonDS's non-abort branch)
  and runs **8,000 steps further** (13,134,180 -> 13,142,564) before a *new,
  different* barrier: overlay-slot snapshot content (see §5).
* Strict prefix vs the 30 M-PC melonDS capture: **3,634,037 strict /
  3,643,409 window-filtered** — unchanged from Phase-4E (first divergence
  still the known card-poll timing offset `0x02016B4C` vs `0x02016B3C`).
* `tools/verify_phase2a.sh`: **green** (identical output to the Phase-4E run).
* No picture yet: the guest still never enables the display
  (`DISPCNT` stays 0 — 29 frames presented, all black).  Honest stop-report
  in §5 names the exact next barrier.

## 1. The captures (melonDS probes extended in `tools/melonds-p4f-probe.patch`)

`RegProbe` (ARM.cpp) gained:

| env | meaning |
|---|---|
| `MELONDS_REGPROBE_MEM="a:len[,a:len]"` | up to 4 ranges hexdumped on every hit, read through `ARMv5::DataRead8` so **DTCM hits read the guest's view** |
| `MELONDS_REGPROBE_MEM_PATH` | output file (line-buffered, `hit <n>` + `range` headers) |
| `MELONDS_REGPROBE_EXIT_PC` + `MELONDS_REGPROBE_EXIT_HIT` | `exit(0)` right after logging the k-th hit of that PC (pins a capture to one call, not the first) |

`MemWatch` (NDS.cpp) gained `MELONDS_MEMWATCH_LO/HI` so a write watch can
target DTCM (`0x027Exxxx`) instead of the boot's hard-coded offsets.

The native runtime (`nnktool/src/recompile.rs`) gained the matching side:

| env | meaning |
|---|---|
| `NNK_DEBUG_MEM="a:len[,a:len]"` + `NNK_DEBUG_MEM_PATH` | hexdump of each range on every `NNK_DEBUG_AT_PC` hit, read through the guest data path (DTCM ranges work) |
| `NNK_HALT_AT_PC=<hex>` | honest `halt-at-pc` stop, so a probe run ends at the interesting instruction instead of the 60 M budget |
| debug line now carries `step=` | lets a capture be pinned to a step index |

## 2. The measurements that killed the "data model" hypothesis

At the failing call the two emulators agree on *everything* except the one
corrupted register:

| quantity | melonDS | native |
|---|---|---|
| entry regs at `0x0200C638` | `r0=0213DD04 r1=027E3958 r2=027E3968 r3=200 r4=027E3968 r13=027E3958 r14=0200D114` | **identical** |
| path string @`0x0213DD04` (0x120 bytes) | `/data/System/ItemParam/ItemParam.dat` | **byte-identical** |
| DTCM arg block `0x027E3A00..0x027E3B00` at the call | — | surrounding frame **identical** (`0x027E3AA0` = `{r3,r4,r5,lr}` match word for word) |
| SDK state struct `0x0206E608` (`+0`=0x0206E8A0, `+4`=0x0206E8A0, `+8/+0xA/+0xC`=0) | same | same |
| return of `0x0200C638` | `1` (parse ok, 85 body PCs on the first call / 1339 on this one) | **`0`** (231 PCs) |

So the arg block was *not* the first differing word — the divergence is inside
the static-image SDK helper, at PC #177 of `0x0200C638`'s body, inside
`0x0200C5D8` (a bounded string copy):

```
melonDS: ... 0x0200c604 0x0200c608 0x0200c5f0 0x0200c5f4 ...   (loop again)
native : ... 0x0200c604 0x0200c608 0x0200c60c ...              (loop exits)
```

The native debug at `0x0200C5D8` (`r14` = the call site) shows the smoking
gun:

```
dbg1 step=12233535 pc=0200C5D8 r0=2F6D6574 r1=00000104 r2=0213DD05 r3=00000004 ...
     r8=7379532F ...  r14=0200C8A0
```

* `r8 = 0x7379532F` is the **32-bit little-endian word** at `0x0213DD09`
  (`'/' 'S' 'y' 's'`) where `0x0200C7C8 ldrsb` must have produced
  `0x0000002F`.
* `r9` (used as the destination base at `0x0200C89C`) is
  `0x2F6D6574` = the word at `0x0213DD0D` (`'t' 'e' 'm' '/'`), while it must
  be `0x027E3968`.  `r9` is exactly the register an `ldrd r8,[sl,r7]` writes
  as its second half.

`case 0x0200C7C8u:` in the old generated C (`arm9_thumb_ovl_itcm_p4f2.c`)
confirmed it: `/* 0x0200C7C8: ldrd r8, [sl, r7] */`.

## 3. The bug and the fix (`nnktool/src/decode.rs`)

ARM extra load/store, bits 7-4 = `1SH1`, S/H = bits 6-5, L = bit 20:

| S/H | L | instruction |
|---|---|---|
| 01 | 0/1 | STRH / LDRH |
| 10 | 0 | **LDRD** |
| 10 | 1 | **LDRSB** |
| 11 | 0 | **STRD** |
| 11 | 1 | **LDRSH** |

The table matched `(h=2, L=1)` to LDRD, `(h=2, L=0)` to STRD and the `_` arm
to LDRSB — i.e. LDRSB/LDRD/STRD/STRD were all mis-assigned.  Effect on the
current image: **271** instructions decoded as `ldrd` are really `ldrsb`, and
**29** decoded as `ldrsb` are really `strd` (single-byte load instead of an
8-byte store).  The emitter already had correct `Ldrsb`/`Ldrsh`/`Ldrd`/`Strd`
bodies, so only the decode table changed.

## 4. Verification after the fix

```
old: halt_pc=0x0210E388 // bl 0x020090D8 (SDK "unrecoverable, stop")
new: 0x0210E380 -> 0x0210E384 (popne, i.e. melonDS's branch) -> ... -> 13,142,564 steps
```

* `0x0200C7C8` now emits `r[8] = (uint32_t)(int32_t)(int8_t)ld8(ea)`.
* melonDS never executes `0x0210E388` in 30 M PCs; after the fix the native
  never does either, and it reaches the *same* call state melonDS has
  (`r1=0x0215C1C0 r2=0x02159FE0 r5=0x13 r6=0x14 r14=0x0205D3F8` — identical in
  both).
* `nnktool` regression suites (`verify_phase2a.sh` sections A/A2/B/C/D) pass.

## 5. Honest stop-report — the next barrier

After the fix the guest runs to step **13,142,564** and halts at
`0x0215B350` with `halt=SWI not implemented` (imm24=0x5E6DB2, i.e. the
snapshot bytes there are **not** code):
`b2 6d 5e 7f f2 ce de 43 ...` — 0x0215B34C..0x0215B3FF is the loader's
staging/garbage in the recompiled snapshot.

Ground truth: melonDS calls `bl 0x0215B34C` from `0x0205D458` and executes
`0x0215B34C -> 0x0215B350 -> 0x0215B354 ... 0x0215B36C -> 0x0215B6E0` as
**ordinary instructions**; i.e. in melonDS that page holds real code.

What the sweep found along the way (all measured this phase):

* the previous halt (`0x0216F98C`, dispatch-to-untranslated) was the *same*
  class: a module at `0x0215B400` that the p4e snapshot still had as zeros
  (melonDS executes `0x0215B400` twice; the native executed the zero-filled
  cases as `andeq` and slid through `0x0215Exxx..0x0216F98C`).
* Refreshing the snapshot from the runtime's own 24 MiB main-RAM dump
  (`tools/p4f_extract_ram.py`, `tools/p4f_merge_ovl.py`, merged with the p4e
  snapshot so early modules keep their bytes) **does** move the barrier: the
  module at `0x0215B400` now executes real code (native state at the call
  matches melonDS register-for-register) and the run gains 8,000 steps.
* What it does not fix: an overlay **slot** holds different modules at
  different times, and the dispatch image has exactly one case per PC.  The
  dump point also matters — the region at `0x0215B34C` is *overwritten later
  in the run*, so a dump taken at the halt is not the content the earlier call
  saw.

**Exact next barrier:** per-slot, per-load overlay snapshots.  Concretely:
dump main RAM at the moment of the call (`NNK_DUMP_AT_PC=0x0215B34C` +
`NNK_CHECKPOINT_MEM`), extract `0x020C0000..0x02180000` from *that* dump,
merge, recompile the overlay image, splice, rebuild — and tag each overlapping
slot with the load generation so two modules at the same addresses do not
share one case.  Display init is behind this: `DISPCNT` is still 0, so the 29
frames presented are legitimately black (no picture yet).

## 6. Numbers (all measured)

| metric | value |
|---|---|
| abort `0x0210E388` | **gone** (melonDS's `0x0210E384` branch taken) |
| steps reached | 13,142,564 (was 12,233,662 at the abort) |
| mis-decoded instructions fixed in this image | 271 LDRSB (were LDRD) + 29 STRD (were LDRSB) |
| strict prefix vs 30 M melonDS capture | 3,634,037 (unchanged) |
| window-filtered prefix | 3,643,409, first divergence `0x02016B4C` vs `0x02016B3C` (known card-timing offset) |
| frames presented | 29, all black (`DISPCNT` = 0) |
| `verify_phase2a.sh` | PASS |

## 7. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# melonDS state at the call (probe patch: tools/melonds-p4f-probe.patch)
(cd /home/hermes/tools/melonDS && git apply /home/hermes/NiNoKuni/tools/melonds-p4f-probe.patch 2>/dev/null || true)
MELONDS_REGPROBE_PATH=$S/reg.txt MELONDS_REGPROBE_PCS=0x0210E380 \
MELONDS_REGPROBE_MEM="0x027E3A00:0x100,0x0213DCE0:0x120" \
MELONDS_REGPROBE_MEM_PATH=$S/mem.txt \
MELONDS_REGPROBE_EXIT_PC=0x0215B400 MELONDS_REGPROBE_EXIT_HIT=1 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "<ROM>"

# native side (build: S=... sh tools/p4e_sweep.sh p4f2)
NNK_ROM_PATH="<ROM>" NNK_HALT_AT_PC=0x0210E388 NNK_DEBUG_AT_PC=0x0200C5D8 \
NNK_DEBUG_MEM="0x0206E600:0x40" NNK_DEBUG_MEM_PATH=$S/nat_mem.txt \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin

# the call-body diff (the tool that found the divergence)
python3 tools/p4f_cmp_call.py $S/melonds_p4e_pc.txt $S/native_tail.txt
```
