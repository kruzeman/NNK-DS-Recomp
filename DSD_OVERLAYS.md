# Phase-5AF — the dsd extract, the full 21-overlay table, and the runtime slots

Follows Phase-5AE (`370f365`).  The card supplied a full `dsd` extract of the
ROM (`work/dsd_extract/`, AetiasHax/ds-decomp) and asked for three things: a
reconciliation of the static overlay table with what the runtime actually
loads, a recompiler/dispatch table-driven for **all 21** overlays including the
static-constructor (ctor) ranges, and byte cross-checks of the extracted
overlay files against the memory image the runtime produces.

**Headline: the table was never the problem — the shipped runtime had no
overlay knowledge at all.**  `work/phase3a/arm9_thumb_ovl_itcm_p5u.c`, the image
every acceptance run is built from, contains no `io_ovl_init`, no `ovl_tab`, no
load log: it predates Phase-4G's registry, so the "measured loads at
`0x020D0A00` and `0x02159FE0` only" was a property of a *different*, earlier
generated image plus a 13 M-step horizon — not of the ROM.  With the registry
spliced into the shipped image from the canonical source, **all four arenas are
used before step 30,000,001**:

```
ovl: load id=17 rom=0x00145E00 ... dest=0x020D0A00 gen=1     arena C
ovl: load id=20 rom=0x001ABA00 ... dest=0x02159FE0 gen=1     arena D
ovl: load id=19 rom=0x001A9800 ... dest=0x02159FE0 gen=2     arena D (2nd module)
...
ovl: load id=14 rom=0x00133800 ... dest=0x020C18E0 gen=1     arena B
ovl: load id=6  rom=0x000E0C00 ... dest=0x02079F80 gen=1     arena A
```

and the extracted table and the ROM's own table agree **field for field on all
21 overlays (0 mismatches)**, while 20 of the 21 extracted overlay files are
byte-identical to our own decompressor's output.  The 21st (ov19) is not a
disagreement — it is a discovery: `dsd` *decrypts* that overlay during
extraction (Level-5 overlay protection), so `ov019.bin` is the post-decrypt
module the guest computes at run time.  §6 has the numbers.

## 1. The table (ROM is authoritative, dsd confirms it)

`tools/ovl_table.py --rom <rom> --yaml work/dsd_extract/arm9_overlays/overlays.yaml`
reads the table out of the ROM the runtime is given and cross-checks every field
against the extract:

```
# ARM9 overlay table: 21 entries, OVT 0x4D400..0x4D6A0, FAT 0x202A00
# id  base        size    bss     end         ctor_lo     ctor_hi     file  rom_off     stored  flags
  0  0x02079F80  0x26120 0x000A0 0x020A0140  0x0209DBC4  0x0209DBCC     0  0x0004D800  0x175CC  compressed
  1  0x02079F80  0x46D80 0x00000 0x020C0D00  0x020BE75C  0x020BE760     1  0x00064E00  0x2CB98  compressed
  2  0x02079F80  0x2F7A0 0x06D40 0x020B0460  0x020A69F4  0x020A69FC     2  0x00091A00  0x1DA34  compressed
  3  0x02079F80  0x1CD00 0x00080 0x02096D00  0x02093F54  0x02093F5C     3  0x000AF600  0x10378  compressed
  4  0x02079F80  0x21200 0x00020 0x0209B1A0  0x02097768  0x02097774     4  0x000BFA00  0x11E10  compressed
  5  0x02079F80  0x1A100 0x00020 0x020940A0  0x02092CA0  0x02092CA4     5  0x000D1A00  0x0F048  compressed
  6  0x02079F80  0x04CA0 0x00000 0x0207EC20  0x0207E730  0x0207E734     6  0x000E0C00  0x03000  compressed
  7  0x02079F80  0x478C0 0x000A0 0x020C18E0  0x020BDA00  0x020BDA04     7  0x000E3C00  0x2A5D8  compressed
  8  0x02079F80  0x20960 0x019A0 0x0209C280  0x02097D38  0x02097D3C     8  0x0010E200  0x12F10  compressed
  9  0x02079F80  0x074E0 0x00000 0x02081460  0x02081178  0x0208117C     9  0x00121200  0x04368  compressed
 10  0x02079F80  0x02FA0 0x00000 0x0207CF20  0x0207CBC8  0x0207CBCC    10  0x00125600  0x01D74  compressed
 11  0x02079F80  0x027C0 0x00000 0x0207C740  0x0207B9EC  0x0207B9F0    11  0x00127400  0x01910  compressed
 12  0x02079F80  0x04600 0x00000 0x0207E580  0x0207DC74  0x0207DC78    12  0x00128E00  0x02C94  compressed
 13  0x02079F80  0x11000 0x00480 0x0208B400  0x0208AE30  0x0208AE34    13  0x0012BC00  0x07BD4  compressed
 14  0x020C18E0  0x0B5C0 0x030E0 0x020CFF80  0x020CC4DC  0x020CC4F0    14  0x00133800  0x71F4   compressed
 15  0x020C18E0  0x05E80 0x00040 0x020C77A0  0x020C74A4  0x020C74A8    15  0x0013AA00  0x3BD8   compressed
 16  0x020C18E0  0x0BBE0 0x03540 0x020D0A00  0x020CC758  0x020CC75C    16  0x0013E600  0x7760   compressed
 17  0x020D0A00  0x72700 0x16EE0 0x02159FE0  0x0213A26C  0x0213A290    17  0x00145E00  0x4BAC8  compressed
 18  0x020D0A00  0x25E40 0x026C0 0x020F8F00  0x020F52A4  0x020F52A8    18  0x00191A00  0x17DD0  compressed
 19  0x02159FE0  0x021E0 0x00020 0x0215C1E0  0x0215C184  0x0215C188    19  0x001A9800  0x20E4   compressed
 20  0x02159FE0  0x01FA0 0x00000 0x0215BF80  0x0215BD70  0x0215BD74    20  0x001ABA00  0x162C   compressed
# yaml cross-check: 21/21 overlays, 0 field mismatch(es)
```

Entry layout of **this** image (measured, and what the runtime parser now reads;
the classic Nitro layout has no leading id word, so a reader must not assume
it):

| offset | field | notes |
|---|---|---|
| +0x00 | overlay id | matches the extract's `id` for all 21 |
| +0x04 | load base | four arenas (§2) |
| +0x08 | code size | what the loader copies (extract: `code_size`) |
| +0x0C | bss size | what the loader zero-fills (extract: `bss_size`) |
| +0x10 | ctor (sinit) start | extract: `ctor_start` |
| +0x14 | ctor (sinit) end | extract: `ctor_end`, always start+4 or start+8…+0x14 |
| +0x18 | FAT file id | extract: `file_id` |
| +0x1C | bit24 compressed \| stored size | e.g. ov17 `0x0104BAC8` = compressed, 0x4BAC8 stored |

`ROM_MAP.md` §"ARM9 overlay table" already carried exactly these rows, so no
correction was needed there — the missing piece was using them at run time.

## 2. The four arenas, and why they are exactly four

```
# arenas (static table):
#   0x02079F80: ids 0..13   span 0x47960      (A)
#   0x020C18E0: ids 14..16  span 0xF120       (B)
#   0x020D0A00: ids 17,18   span 0x895E0      (C)
#   0x02159FE0: ids 19,20   span 0x2200       (D)
```

The arenas are packed, not arbitrary: the *largest* module's `base + code + bss`
lands exactly on the next arena's base —

* ov07 `0x02079F80 + 0x478C0 + 0xA0` = `0x020C18E0` (A -> B)
* ov16 `0x020C18E0 + 0xBBE0 + 0x3540` = `0x020D0A00` (B -> C)
* ov17 `0x020D0A00 + 0x72700 + 0x16EE0` = `0x02159FE0` (C -> D)

so a module can use its whole arena and the loader's destination for the next
group is fixed.  That is the static-table proof that these four slots (and no
others) exist — a fifth slot cannot be addressed without overlapping one of
them.

## 3. The measured runtime slot map

`NNK_OVL_TRACE` from the 30 M-step headless boot (`work/p5af/ovl_trace_30m.txt`),
reconciled by `tools/ovl_table.py --rom <rom> --trace <log>`:

| seq | overlay | slot | code / bss | generation |
|---:|---:|---|---|---:|
| 1 | ov17 | `0x020D0A00` | `0x72700` / `0x16EE0` | 1 |
| 2 | ov20 | `0x02159FE0` | `0x1FA0` / 0 | 1 |
| 3 | ov19 | `0x02159FE0` | `0x21E0` / `0x20` | **2** |
| 4 | ov20 | `0x02159FE0` | | 3 |
| 5 | ov19 | `0x02159FE0` | | 4 |
| 6 | ov20 | `0x02159FE0` | | 5 |
| 7 | ov14 | `0x020C18E0` | `0xB5C0` / `0x30E0` | 1 |
| 8 | ov06 | `0x02079F80` | `0x4CA0` / 0 | 1 |

All eight loads happen before step 30,000,001; every `dest` equals the table's
`base` for that id (`0/21` mismatches), and every `ram_size` equals the table's
code size.  Arena D is reloaded five times (19/20 alternating) in the boot path
alone — the generation problem Phase-4F/4I were built for is real and early.
Arena A's first module is **ov06**, which is why the movie driver at
`0x0207B1D0`/`0x0207B218` (+0x1250/+0x1298) runs at all in the intro: it *is*
ov06, and `0x0207B218` also lies inside ov0..ov13's ranges (aliasing, §7).

## 4. Reconciliation: why earlier phases saw only two slots

Three separate reasons, all now measurable:

1. **The shipped image had no registry.**  `grep io_ovl_init` on
   `arm9_thumb_ovl_itcm_p5u.c` returns nothing, so the build used by every
   acceptance run since Phase-4H could not log a load at all.  The slot map in
   `PHASE4G_OVERLAY_SLOTS.md` came from a run of a different generated image.
2. **Horizon.**  Phase-4G's trace came from a 13.1 M-step run that halted at
   `0x0205D46C`; by then only ov17 (slot C) and ov20/ov19 (slot D) had loaded.
   Arenas A (ov06) and B (ov14) are loaded later in the boot (still before
   30 M).
3. **Generation.**  Only slot D is reloaded, so a single print of "the resident
   module" is only ever right for one of the two ids (19 or 20).  The registry
   keeps `(id, gen)` per arena and the dumps are named by generation.

## 5. What the runtime does with the table now (code changes)

Canonical text lives in `nnktool/src/recompile.rs` between the
`BEGIN_NNK_OVL_BLOCK` / `END_NNK_OVL_BLOCK` markers; the frozen image gets the
identical text spliced in by `tools/p5af_ovl_table_splice.py`, wired into
`tools/build_split.sh` (no-op when the marker is already present):

* **`ovl_tab[]` carries the whole table** — id, file id, ROM range, base, code
  size, bss, ctor range — parsed from `NNK_ROM_PATH` at startup.
* **`NNK_OVL_TABLE=<path>`** dumps exactly that table, so the numbers above can
  be re-measured from the runtime (the two columns agree: §1 vs `work/p5af/ovl_table.tsv`).
* **Load events are matched by FAT range**, not by an exact first-byte match:
  a read that lands mid-stream is reported once per overlay ("load start not
  seen") instead of being silently ignored, and the generation only advances on
  a real start.
* **`io_ovl_pc_report(pc)`** runs before the `dispatch to untranslated pc`
  halt.  For every overlay whose `base .. base+code+bss` contains the PC it
  prints `arena`, `off`, `ov`, `kind` (`code` / `bss` / `past-code` / **`ctor`**)
  and the resident module + generation.  This is the class the card asked
  about: a ctor hit is a static-constructor call into an overlay, and a code hit
  is a module the image never translated.
* **`NNK_OVL_DUMP=<dir>`** writes the resident arena's live bytes at the first
  dispatch into the module after each load (`arena_<base>_ov<id>_first_gen<g>.bin`)
  and at halt (`..._exit_gen<g>.bin`) — "the memory image after the
  corresponding load", which is what §6 compares.

Historical halt PCs classified by the same table (no game bytes needed to
reproduce, the table is in the ROM):

```
0x0207B218: arena 0x02079F80 +0x1298 [code:ov0-13]
0x0215B34C: arena 0x02159FE0 +0x136C [code:ov19-20]      the in-place decrypt page
0x0215A084: arena 0x02159FE0 +0xA4   [code:ov19-20]      the Thumb entry Phase-4G blx'd into
0x020E1B32: arena 0x020D0A00 +0x11132 [code:ov17-18]
0x0205D46C: NOT in any overlay range (static image)
0x0209DBC4: arena 0x02079F80 +0x23C44 [code:ov1-2,7 ctor:ov0]   ov0's ctor array
```

The `0x0207B218`-class addresses in ov00..ov13 space are therefore **not** an
unmapped overlay: they are arena A, and the class was "the dispatch image has no
case for this PC" (Phase-5AA fixed the specific site; the runtime now names the
candidate module instead of halting with a bare address).

## 6. Byte cross-checks

### (a) dsd extract vs our own decompressor — 20/21 identical

`tools/ovl_table.py --xcheck 1 --dsd ... --images ...` (images from
`tools/dump_ovl_targets.py`, i.e. ndspy NITRO code-compression, independent of
dsd): **20 of 21 files byte-identical**, e.g.

```
ov00 155936 bytes byte-identical (sha1 d4ad34969a86)
ov06  19616 bytes byte-identical (sha1 bb367a2f36bb)
ov14  46528 bytes byte-identical (sha1 fd1367c2cd90)
ov17 468736 bytes byte-identical (sha1 1ed40b96ec77)
ov20   8096 bytes byte-identical (sha1 bb6a8e891356)
```

`ov19` differs in 5775 of 8672 bytes, starting at +0x210, in a 3-of-4-bytes
pattern.  That is not a decompressor disagreement: at +0x136C

```
dsd  ov019.bin: 0F C0 8F E1 0C C0 1C E0 00 C0 A0 03 ...   (valid ARM code)
ours / ROM    : FE 5D 63 9E B2 6D 5E 7F F2 CE DE 43 ...   (ciphertext)
```

and Phase-4G recorded the melonDS memory at `0x0215B34C` as `E0 D7 15 02 00 00
00 00 E0 D7 15 02 0F C0 8F E1 …` — the dsd bytes are the *decrypted* ARM stub.
**`dsd` decrypts ov19 during extraction** (this ROM's Level-5 overlay
protection); the extract is the post-decrypt module, ours is the stored form.
That is a usable oracle: Phase-4I reconstructed this module's decrypted pages
from a native RAM dump; `work/dsd_extract/arm9_overlays/ov019.bin` is the same
data without a capture.

### (b) runtime memory after the load vs the extracted file — 4 exact

`NNK_OVL_DUMP` + `tools/ovl_table.py --bin ... --arena ... --id ...`:

| module | dump | bytes | match |
|---|---|---:|---:|
| ov14 | `arena_020C18E0_ov14_first_gen1.bin` | 46528 | **100.00 %** (46528/46528) |
| ov17 | `arena_020D0A00_ov17_first_gen1.bin` | 468736 | **100.00 %** (468736/468736) |
| ov06 | `arena_02079F80_ov06_exit_gen1.bin` | 19616 | **100.00 %** (19616/19616) |
| ov20 | `arena_02159FE0_ov20_exit_gen5.bin` | 8096 | **100.00 %** (8096/8096) |
| ov19 | `arena_02159FE0_ov19_first_gen2.bin` | 8672 | 8666/8672 vs the *stored* image |

So the loader's output in the native is the extracted overlay, byte for byte,
for four modules in three different arenas — including a 468 KiB module (ov17)
that occupies slot C for the whole boot.

ov19's dump is the stored (pre-decrypt) form — 8666/8672 = 99.93 % against our
ROM image, the 6 differing bytes being two guest-written words at +0x206C and
+0x20A8 — and 2891/8672 against the dsd (decrypted) file.  The guest decrypts
the module in place with its own rolling-XOR pass (`0x0215B468`, Phase-4G/4I);
the native reaches the integrity check at `0x0205D448` with
`r4 = 0x2FBB82E1` = the expected hash, i.e. the decrypt is correct *in the
guest*, and the module's stored image is what the loader must place there.
The dispatch image's arena-D window (`work/phase3a/ovl_p4f4.bin` at
`0x02159FE0`) is also the stored form (8666/8672 vs the stored image, 2891 vs
the decrypted one), matching the live bytes at the first dispatch.

## 7. Aliasing and ctor overlap in arena A

`tools/ovl_table.py --pc 0x0209DBC4` prints
`arena 0x02079F80 +0x23C44 [code:ov1-2,7 ctor:ov0]`: the address is ov0's
static-constructor **array** and simultaneously inside the *code* of ov1, ov2
and ov7.  Any dispatch image that treats arena A as one code region must pick a
generation; the table makes the ambiguity explicit instead of invisible.

## 8. Regression — the Phase-5AD acceptance re-run on this build

Runner: `work/split/p5af_bin`, i.e. the frozen image with the Phase-5X,
Phase-5AE and Phase-5AF splices applied by `tools/build_split.sh`.  Unscripted,
canonical YUV pair, `NNK_STEP_LIMIT=2400000000`, `NNK_GX_SHOT_EVERY=25`:

```
movie: stream 0 codec=0x0227834C source=op_u.yuv frames=1317 step=2256160152
movie: stream 1 codec=0x022E8B74 source=op_d.yuv frames=1317 step=2256257061
done steps=2400000001 halt_pc=0x01FFC4FC halt=step budget exhausted
gx: 4291 frames presented
untranslated pc lines: 0
```

Band (`tools/p5ad_band.py`, same frame range as Phase-5AD):

| capture | TOP best ref / corr | BOTTOM (op_d) best ref / corr |
|---|---|---|
| f4125 | `op_u` f16 **0.9986** (MAE 1.62) | f16 **0.9990** |
| f4150 | `op_u` f20 **0.9986** (MAE 1.53) | f20 **0.9990** |
| f4175 | `op_u` f24 **0.9986** (MAE 2.01) | f24 **0.9992** |
| f4200 | `op_u` f27 **0.9989** (MAE 2.73) | f27 **0.9992** |
| f4225 | `op_u` f30 **0.9990** (MAE 2.92) | f30 **0.9991** |
| f4250 | `op_u` f34 **0.9990** (MAE 2.96) | f34 **0.9991** |
| f4275 | `op_u` f37 **0.9990** (MAE 2.96) | f37 **0.9991** |

Same reference frames at the same display frames and the same correlations as
Phase-5AD's table (0.9986..0.9990 top, 0.9990..0.9992 bottom) — the overlay
table changed nothing the guest can see.  The session-start log line is 96,792
steps earlier than Phase-5AD's (2,256,160,152 vs 2,256,256,944), which is
~45 display frames of *instruction* count: the captures still name the same
reference frames, so the session starts at the same display frame (f4033).

## 9. Tests, build and what a regenerated image needs

* `cargo test`: **44 passed** (suite 43 -> 44).  New class-43 test
  `phase_5af_overlay_map_is_table_driven` runs
  `tools/ovl_table.py --self-test` (a synthetic 4-overlay ROM built inside the
  test — no game bytes) and locks the OVT field offsets, the
  code/bss/ctor/aliasing classification, every field the runtime assigns, the
  `io_ovl_pc_report` hook on the untranslated-PC path, the arena dump hooks, and
  the fact that `tools/p5af_ovl_table_splice.py` copies the canonical block
  instead of a second copy of it.
* `tools/build_split.sh` now applies the Phase-5AF splice after the Phase-5X and
  Phase-5AE ones, gated on `io_ovl_pc_report` (so a future full
  `recompile-all` that already carries the text is a no-op).
* `tools/ovl_table.py` is new and dependency-free (`--table`, `--trace`,
  `--pc`, `--xcheck`, `--self-test`).
* `SKIP_MELONDS=1 sh tools/verify_phase2a.sh` runs green end to end: sections
  A / A2 (PASS) / B / C / D / E / F, strict common prefix **1000 / 1000
  melonDS PCs (100.0 %)** against a mono image regenerated from the edited
  `nnktool/src/recompile.rs` (so the canonical prelude compiles and does not
  move the PC sequence).
* `ROM_MAP.md` §overlay table needed no correction (it already matched the
  extract); this phase is the runtime finally using it.

## 10. Remaining / honest notes

* The registry tracks **resident id + generation** but the dispatch image is
  still one case per PC, so a generation-dependent case still needs a
  resident-module check (Phase-4G's open item, unchanged).  What changed is that
  the runtime can now *say* which module and generation a PC belongs to, so the
  next generation-aware dispatch has its input.
* `ovl_arena[8]` is a fixed eight-slot array; the ROM has four arenas, so six
  slots are spare.  If a future ROM variant had more, the array would need
  growing (checked: the table here yields four distinct `base` values).
* The dsd extract's `ov019.bin` decrypted module is a candidate to *replace* the
  arena-D window in the dispatch image (no capture needed), but the dispatch
  image currently works with the stored form and changing it is a separate
  experiment — not done here.
