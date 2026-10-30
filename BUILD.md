# BUILD.md — split dispatch build + content-hash object cache (nnktool images)

Phase-3A4. This document describes how the nnktool-generated dispatch image is
turned from one unbuildable translation unit into many small, independently
cached ones, and how to reproduce the numbers below.

Ported from DQ8's `tools/{split_dispatch.py,build_split.sh}`
(`/home/hermes/DQ8/tools/`), adapted to nnktool's emitter format.

## 1. The problem

`nnktool recompile-all` emits **one** C program whose `run()` contains a single
flat `switch (pc)` with one `case 0xADDRu:` label per translated instruction.

Current Phase-3A Thumb image (`work/phase3a/arm9_thumb.c` in the main checkout):

| metric | value |
|---|---|
| file size | 23.6 MB |
| lines | 634,616 |
| case labels | 73,120 |
| PC range | `0x02000218` .. `0x0206CA04` |

`cc` needs **~295 s wall and 2.6 GB peak RSS** to compile that as a single TU at
`-O0`, and more at `-O1`/`-O2` (a mono `-O1` build was SIGTERM'd mid-run by a
prior worker). Several worker runs died on it. It also means *every* change to
*any* translated instruction forces a full 5-minute rebuild.

## 2. The mechanism

`tools/split_dispatch.py` rewrites the image C into:

```
work/split/c/
  nnk_rt.h        extern decls for the runtime symbols the cases reference,
                  band-shift constants, and prototypes for ALL 768 possible
                  band functions (stable cache keys)
  nnk_main.c      the runtime prelude (IO engine, guest memory, SWI HLE,
                  trace, checkpoint/debug) + a 768-entry band dispatch table
                  + main()   — compiled with the RUNTIME flag set (-O2)
  nnk_part_NNN.c  one TU per 32 KiB PC band; each is
                  `int nnk_part_NNN(uint32_t *pc_io)` carrying only that
                  band's cases  — compiled with the GUEST flag set (-O0)
```

14 part files exist today (768 possible bands, only 14 occupied).

### 2.1 Why parts are keyed by PC *address*, not file position

A part is `band = (pc - GBASE) >> NNK_BAND_SHIFT` — an **address band**, not
"group N of the file". Adding a recomp root that introduces a new translated PC
only changes the one band that PC falls into, plus `nnk_main.c`'s dispatch
table. Every other part keeps its bytes and its cache stamp.

The naive alternative (split by file position) would shift every subsequent
group whenever a new instruction is recovered, invalidating the whole cache on
every recomp-root change.

To keep the cache key stable, `nnk_rt.h` always declares **all 768** possible
band functions, even though only 14 exist. A new band appearing therefore does
not change the header's bytes, so it does not invalidate every object.

Stale `nnk_part_*.c` / `.o` from a previous, larger manifest are pruned after a
successful split (both in the splitter and in `build_split.sh`).

### 2.2 Bare `return;` — the nnk-specific hazard

DQ8's case bodies never return early. nnk's contain **17 bare `return;`**
statements (undefined-instruction traps and failed SWI HLE) which, in the mono
build, return from `run()`. Inside a part function a bare `return;` would
instead return from the *part*, and the caller would treat it as "PC not in
this band" and report `untranslated pc` — a silent semantic change.

So the splitter rewrites every bare `return;` to `return 2;` (a halt sentinel),
and `nnk_main.c`'s dispatch maps `rc == 2` back to a `run()` return:

| rc | meaning |
|---|---|
| 1 | handled — `*pc_io` holds the next PC |
| 2 | case body executed a bare return (guest halt) → return from `run()` |
| 0 | PC not in this band's case set → caller reports "untranslated" |

A *valued* `return`, a `goto` or a `continue` inside a case body is **not**
representable this way; the splitter aborts with a diagnostic rather than
miscompile. (None occur in the current image.)

### 2.3 What is preserved exactly

Every translated instruction body, every `trace_pc()` call, the per-instruction
`steps` cadence (`nnk_main.c` increments `steps` once per dispatch, exactly like
the mono loop), the `default:` branch's `untranslated pc=0x%08x` message +
halt, and the step-budget check. **The split changes structure only, never
semantics.** §4 proves it.

## 3. Cache invalidation rules

Each object has a sidecar stamp `obj/<name>.o.stamp` containing a SHA-256 over
four inputs. All four must match for the object to be reused:

1. the **source** file's contents (`sha256sum nnk_part_NNN.c`)
2. the **shared header** `nnk_rt.h` contents (included by every TU)
3. the **exact compiler command line**, including the full flag set
4. the **compiler version** (`cc --version`)

Consequences, all verified in §5:

| change | rebuilds |
|---|---|
| nothing (no-change rebuild) | 0 TUs — link only |
| one band's source (e.g. new instruction in band 005) | **1 TU** |
| the runtime (`nnk_main.c`) source | 1 TU (nnk_main.o) |
| `nnk_rt.h` | all TUs (it is part of every key — by design) |
| guest flag set | all guest TUs, not the runtime TU |
| runtime flag set | runtime TU only, not the guest TUs |
| compiler upgrade | all TUs |

A failed compile deletes the object *and* its stamp, so a stale `.o` can never
be linked.

## 4. Equivalence: mono vs split

Built the **same** `work/phase3a/arm9_thumb.c` two ways — mono `cc`, and the
split pipeline — and ran both on `work/phase3a/arm9_stored_padded.bin` with
`NNK_ROM_PATH` pointing at the real `.nds`:

```
env NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" <binary> \
    work/phase3a/arm9_stored_padded.bin > trace.txt 2> hle.txt
```

Result — **the full 10,000,003-line guest-PC traces are byte-identical**:

```
6a2cd8c5d4a7f1e9bcc8a2d324a9af0b879ac62f3225dd211115764c32667805  trace_mono.txt
6a2cd8c5d4a7f1e9bcc8a2d324a9af0b879ac62f3225dd211115764c32667805  trace_split_10m.txt
```

`cmp` reports no difference. The stderr side is identical too:

```
done steps=200000001 traced=200000000 distinct_pcs=1032 hle_hits=1
     halt_pc=0x0201C4B8 halt=step budget exhausted
halt state: r0=00000000 r1=00001000 r2=02FFFC00 r3=00000000 r4=00000001
            r5=0000000D r6=027E0080 r7=00000000 r13=027E3B58 r14=0201C4B8
            nzcv=0110
```

Both runs hit the 200 M step budget at the same PC with the same register
file — the guest state is bit-identical, not just the trace.

Reproduce:

```bash
cd /home/hermes/NiNoKuni/worktrees/build-split
NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=10000000ull" \
  tools/build_split.sh work/split/arm9_split
NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=10000000ull" NNK_MONO=1 \
  tools/build_split.sh work/split/arm9_mono
# (both need the same TRACE_LIMIT or the traces differ only in length)
```

> `NNK_STEP_LIMIT` is read by the generated `main()` into `step_limit`, but
> `run()` checks the compile-time `STEP_LIMIT` macro, so the env var currently
> has no effect. That is an **emitter** quirk affecting mono and split equally;
> it is documented here but out of scope for this build-plumbing task.

## 5. Timings (honest, measured on this worker: 24 cores, 23 GB RAM, gcc 13.3)

Build of the 23.6 MB / 73,120-case image:

| build | wall | peak RSS |
|---|---|---|
| **mono, single TU, `-O0`** | **294.97 s** | **2,685,496 KB (2.6 GB)** |
| **split, cold, 6 jobs** | **14.87 s** | **298,940 KB (292 MB)** |
| split, warm no-change (re-split + relink) | 5.36 s | 104,780 KB |
| split, warm, `NNK_SPLIT_SKIP_REGEN=1` (pure relink) | **0.56 s** | — |
| split, touch one band's source | 5.43 s (1 TU compiled, 14 reused) | — |
| split, flip guest `-O2` → `-O0` | (runtime TU reused, 14 guest TUs rebuilt) | — |

The last row is the asymmetric-invalidation guarantee: the runtime flag set is
hashed only into `nnk_main.o`'s key and the guest flag set only into the part
keys, so switching guest optimization levels never touches `nnk_main.o`.

That is a **~20x cold-build speedup** and a **~500x warm-build speedup**, with
peak memory down ~9x. The split also parallelises: `BUILD_JOBS` bounds the pool
(default 6; the machine has 24 cores, so raising it helps further).

## 6. Runtime cost (be aware)

The split binary is **slower at run time** than the mono binary when guest
parts stay at `-O0`, because cross-TU calls to the runtime helpers
(`ld32`/`st32`/`trace_pc`/`hle_swi`) are no longer inlinable into each case
body. `-O2` on the runtime TU alone does not recover it (the calls are the
cost, not the helper bodies).

The fix is to raise the **guest** flag set. Measured on the same 200 M-step run
with a 10 M-line trace:

| binary | run wall | build wall | trace |
|---|---|---|---|
| mono, single TU, `-O0` | 5.68 s | 294.97 s | reference |
| split, guest `-O0` + runtime `-O2` | 28.73 s | 14.87 s | **identical** |
| **split, guest `-O2` + runtime `-O2`** | **4.23 s** | 56.53 s | **identical** |

So guest `-O2` makes the split build **faster than mono at run time too**
(4.23 s vs 5.68 s), while still building ~5x faster than mono. The cost is a
56 s cold build instead of 15 s — and the cache absorbs it, since unchanged
TUs are still skipped.

Recommended: keep guest `-O0` while iterating on the *image* (fastest
rebuilds, and a stable `-O0` object is the most faithful artefact to diff),
switch to guest `-O2` for long runs.

The flag set is part of the cache key, so flipping `-O0` → `-O2` rebuilds only
the guest TUs and leaves `nnk_main.o` cached.

Every flag set produces a **byte-identical guest-PC trace**: the `-O2` split
trace hashes to the same SHA-256 as the mono `-O0` trace
(`6a2cd8c5…67805`), so optimization level affects speed only, never guest
semantics.

## 7. Knobs

| knob | default | effect |
|---|---|---|
| `BUILD_JOBS=N` | 6 | parallel compile pool size |
| `NNK_CC=cc` | `cc` | compiler; its version is in every cache key |
| `NNK_SPLIT_CFLAGS` | `-std=c11 -O0 -g0 -w` | guest part flags (`nnk_part_NNN.c`) |
| `NNK_SPLIT_RUNTIME_CFLAGS` | `-std=c11 -O2 -g0 -w` | runtime flags (`nnk_main.c`) |
| `NNK_CFLAGS_EXTRA` | *(empty)* | appended to **both** sets (e.g. `-DTRACE_LIMIT=...`) |
| `NNK_SPLIT_SKIP_REGEN` | *(unset)* | skip `split_dispatch.py`, rebuild only the C |
| `NNK_MONO` | *(unset)* | build the untouched single-TU image instead (no cache) |
| `NNK_IMAGE_C` | `<main>/work/phase3a/arm9_thumb.c` | which generated image to split |
| `SPLIT_DIR` | `work/split/c` | split output + `obj/` cache location |

`--band-shift` on `split_dispatch.py` (default 15 = 32 KiB bands) trades part
count against part size; it is not currently exposed as a `build_split.sh`
knob. Changing it regenerates everything (new band indices → new file names →
new cache keys), which is correct but not incremental.

## 8. Layout

```
tools/split_dispatch.py   the splitter (nnktool emitter format)
tools/build_split.sh      regenerate-or-reuse, split, cached parallel build, link
work/split/c/             generated nnk_rt.h + nnk_main.c + nnk_part_NNN.c   (gitignored)
work/split/c/obj/         cached .o + .o.stamp sidecars                        (gitignored)
work/split/arm9_split     the linked runner                                     (gitignored)
```

`work/` is gitignored in this repo (it is runtime/build state, never game data).
Only the scripts and this document are committed.

## 9. Phase-4B: the ITCM dispatch image (two-region merge)

Code at `0x01FF8000..0x01FFFFFF` (the loader's auto-load block, 0x540
bytes from image `0x0206C5C0`) has no cases in the flat-window image, and
nnktool's recovery indexes its single image at `addr - base`, so the ITCM
block is translated as a **second, independent image** and spliced in:

```
tools/extract_itcm_blob.py <decompressed-arm9.bin> <blob>   # 0x540 @0x0206C5C0
tools/mk_itcm_seeds.py <extended melonDS trace> 0x540 <seeds.tsv>
nnktool recompile-all <blob> <itcm.c> --base 0x01FF8000 --entry 0x01FF8000 --seeds <seeds.tsv>
python3 tools/merge_itcm_cases.py <flat.c> <itcm.c> <merged.c> <trace>
NNK_IMAGE_C=<merged.c> tools/build_split.sh
```

`merge_itcm_cases.py` splices every `case 0x01FF8xxxu:` block of the ITCM
image into `run()` immediately after `switch (pc) {` (the structure
`split_dispatch.py` expects) and reports observed trace PCs that got no
case.  The splitter maps the single band below `GBASE`
(`(0x01FF8000 - 0x02000000) >> 15 == -1`) to its own part
`nnk_part_itcm.c` / `nnk_part_itcm`, dispatched by an explicit
`pc >= NNK_ITCM_LO && pc < NNK_ITCM_HI` branch in `nnk_main.c`; the
prototype is always in `nnk_rt.h` so the header (a cache key) stays stable
when ITCM cases come and go.

The merged image used for Phase-4B/4C runs:

```
NNK_IMAGE_C=/home/hermes/NiNoKuni/work/phase3a/arm9_thumb_itcm.c \
NNK_CFLAGS_EXTRA="-DTRACE_LIMIT=20000000ull" tools/build_split.sh
```

## 10. Phase-4C: ARM9 IRQ/exception model, mode banking, main-RAM mirror

Implemented in `nnktool/src/recompile.rs` (`RUNTIME_ALL`, and mirrored in the
single-function runtime because the Phase-1A entry translation contains the
crt0's `msr CPSR_c`):

* **IRQ machinery** — scripted IF raises + architecturally gated exception
  entry + BIOS epilogue HLE, regenerated from a melonDS capture by
  `tools/gen_irq_script.py` (`tools/add_irqtrace.py` is the melonDS hook).
  Measured: 624 IF raises, 3 deliveries in 10M ARM9 PCs.
* **Mode banking** — r13/r14 per mode (USR/SYS shared, FIQ, IRQ, SVC, ABT,
  UND) plus an SPSR per mode; `msr CPSR_c` switches banks; `msr SPSR_*` writes
  the current mode's SPSR and never the CPSR; `ldm/stm ...^` reads/writes the
  USER bank for r13/r14; `subs pc, lr, #4` / `movs pc, lr` implement the ARM
  exception return (PC = value, CPSR <- SPSR, bank swap).
* **Main-RAM mirror** — every address in 0x02000000..0x02FFFFFF aliases the
  4 MiB main RAM through `addr & 0x3FFFFF`, exactly like melonDS's
  `case 0x02000000` in ARM9Read32/Write32 (`win_off()`); scripted ARM7-side
  writes use `main_ram_ptr()`. The SDK depends on it: boot params live at
  0x027FFE00 (= main-RAM 0x3FFE00) and the 0x02FFFE80 / 0x02FFFF8C flags are
  aliases of main-RAM 0x3FFE80 / 0x3FFF8C.

One-shot iterate/reproduce helper (emitter build → both recompile-all runs →
merge → split build → native run → trace compare):

```
sh tools/it_irq.sh /tmp/nat_pc.txt
```

Diagnostics knobs: `NNK_MODE_TRACE=1` logs every bank switch
(`mode a->b step=N sp=0x.. lr=0x..`), `NNK_DEBUG_AT_PC=<pc>` dumps registers
at a PC, `NNK_STEP_LIMIT=<n>` caps the run.

melonDS ground-truth hooks added for this phase:

* `tools/add_regprobe.py` — per-PC register dump including the banked SVC/IRQ
  r13/r14 and SPSR (`MELONDS_REGPROBE_PATH`, `MELONDS_REGPROBE_PCS`).
* `tools/add_memwatch_window.py <lo> <hi>` — extra MemWatch main-RAM window
  (logs the writing CPU, its PC and the ARM9 PC-trace index), on top of
  `tools/melonds-phase4a-watch.patch`.
