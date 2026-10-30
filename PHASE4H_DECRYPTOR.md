# Phase-4H — the stub-page decryptor: who decrypts it (melonDS), why native never runs it

Follows Phase-4G (merged `c40b3c5`). Scope was: identify the guest routine that
decrypts the stub page at `0x0215B34C`, root-cause why the plaintext stores
never land in native RAM, fix it, and push toward the first picture.

**Outcome: stop-report, with the decryptor identified and the barrier named by
measurement.** The decryptor is the guest's own rolling-key in-place XOR at
`0x0215B468` (the store loop is `0x0215B4B0..0x0215B4C4`, store at
`0x0215B4BC`). In the native run it is **never executed at all** (0 dispatches
at `0x0215B468`, `0x0215B4BC`, `0x0215B400`). The first differing word is the
return link `r14` at `0x020D5784` (`bx lr`): melonDS returns to `0x0215B400`
(the decrypt chain), native returns to `0x0200DA39` (the `blx r0` call site in
the static image). Class: **runtime memory-state / dispatch-generation gap** —
not a decoder bug (see §5).

## 0. Environment correction (measured, matters for every future run)

The native guest image must be the **stored (still compressed) ARM9 image**:
`work/phase3a/arm9_stored_padded.bin`.

With `work/phase3a/arm9_thumb_p4g.image.bin` (the *decompressed* image) the run
stalls at step ~3.63M in the crt0 decompressor (`0x020009B8` loop), takes the
`tst r5,#128` branch differently from melonDS (r5 comes from a compressed-block
trailer word at `0x02049320`, which is code in the decompressed image and the
trailer `0x0B045328 / 0x00023850` in the stored image — melonDS memory at the
first `0x02000990` hit holds exactly the stored-image words), and never reaches
the overlay. With the stored image the run reproduces Phase-4G **exactly**:

```
done steps=13134253 traced=13134252 distinct_pcs=5130 hle_hits=7251
     halt_pc=0x0205D46C halt=dispatch to untranslated pc
halt state: r4=F220F0F6 r5=00000013 r6=00000014 r14=0205D471
ovl_load seq=1 id=17 rom=0x00145E00 dest=0x020D0A00 gen=1
ovl_load seq=2 id=20 rom=0x001ABA00 dest=0x02159FE0 gen=1
ovl_load seq=3 id=19 rom=0x001A9800 dest=0x02159FE0 gen=2
```

## 1. The decryptor, from melonDS ground truth

Probe: `MELONDS_MEMWATCH_{PATH,N,LO=0x0215B340,HI=0x0215B400}` (the Phase-4F
watch, with `Hit()` made **exclusive** for an explicit range so a targeted
watch is not flooded by the pre-entry firehose) plus
`MELONDS_REGPROBE_PCS=0x0215B468`.

Writer census for `[0x0215B340,0x0215B400)` over the whole boot (507 writes):

| pc9 (ARM9 writer) | writes | what |
|---|---|---|
| `0x020009BC` / `0x02000998` | 280 / 104 | crt0 decompressor byte stores (pre-overlay scratch use of the arena) |
| `0x02016B44` | 96 | bulk copy into the arena |
| **`0x0215B4BC`** | **27** | **the decryptor's plaintext stores** — the only writes that produce the checked bytes |

The 27 stores are `0x0215B34C..0x0215B36C`, `0x0215B388..0x0215B3A8`,
`0x0215B3C4..0x0215B3E4` — three 9-word blocks with 6-word gaps, i.e. the
descriptor list decrypts several 36-byte stubs, not one contiguous run. The
first one is exactly the word range the integrity check at `0x0205D420..58`
hashes.

The routine (translated text, matches the ROM overlay 19 image at
`0x0215B468`, offset `0x1488`, which is plaintext — the decryptor is *outside*
the range it decrypts, so there is no bootstrap problem):

```
0x0215B468  push {r4,r5,r6,lr}
0x0215B46C  mov  r1,#0
0x0215B470  sub  r2,r0,#12
0x0215B474  ldr  lr,[pc,#164]      ; seed A
0x0215B478  ldr  ip,[pc,#164]      ; seed B (region-end bias)
0x0215B47C  str  r1,[r2,#8] / str r1,[r2,#4] / str r1,[r0,#-12]
0x0215B48C  ldr  r1,[r0,#4]        ; descriptor end
0x0215B490  ldr  r3,[r0]           ; descriptor start
0x0215B494  sub  r1,r1,ip / sub r1,r1,#0x2100 / lsr r1,r1,#2
0x0215B49C  sub  r4,r3,#0x2100     ; r4 = start-0x2100
0x0215B4A4  mov  r6,lr             ; rolling key
0x0215B4A8  add  r5,r4,r1,lsl #2   ; r5 = end
0x0215B4B0  ldr  r1,[r4]
0x0215B4B4  eor  r3,r1,r6
0x0215B4B8  sub  r1,r3,r3,lsr #8
0x0215B4BC  str  r3,[r4],#4        ; <- plaintext store
0x0215B4C0  eor  r6,r6,r1
0x0215B4C4  cmp  r4,r5 / blo 0x0215B4B0
```

Call site (RegProbe at `0x0215B468`, first hit): `r0=0x0215C110` (descriptor,
inside the same arena), `r5=0x00000013`, `r6=0x00000014` (overlay ids 19/20),
`r14=0x0215B408`, `irq_lr=0x02016B38` (it is reached from the card-read wait
path). So the overlay's own loader decrypts its stub page after the card
transfer, exactly as the DQ8-style overlay pattern predicts.

Ground truth at the integrity check (`REGPROBE_PCS=0x0205D448`,
`MEM=0x0215B340:0x120`): `r0=r4=0x2FBB82E1`, and
`0x0215B34C: E0 D7 15 02 00 00 00 00 E0 D7 15 02 0F C0 8F E1 ...` — the
decrypted stubs, identical in shape to the three-decrypted-block layout above.

## 2. Native: the decryptor is never entered

Same PCs on the native run (`NNK_DEBUG_AT_PC`, `NNK_HALT_AT_PC`):

| PC | melonDS | native |
|---|---|---|
| `0x0215B400` | 1 | **0** |
| `0x0215B408` | 2 | **0** |
| `0x0215B468` (decryptor entry) | yes (probe) | **0** |
| `0x0215B4BC` (store) | 27 | **0** |

So the Phase-4G native memory at `0x0215B34C` is ciphertext because **nothing
ever decrypts it**, not because the stores are mis-decoded.

## 3. Where the two runs part (measured, ghost-aware)

Direct PC-trace comparison needs one correction first: the melonDS ARM9 trace
logs the **second halfword of every 32-bit Thumb instruction** as an extra
`start+2` line (e.g. `0x020e28e0` then `0x020e28e2` before the `blx` target),
so a strict prefix compare reports a false divergence at the first Thumb BL.
`tools/p4h_trace_diff.py` (new) uses `difflib.SequenceMatcher` and skips those
single `prev+2` deletions.

* Full-window strict prefix: 3,643,409 PCs (melonDS `0x02016b4c` vs native
  `0x02016b3c`, the card word-available poll at `0x02016B38`: a **timing skew
  of one poll iteration**, not a control-flow break — the run resyncs).
* Overlay subsequence (`0x020C0000..0x02180000`), ghost-aware: the native
  reproduces melonDS's overlay execution **instruction for instruction** up to
  melonDS's overlay-PC index 17808, where melonDS continues:

```
... 0x020d577c 0x020d577e 0x020d5780 0x020d5782 0x020d5784
    0x0215b400 0x0215b404 0x0215c104 0x0215c108 0x0215c10c
    0x0215b468 0x0215b46c ...            (the decryptor)
```

while the native's overlay execution **ends** there (`0x020d5784` → next
in-window PC `0x0200DA38`, the static image) and never returns; 12.7M more
steps run in the static image and end at the integrity check.

**First differing word:** `r14` at `0x020D5784` (`bx lr`, Thumb, ROM overlay 17
at offset `0x4D84`, plaintext `70 47`):
melonDS `r14 = 0x0215B400`, native `r14 = 0x0200DA39` (measured:
`NNK_DEBUG_AT_PC=0x020D5784` → `r14=0200DA39`; the function was entered from
`0x0200DA34 blx r0`, which sets exactly that link).

## 4. The two dispatch generations are complementary (measured)

Running the *Phase-4F* image (whose overlay cases were compiled from the
**pre-decrypt** ROM generation) on the same stored image:

```
done steps=13142564 ... halt=SWI not implemented
     halt_pc=0x0215B350 svc 0x5E6DB2
halt state: r4=2FBB82E1 r5=00000013 r6=00000014 r14=0205D45C
```

`r4 = 0x2FBB82E1` — **the integrity hash matches and the tamper path is not
taken**: with that image the guest's decryptor *does* run and the plaintext
lands in native RAM. It then halts at `0x0215B350`, because the pre-decrypt
image has no case for the *now decrypted* instruction stream (the first
decrypted stub word `0x0215D7E0` is not valid ARM code; the image's
ciphertext-derived case there is `svc`).

So the two halves of the fix both exist and are each measured:

1. the pre-decrypt (ROM) generation lets the overlay's loader reach and run
   `0x0215B468` → the guest's own decrypt lands in RAM → hash matches;
2. the post-decrypt generation (Phase-4G slot capture) supplies the *decrypted*
   cases that must run once RAM holds plaintext.

Neither image alone can do both, per PC address. This is the registry problem
Phase-4G named (`work/ovl_targets/conflicts.tsv`, 770 runs) reduced to the
barrier at hand: for this barrier the conflict set is the arena
`0x02159FE0` generation-2 image plus the stub-page/decrypt-target runs.

## 5. Class of the gap (honest)

Not a decoder/dataflow bug. Evidence: for the full overlay prefix the native
executes **the same PC sequence** as melonDS (ghost-aware subsequence match to
index 17808, ~17.8k overlay instructions), and all arithmetic/dataflow on that
path is therefore already correct. The split is at a cross-arena **return
link** (`bx lr` → different in-window target), i.e. a difference in runtime
memory/state that only exists because the two images were compiled from
different generations of the same overlay arena:

* the code that *performs* the decrypt is only reachable on the pre-decrypt
  image;
* the code that *runs after* the decrypt only exists on the post-decrypt image.

Class: **generation-aware dispatch / runtime image-state**, the same family as
the Phase-4G registry gap, not the Phase-3A/4F decoder family.

## 6. Bounded handoff (next steps, in order)

1. Build one image with **two generations for the arena `0x02159FE0`**: pre-
   decrypt (ROM) cases for the loader/decryptor path (`0x020D5xxx` callers,
   `0x0215B468..0x0215B4C8`, `0x0215C1xx` descriptor code) and post-decrypt
   (Phase-4G capture) cases for the stub page `0x0215B34C..0x0215B3E4` and the
   other decrypted runs. The pre-decrypt run proves (1) and the post-decrypt
   run proves (2); only their union satisfies both.
2. Equivalently (and more cheaply the first time): apply the measured
   rolling-key transform to the native's RAM at the point the guest calls
   `0x0215B468` (it is a plain XOR stream keyed from `[0x0215B474]` and
   `[0x0215B478]` with the descriptor bounds) — but keep (1) as the
   architecture, because the guest repairs the page itself on real hardware.
3. Re-run and expect: hash `0x2FBB82E1`, no tamper `blx 0x0215A084`, and the
   run continuing past `0x0215B350` into the overlay's real entry.

## 7. Tooling / reproduction

```sh
# melonDS ground truth (probe tree: /home/hermes/tools/melonDS, build-ptrace)
MELONDS_MEMWATCH_PATH=w.txt MELONDS_MEMWATCH_N=50000 \
MELONDS_MEMWATCH_LO=0x0215B340 MELONDS_MEMWATCH_HI=0x0215B400 \
MELONDS_REGPROBE_PATH=r.txt MELONDS_REGPROBE_MEM_PATH=m.txt \
MELONDS_REGPROBE_MEM=0x0215B340:0x120 MELONDS_REGPROBE_PCS=0x0205D448 \
MELONDS_REGPROBE_EXIT_PC=0x0205D448 MELONDS_REGPROBE_EXIT_HIT=1 \
xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"

# native (the STORED image, see §0)
SPLIT_DIR=out/split/c NNK_IMAGE_C=work/phase3a/arm9_thumb_ovl_itcm_p4g.c \
    NNK_GX=1 BUILD_JOBS=12 tools/build_split.sh out/split/arm9_h
NNK_ROM_PATH="Ni No Kuni.nds" NNK_GX=headless NNK_OVL_TRACE=o.txt \
    out/split/arm9_h work/phase3a/arm9_stored_padded.bin

# ghost-aware trace diff
python3 tools/p4h_trace_diff.py <melonds_overlay_pcs.txt> <native_overlay_pcs.txt>
```

melonDS side changes are probe-only and live in `/home/hermes/tools/melonDS`
(not committed here): `MemWatch::Hit()` now treats an explicit
`MELONDS_MEMWATCH_LO/HI` range as exclusive. No ROM bytes are committed.
