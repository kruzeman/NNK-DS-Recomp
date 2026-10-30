# Phase-4G — per-load overlay slot dumps + runtime slot registry

Follows Phase-4F (merged `65549fd`).  Scope: replace the one-shot overlay-slot
snapshot with **per-load** slot images plus a **runtime slot registry**, push
past the `0x0215B34C` barrier, and report the next exact barrier if the picture
is still not there.

## 0. Headline

* **The `0x0215B34C` barrier is gone as a dispatch barrier**: the case now
  exists and the guest executes it (no more `svc 0x5E6DB2` halt on snapshot
  bytes).  The run reaches **13,134,253** steps and halts at `0x0205D46C`
  (`dispatch to untranslated pc`, `blx 0x0215A084`).
* **The cause of that new halt is measured, not guessed**: `0x0215B34C` is not
  ordinary overlay code, it is a page the guest **decrypts in place at run
  time**, and the static code at `0x0205D420..0x0205D458` is an **integrity
  check over exactly those nine words**:

  | quantity | melonDS (ground truth) | native (this phase) |
  |---|---|---|
  | expected hash (`r0`, from `0x0205D4B0`) | `0x2FBB82E1` | `0x2FBB82E1` |
  | computed hash (`r4`) | `0x2FBB82E1` (matches) | **`0xF220F0F6`** (mismatch) |
  | branch taken | `bl 0x0215B34C` (normal path) | `blx table[r1]` = `0x0215A084` |
  | memory at `0x0215B34C` at the check | decrypted stub `E0 D7 15 02 00 00 00 00 E0 D7 15 02 0F C0 8F E1 …` | **still ciphertext** `FE 5D 63 9E B2 6D 5E 7F F2 CE DE 43 …` (= overlay 19 ROM image offset `0x136C`) |

  So the next barrier is the guest's own decryptor not having run over that
  page before the check — see §6.
* **Per-load dumps and the registry work**: the runtime trace shows the real
  load sequence, including the slot reuse the task asked about —
  `17 -> 0x020D0A00`, `20 -> 0x02159FE0` (gen 1), `19 -> 0x02159FE0` (gen 2).
* Still no picture: 29 frames presented, all black, `DISPCNT` stays 0.
* `tools/verify_phase2a.sh`: PASS (see §7).

## 1. What a "load generation" is in this ROM (measured)

The ARM9 overlay table (`ROM_MAP.md` §overlay table) shows four load arenas:

| arena | overlays |
|---|---|
| `0x02079F80` | 0..13 |
| `0x020C18E0` | 14..16 |
| `0x020D0A00` | 17..18 |
| `0x02159FE0` | **19, 20** |

`tools/dump_ovl_targets.py` decompresses every overlay exactly as the guest
loader does (NITRO code-compression) and compares them per arena: **770
address runs (≈ 0.5 MB) where two overlays of one arena place different bytes**
(`work/ovl_targets/conflicts.tsv`).  A dispatch image with one case per PC
cannot represent both; the registry says which one is resident.

The runtime now records the events themselves (`nnktool/src/recompile.rs`,
`NNK_OVL_TRACE=<path>`): a card `0xB7` read whose address is the first byte of
an overlay's FAT range **is** that overlay's load, and the destination is the
OVT `ramAddress`.  From the Phase-4G run:

```
ovl_load seq=1 id=17 rom=0x00145E00 size=0x4BAC8 dest=0x020D0A00 ram_size=0x72700 gen=1
ovl_load seq=2 id=20 rom=0x001ABA00 size=0x162C  dest=0x02159FE0 ram_size=0x1FA0  gen=1
ovl_load seq=3 id=19 rom=0x001A9800 size=0x20E4  dest=0x02159FE0 ram_size=0x21E0  gen=2
```

Generation 2 of arena `0x02159FE0` is overlay 19 — i.e. the second module over
the same addresses, exactly the case the Phase-4F snapshot could not express.

## 2. Ground truth for the slot image (melonDS probe)

RegProbe capture of the whole arena at the moment melonDS executes
`0x0215B34C` (`MELONDS_REGPROBE_PCS=0x0215B34C`,
`MELONDS_REGPROBE_MEM="0x02159FE0:0x2200"`,
`MELONDS_REGPROBE_EXIT_PC/EXIT_HIT` to pin hit 1).  Matching that dump against
the per-overlay ROM images (`tools/ovl_registry.py match`):

```
arena 0x02159FE0 candidates 19,20
  ovl 19: 74.7822% of 7804 non-zero bytes match
  ovl 20:  0.3117% of 7378 non-zero bytes match
  -> resident: overlay 19
```

74.78 % — not 100 % — is the interesting number: **1965 of the arena's bytes
differ from the ROM image because the guest wrote them at run time**.  A MemWatch
over `[0x0215B340,0x0215B400)` shows the writer: `pc9=0x0215B4BC`, i.e. code
inside the resident overlay, running a rolling-key loop

```
0x0215B4B0  ldr  r1,[r4]
0x0215B4B4  eor  r3,r1,r6
0x0215B4B8  sub  r1,r3,r3,lsr #8
0x0215B4BC  str  r3,[r4],#4
0x0215B4C0  eor  r6,r6,r1
```

seeded from `[0x0215B520]`/`[0x0215B524]` and bounded by the descriptor word
pair `[r0]`/`[r0+4]` — an **in-place decryptor over the resident module's own
pages**.  So the arena is not "the ROM image"; it is "the ROM image after the
guest's own decrypt + init passes", and the per-load dump has to capture it at
the right moment.

## 3. The tooling (new in this phase)

| tool | purpose |
|---|---|
| `tools/dump_ovl_targets.py` | per-overlay slot images straight from the ROM (`ovl_<id>_<ram>.bin`), the arena map, `registry.tsv`, and `conflicts.tsv` (the address runs a one-case-per-PC image cannot represent) |
| `tools/ovl_registry.py` | the one registry of overlay-to-slot state: `build` (generations from an `NNK_OVL_TRACE` log), `match` (identify the resident overlay of a captured slot dump), `slots` (write a captured arena window as a slot image), `conflicts`, `resident` |
| runtime registry | `nnktool/src/recompile.rs`: ROM overlay table + FAT parsed from `NNK_ROM_PATH`, load events logged to `NNK_OVL_TRACE`, per-arena resident id + generation kept in `ovl_arena[]` |
| `tools/p4g_sweep.sh` | one-pass sweep: static-image seeds + recompile, per-load slot images, slot capture refresh, overlay recompile, splice, split build with the live `gx_display` bridge |

Capture/compile chain for the run below:

```
tools/dump_ovl_targets.py "$ROM" work/ovl_targets
tools/ovl_registry.py match  --dump <probe dump> --images work/ovl_targets
tools/ovl_registry.py slots  --dump <probe dump> --arena 0x02159FE0 --size 0x21E0 --out slot_p4g.bin
# base snapshot work/phase3a/ovl_p4f4.bin, refreshed with the 1965 captured bytes
SKIP_SEEDS=1 OVL_SLOT_DUMP=<probe dump> sh tools/p4g_sweep.sh p4g
```

Note on scope item 3 ("extend the melonDS probe patch with overlay load
events"): no new melonDS *code* was needed — the Phase-4F probe patch already
exposes exactly the call-site `r0`/`r1` capture that the reference lesson asks
for (`MELONDS_REGPROBE_PCS` + `MELONDS_REGPROBE_MEM` + `EXIT_PC/EXIT_HIT`), and
that is what produced the slot dump above.  The *load-event* side (id, ROM
range, destination, generation) is now measured on the guest side by the
runtime registry (`NNK_OVL_TRACE`), which is the authoritative load sequence.

## 4. Run result

```
done steps=13134253 traced=13134252 distinct_pcs=5130 hle_hits=7251
     halt_pc=0x0205D46C halt=dispatch to untranslated pc
halt state: r0=00000000 r1=00000000 r2=0215A084 r3=00000000 r4=F220F0F6 r5=00000013
             r6=00000014 r13=027E3B60 r14=0205D471 nzcv=1010
gx: 29 frames presented, 9769 unhandled io writes
```

* `0x0215B34C` executes (Phase-4F's `svc 0x5E6DB2` halt is gone).
* `r14=0x0205D471` — bit 0 set: the guest `blx`'d into **Thumb** code at
  `0x0215A084` (arena `0x02159FE0` + `0x84`), which has no dispatch case.
* `r4=0xF220F0F6` vs the expected `0x2FBB82E1` is the integrity mismatch of §0;
  the native's memory dump at the check proves it is the stub page that
  differs, not the hash table (`r0` is already the same constant in both).
* Frames: 29 presented, all black (`DISPCNT` = 0), so still no picture.

Step count is lower than Phase-4F's 13,142,564 because the guest now takes the
*other* branch of the same check (tamper path) instead of running 8 k more
steps of the normal path; it is not a coverage regression.

## 5. Known limits, still open (unchanged, documented)

* Guest VRAM reads still return 0 and VBlank/HBlank DMA is not triggered
  (Phase-4E notes); irrelevant until `DISPCNT` is programmed.
* A single dispatch image still has one case per PC.  The registry now *names*
  the conflicting runs (770, `work/ovl_targets/conflicts.tsv`) and the resident
  module per generation, but generation-aware case selection is not implemented;
  the image still compiles one generation per address (currently the captured
  overlay 19 generation for arena `0x02159FE0`).

## 6. Exact next barrier (bounded handoff)

**The stub page at `0x0215B34C` must be decrypted in the native's RAM before
`0x0205D448` compares it.**

Evidence: melonDS's memory at the check holds the decrypted stub; the native's
holds the ROM ciphertext (dump taken at `0x0205D448`, step 13,134,246).  The
decryptor is `0x0215B468` in the resident overlay (rolling-key XOR over the
descriptor bounds in `r0`), and it is *itself inside the region that gets
decrypted*, so the bootstrap order matters.  Bounded next steps:

1. `MELONDS_REGPROBE_PCS=0x0215B468` with `MELONDS_REGPROBE_MEM="0x0215B468:0x100"`
   to capture the decrypt call(s): `r0` = descriptor, `[r0]`/`[r0+4]` = the
   region bounds, and the two seed words at `0x0215B520`/`0x0215B524`.
2. The same PCs in the native run (`NNK_DEBUG_AT_PC`/`NNK_DEBUG_MEM`) to find
   whether the call is reached at all, or whether the descriptor list is
   already empty because of an earlier data gap.
3. Since the transform is a plain rolling-key XOR, it can be applied offline
   (`tools/ovl_registry.py slots` + a decrypt pass) to turn the ROM slot image
   into the post-decrypt slot image **without** a capture, which is what makes
   the dispatch image reproducible from the ROM alone.

## 7. Verification

* `tools/verify_phase2a.sh` (SKIP_MELONDS=1, nnktool rebuilt with this phase's
  changes): sections A/A2/B/C/D/F all PASS — same output as Phase-4F.
* No ROM bytes are committed: the ROM, the extracted overlays, the captures
  and every generated image live under gitignored `work/` / the scratch dir.
  Committed files are tools + the runtime registry code + this document.
