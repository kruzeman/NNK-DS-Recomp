# Phase-4I — generation-aware overlay dispatch: the union of both images, through the integrity check

Follows Phase-4H (merged `96aaa5a`).  Scope: extend the runtime overlay-slot
registry so the dispatch serves the **union** of the overlay generations, prove
that (1) the guest's stub-page decryptor at `0x0215B468` executes in native with
its plaintext stores landing in RAM and (2) the integrity check at
`0x0205D448` matches `0x2FBB82E1` with the tamper path not taken, then push
toward display init.

**Outcome: both success markers are met and measured; the picture is still not
there — honest stop-report at the next unpatched decrypt generation
(`0x0215B834`).**  The class is no longer "two generations" but **N
generations**: melonDS calls the arena's own decryptor **10 times** during the
boot, each pass rewriting a different range of the resident module, so a
compile-time union of two images is always one pass behind.  That is the bounded
hand-off in §6.

## 1. The measured generation structure of arena `0x02159FE0`

From the melonDS overlay PC trace (`p4h_mel_ovl.txt`, 288,710 overlay PCs) the
decryptor `0x0215B468` is entered at overlay-PC indices

```
17812 19199 20967 22086 25005 | 104010 105397 107165 108284 111203
```

— five passes early in the boot, five later.  Each pass runs the loop
`0x0215B4B0..0x0215B4C8` and stores plaintext over the resident module.  The
caller chain is identical every time:

```
0x0215B51C  (descriptor-loop tail)
  -> 0x0215B408   bl 0x0215B750      (loader helper, never decrypted)
     -> 0x0215B750 0x0215B754 0x0215B758  (thunk into the decryptor)
        -> 0x0215B468  (decryptor)
```

So the arena is a **code area whose generations are produced at run time, in
several passes**, and three kinds of addresses live in it simultaneously:

| kind | example | generation that executes it |
|---|---|---|
| loader / decryptor machinery | `0x0215B400` (`bx lr` entry), `0x0215B408` (`bl 0x0215B750`), `0x0215B4EC..0x0215B51C` (descriptor loop), `0x0215C100..0x0215C140` | **ROM** (never decrypted) |
| decrypt-target code | `0x0215B34C..0x0215B3E8` (three 9-word thunks), `0x0215B6E0..0x0215B748`, `0x0215A7C0..0x0215B9B0` | **post-decrypt** (pass-dependent) |
| runtime data / zero-fill | `0x0215B410` = ids `19`/`20`, descriptors, BSS | neither (data; reads come from RAM, not from the image) |

Key architectural point (new, and what makes the rest tractable): **the
dispatch image only decides which case runs at a PC — guest data reads always
come from RAM.**  Overwriting a *data* address in the image is harmless unless
some PC executes it; overwriting a *machinery* address is fatal.

## 2. What was built

`tools/ovl_union_gen.py` (new): builds the union slot image from

* a base image (the ROM generation, `work/phase3a/ovl_p4f4.bin`),
* the melonDS overlay PC trace (which arena words actually execute),
* a native RAM dump taken after the guest's decryptor ran (post-decrypt bytes),

with the measured loader/machinery windows excluded and zero-fill runs left on
the base generation.  Re-running it with a later RAM dump folds in one more
decrypt pass ("loop method": every native halt yields the next dump).

Chain per iteration (all reproducible, no ROM bytes committed):

```sh
python3 tools/ovl_union_gen.py <prev.bin> <ram_dump.bin> <mel_ovl_pcs.txt> work/phase3a/ovl_p4iN.bin
nnktool/target/release/nnktool recompile-all work/phase3a/ovl_p4iN.bin work/phase3a/ovl_p4iN.c \
    --base 0x020C0000 --entry 0x02139E48 --seeds work/phase3a/seeds_p4g_ovl.tsv
python3 tools/merge_itcm_cases.py work/phase3a/arm9_thumb_p4g.c work/phase3a/ovl_p4iN.c \
    work/phase3a/arm9_thumb_ovl_p4iN.c - 0x020C0000 0x02180000
python3 tools/merge_itcm_cases.py work/phase3a/arm9_thumb_ovl_p4iN.c $S/itcm_image_p4d.c \
    work/phase3a/arm9_thumb_ovl_itcm_p4iN.c $S/melonds_arm9_trace_itcm.txt
SPLIT_DIR=out/split/c NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4iN.c NNK_GX=1 \
    BUILD_JOBS=16 tools/build_split.sh out/split/arm9_gx
```

Native run (the **stored** image, Phase-4H §0 trap):

```sh
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless NNK_OVL_TRACE=o.txt \
  NNK_DEBUG_AT_PC=0x0205D448 NNK_DEBUG_N=1 \
  NNK_DEBUG_MEM=0x0215B340:0x120 NNK_DEBUG_MEM_PATH=m.txt \
  NNK_DUMP_MEM=mem.bin out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
```

## 3. Success marker 1 — the decryptor executes natively

`NNK_DEBUG_AT_PC=0x0215B4BC` (the plaintext store inside the loop), first three
and last three of the captured hits (30 hits captured, `NNK_DEBUG_N=30`):

```
dbg1  step=13134205 pc=0215B4BC r0=0215C110 r3=E18FC00F r4=0215C014 r5=0215C038 r6=7FEC9DF1
dbg2  step=13134212 pc=0215B4BC r0=0215C110 r3=E01CC00C r4=0215C018 r5=0215C038 r6=9F42ADBE
dbg3  step=13134219 pc=0215B4BC r0=0215C110 r3=03A0C000 r4=0215C01C r5=0215C038 r6=407E0EF2
...
dbg30 step=13134456 pc=0215B4BC r0=0215C128 r3=03A0C000 r4=0215C0D0 r5=0215C0EC r6=407E0EF2
```

`r3` is the plaintext word about to be stored — exactly the melonDS decrypted
stub words (`E18FC00F E01CC00C 03A0C000 …`, cf. `PHASE4G` §2).  `r0` walks the
descriptor table `0x0215C110 -> 0x0215C128` (the two descriptors whose targets
the run reaches), `r13=027E3B4C` is the guest stack.  Phase-4H's "0 dispatches
at `0x0215B468`/`0x0215B4BC`" is now **30+ stores**, i.e. the 27 plaintext
stores of the first descriptor plus the following block.

## 4. Success marker 2 — the integrity hash matches, tamper path not taken

`NNK_DEBUG_AT_PC=0x0205D448` (the compare), step 13,142,558:

```
r0=2FBB82E1  r1=00000001  r2=E8BD8000  r3=00000000  r4=2FBB82E1
r5=00000013  r6=00000014  r14=0215B370  nzcv=0110
```

* `r4 = 0x2FBB82E1` = `r0` (expected) — **the hash matches**;
* `r2` is *not* `0x0215A084`, and `r14 = 0x0215B370` — the guest went to the
  normal path (`bl 0x0215B34C`), **the tamper path `blx 0x0215A084` is not
  taken** (Phase-4G's `r4=F220F0F6`, `r14=0205D471`).

Native RAM at the check is byte-identical to the melonDS ground truth
(`p4h_mem2.txt`), including the three decrypted 36-byte thunks
(`0F C0 8F E1 0C C0 1C E0 00 C0 A0 03 …`) and the descriptor ids `13 14` at
`0x0215B410`.

## 5. Iteration ladder (all numbers measured, same stored image)

| iteration | union content | steps | halt | note |
|---|---|---|---|---|
| it1 | ROM gen + the 3 melonDS-dumped stub blocks (27 words) | 13,142,581 | `0x0215B704` `svc 0x2AE279` | markers 1+2 hold; next block `0x0215B6E0` still ciphertext |
| it2 | RAM-wholesale for every executed word (no windows) | 13,135,566 | pc=`0x00000002` | **regression**: `0x0215B408` overwritten `bl 0x0215B750` -> `pop {r4,pc}`, i.e. the loader machinery must keep the ROM generation (this measurement is what defines the exclusion windows) |
| it3 | windows excluded, it1 RAM dump | 13,158,489 | pc=`0x7E15B05C` at `0x0215B054` | 464 words patched |
| it4 | + it3 RAM dump | 13,168,769 | pc=`0x34E12FFE` at `0x0215B77C` | 65 more words |
| it5 | + it4 RAM dump | 13,169,110 | pc=`0x00000000` at `0x0215B834` | 39 more words |

Frames: 29 presented, all black, `DISPCNT` still 0 (Phase-4E limit, unchanged).

## 6. Honest stop point and the exact next step

**First differing word: `0x0215B834`.**  Native executes the case compiled from
that address's *current* image generation, computes a garbage branch and lands
on `pc = 0x00000000` (`untranslated pc=0x00000000`, `halt_pc=0x0215B834`,
`r14=0x02008F38`, step 13,169,110).  Class: **generation-aware dispatch, still
one decrypt pass behind** — not a decoder bug (every marker instruction on the
path executes and the hash/check dataflow is exact).

Why the loop stalls: the post-decrypt bytes are taken from the RAM dump of the
*previous* halt, and the guest decrypts further ranges in *later* passes (the
five passes at mel indices 104010..111203).  Downstream of the current halt
point the dump is therefore still ciphertext, so each iteration only advances to
the next not-yet-decrypted block (`0x0215B704 -> 0x0215B77C -> 0x0215B834`).

Bounded next steps, in order:

1. **One melonDS capture of the *fully* decrypted arena**: `MELONDS_REGPROBE_PCS=0x0215B468`
   with `MELONDS_REGPROBE_EXIT_HIT=10` (the last pass) and
   `MELONDS_REGPROBE_MEM="0x02159FE0:0x21E0"`.  Feed it to
   `tools/ovl_union_gen.py` as the RAM dump (a melonDS-derived dump also removes
   the one-pass-behind problem entirely).  Expected: the whole decrypt-target
   set of the resident module comes from the decrypted generation in one shot.
2. **Runtime selection for the residual conflicts**: for the words that both a
   pre-decrypt path and a post-decrypt path execute (`0x0215B400`, `0x0215B404`,
   `0x0215C104..0x0215C10C` — the 5 measured conflicts), emit both bodies and
   select per current memory state (a per-arena "generation" flag flipped by the
   guest's own stores through the emulated memory path), i.e. the registry
   *is* consulted at dispatch time instead of being frozen at compile time.
3. Then re-run: the run should pass the integrity check (already green) and
   continue into the overlay's real entry (`0x0215B528...`), which is where
   display init can begin to be exercised.

## 7. Verification

* `tools/verify_phase2a.sh` (`SKIP_MELONDS=1`): PASS — sections A/A2/B/C/D/F,
  same output as Phase-4F/4G/4H (`p4i_verify.log`).
* No ROM bytes committed: ROM, extracted overlays, slot images, RAM dumps and
  the melonDS captures live under gitignored `work/` and the scratch dir.
  Committed: `tools/ovl_union_gen.py` + this document.
