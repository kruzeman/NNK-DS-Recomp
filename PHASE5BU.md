# PHASE-5BU — the blue band is a ZERO POSITION MATRIX, not a strip rule

**Card t_aff18b06.** Claim under test: our 3D layer emits extra sliver polygons
because it is missing melonDS's `LastStripPolygon` / clipped-vertex-reuse rule,
so a triangle strip that crosses a polygon or matrix boundary keeps a stale
vertex that melonDS would have replaced; implementing the rule should drop the
four metrics to the reference values.

## Answer (one paragraph)

The card's **mechanism is refuted and the symptom is re-explained**. The strip
rules are *already* a line-for-line port of melonDS (`GPU3D.cpp:1703-1760` for
the triangle/quad-strip vertex shuffle, `1337-1412` for the reuse decision,
`1660-1668` for `LastStripPolygon`), and the reuse path is **geometry-neutral by
construction**: melonDS only takes it when the previous polygon's two shared
vertices carry `Clipped == 0` and its vertex count is unchanged, i.e. when the
stored vertices *are* the raw `TempVertexBuffer` values — so "reuse" and our
"always re-clip from the raw retained vertices" produce identical vertices (the
only extra transform melonDS applies, `W &= 0x00FFFFFF`, is idempotent and a
no-op for this scene's `W = 0x20000`). The detached vertices are **not stale
either**: a new birth-frame tag shows all three raw slots of all 78 slivers
(39 in frame 3372 and 39 in frame 3375) were transformed **in the same frame**,
and the detached clip triple occurs **284 times** in that frame's own
`SubmitVertex` stream. What actually happens is that the guest submits an
**`MTX_LOAD_4x3` whose nine basis words are all zero** (`cmd=17` with
`0,0,0,0,0,0,0,0,0,tx,ty,tz`): the position matrix's 3x3 part is empty, so
`clip = ProjMatrix`'s translation column for *every* vertex submitted under it.
A strip that starts in that state and continues after the matrix is restored
therefore mixes one collapsed vertex with real ones, and the homogeneous clip
(unchanged, correct) turns that into a full-width sliver. melonDS's own GPU
state in the same savestate contains **zero** vertices at that collapse point
in either vertex bank, so melonDS's guest never submitted that vertex set — the
divergence is upstream of the 3D layer, in the guest data the twelve matrix
words are copied from.

## 1. New diagnostics (all env-gated, statics only — `Gx`/`Gxd` untouched)

| knob | what it does |
|---|---|
| `NNK_GX_PSTAT=<f>[:<f2>]` | per-frame polygon counters printed at each `gxd_frame`: `polys` (post-clip), `zero` (a vertex at `sx==0`), `full` (`sx` spans 0..>=256), `gt255` (any `sx>255`) |
| same knob | `pfull ...` line for every full-width polygon: mode, raw clip vertices, screen vertices, the 16-word clip matrix |
| same knob | `clipdeg ...` line whenever a vertex is transformed with an all-zero clip-matrix 3x3: the position/vector/clip matrices, the strip slot and the last 32 FIFO words |
| same knob | `mdbg ...` one line per `0x10-0x1C` matrix command (params, matrix mode, stack pointer, `ZERO3` flag) |
| `NNK_GX_VD` (5BT) | gained `born=<frame>` on each raw vertex |

## 2. The four metrics

Frame 3375, operator savestate, step 1871878703 (`work/repro_5bo/operator_save.nnks`).
"before" = master `da21e08` build, "after" = this tree's diagnostics build.
Nothing functional was changed, so the metrics are the same; the melonDS column
is re-derived here from the savestate's `GP3D` section (field walk: `ProjMatrix`
at payload+2559, `VertexRAM` at payload+7393, `PolygonRAM` at payload+793825,
188-byte polygon records, 64-byte vertex records — the walk is validated by
reading back the known `ProjMatrix` bit-for-bit).

| metric (frame) | before | after | melonDS (last completed frame, bank 0) |
|---|---|---|---|
| polygons reaching the rasteriser | 379 | 379 | 434 |
| polygons with a vertex at `sx==0` | **120** | 120 | **10** |
| polygons spanning `sx=0..>=256` | **39** | 39 | **0** |
| polygons with a vertex at `sx>255` | 47 | 47 | 32 |
| longest run of the band's own colours, rows 19..100 | **72 px** | 72 px | 0 px of those colours (5BT: 7 px by its own predicate) |
| px matching (77,158,215)±10 | 925 | 925 | 0 |
| px matching (60,142,207)±10 | 521 | 521 | 0 |
| px matching (44,125,199)±10 | 198 | 198 | 0 |

(5BT quoted 88 px / 925 / 641 / 285 and 413 / 6 / 0 / 27. The differences are
instrument differences, not scene changes: this phase counts only the three
exact band colours where 5BT counted a blue-ish predicate, and it counts every
non-empty polygon record in melonDS's bank instead of 5BT's filtered subset.
The direction and the conclusion are identical — and this phase's melonDS
numbers are read straight out of the state with a validated field walk.)

## 3. Why the vertices cannot be stale (they are born in this frame)

`born` is the value of the presented-frame counter at the moment
`submit_vertex()` transforms the vertex into its `temp_vtx` slot; the slot tags
are shuffled alongside the vertices in all four strip branches. Every one of
the 78 full-width polygons prints `born=3372,3372,3372` (frame 3372) or
`born=3375,3375,3375` (frame 3375). The detached clip triple
`(-614400,177418,32768|w=131072)` also appears **284 times** in frame 3375's own
`gxv ... clip=` stream. So the vertex is a vertex of the same frame, produced by
the current matrix state — no retained/stale slot is involved, which is the
card's central premise.

## 4. What the detached vertex actually is

```
clipdeg f=3375 slot=0 mode=2 pos_sp=0 vtx=(-5230,32744,-2)
  pos  : 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 4096
  vec  : 0 0 0 0 0 0 0 0 0 0 0 0 41826584 33984676 36521568 4096
  clip : 0 0 0 0 0 0 0 0 0 0 0 0 -614400 177418 32768 131072
```

`pos` is exactly what `MTX_LOAD_4x3` of twelve zero words produces (3x3 = 0,
`[15] = 0x1000`). With an empty 3x3, `clip = memcpy(ProjMatrix);
MatrixMult4x4(clip, PosMatrix)` collapses to the projection's translation
column — the clip triple above — for **every** vertex, independent of position.
`mdbg` shows the command and its parameters, repeated ~100x per frame:

```
mdbg f=3375 cmd=1a mode=2 sp=0 4096 0 0 0 4096 0 0 0 4096
mdbg f=3375 cmd=17 mode=2 sp=0 0 0 0 0 0 0 0 0 0 41826584 33984676 36521568 ZERO3
mdbg f=3375 cmd=10 mode=1 sp=0 1 ZERO3
mdbg f=3375 cmd=17 mode=1 sp=0 0 0 0 0 0 0 0 0 0 0 0 0 ZERO3
```

The per-word producer trace (`NNK_GX_FIFOPC`) shows the guest really stores that
command word and copies those twelve words:

```
#3356 pc=02003040 word=0000001a        <- MTX_MULT_3x3
#3357..3365 pc=02009c28/30/38  = identity 3x3 (0x1000 on the diagonal)
#3366 pc=02003008 word=00000017        <- MTX_LOAD_4x3
#3367..3375 pc=02005cb4/bc/c4 word=00000000   <- nine ZERO basis words
#3376..3378 pc=02005ccc word=027e3918 020690a4 022d4660   <- the three "translation" words
```

`0x02003008` is `str r2,[r1]` with `r1 = 0x04000400`, `r2 = 0x17`, then
`bx 0x02005cb0`; `0x02005cb0` is four `ldmia r0!,{r2,r3,ip}` / `stmia r1,...`
pairs, i.e. the SDK's "send a 4x3 matrix from a buffer" leaf
(`work/arm9_dec.bin`, `arm-none-eabi-objdump`). So the twelve words are a
faithful copy of a source buffer that holds nine zero basis words — the same
shape of failure Phase-5BQ found for `MTX_MULT_4x3` (the SDK's basis builder
writing zeros because of a mis-emulated math op), one level further along the
same builder chain: the diagonal `0x1000`s of the identity template are missing
while the translation words survive.

## 5. What the reference renderer contains (melonDS `GP3D`, this savestate)

| quantity | melonDS | ours |
|---|---|---|
| `ProjMatrix` | `1024 0 0 0 / 0 1365 0 0 / 0 0 -128 0 / -614400 -521462 0 131072` | **bit-identical** |
| `PosMatrix` at save time | scale 4096, translation (640, 320, -1023) | proper matrix too |
| polygons in the last completed frame (bank 0) | 434 | 379 |
| polygons with a vertex at `sx==0` | 10 | 120 |
| polygons spanning `sx=0..>=256` | **0** | **39** |
| vertices at the collapse point `(proj[12],proj[13],proj[14],proj[15])` | **0** in bank 0 and bank 1 | 284 `SubmitVertex` calls per frame |
| `TempVertexBuffer` at save time | four real quad vertices, `w=131072` | — |

Since the projection is bit-identical, a collapsed vertex set would produce the
same slivers in melonDS's renderer (the clip, cull, zero-dot and viewport paths
are the same code — see §6). melonDS having **zero** vertices at the collapse
point means its guest never submitted the zero basis words: the two emulators
disagree on the *data* the SDK matrix send reads, not on what the 3D layer does
with it.

## 6. Strip-rule audit (why the card's fix cannot move these polygons)

| melonDS | ours | note |
|---|---|---|
| `SubmitVertex` strip shuffle `GPU3D.cpp:1703-1760` | `gx_fifo.c:342-383` | same swap/submit/retain order for modes 2 and 3 |
| reuse decision `1337-1372` | absent (always re-clip) | neutral: taken only when `Clipped==0` on both shared vertices **and** `NumVertices == lastpolyverts`, i.e. when stored == raw |
| `LastStripPolygon` assignment `1660-1668`, invalidation `1320,1328,1402,1410,1499,2381` | n/a | invalidation points are BEGIN_VTXS (we reset `vtx_in_poly`/`num_consecutive` at `0x40`, `gx_fifo.c:1018-1024`), cull reject, clip-empty, degenerate, RAM overflow, frame reset — all of which we also reset or reject |
| cull `1266-1320` | `gx_fifo.c:566-600` | identical normal/dot arithmetic |
| clip `1124-1256` `1330-1400` | `gx_fifo.c:390-488` | homogeneous S-H, z → y → x, far-plane bit 12 |
| zero-dot `1357-1390` | `gx_fifo.c:713-732` | identical |
| viewport/W-norm `1428-1535` | `fill_ras_vertex` | identical |

The one genuine fidelity gap found on the way is unrelated to the band: melonDS
**stores** a polygon whose `ybot > 192` (or any `W == 0`) with
`Degenerate = true` and lets the renderer skip it (`GPU3D.cpp:1505-1512`,
`GPU3D_Soft.cpp`), whereas we drop it before it enters the retained set — that
explains part of the 434 vs 379 count but paints nothing.

## 7. Gates (this tree, `wt/t_aff18b06`)

| gate | result |
|---|---|
| `cargo test --release` (nnktool) | **65 passed, 0 failed** |
| diagnostics are render-neutral | frame 3375 `top`/`bot` PPM **byte-identical** between the pre-diagnostic build and this one (**with** `NNK_GX_PSTAT`/`NNK_GX_VD`/`NNK_GX_FIFOPC` compiled in) |
| 200 M steps, ROM-only, headless | **9.84 s / 8.95 s** (baseline 9.1-9.31 s; within 10 %) |
| `tools/verify_phase2a.sh` (`SKIP_MELONDS=1`) | **1000/1000** melonDS PCs reproduced (all three comparisons, rc=0) |
| savestate round trip (`NNK_SAVESTATE_STRICT=1`) | unchanged: no struct in the state layout was touched (statics only) |

## 8. Next step (bounded, guest-side — not this card)

Find the writer of the source buffer the SDK's 4x3 matrix send copies from (the
`pc=02005cb4/bc/c4/cc` reads) and why it holds nine zero basis words where
melonDS's guest holds a real basis. The previous member of this family
(Phase-5BQ: a 32-bit-truncated `DIV_RESULT` making `Vec3` normalisation return
zero) was fixed in the runtime's math unit; this looks like the same builder
chain reached through a different math op. The decisive instrument is a
side-by-side of the GX word stream: capture the same frame from melonDS
(`MELONDS_GXLOG_DIR`, `gxrender/GX_TRACES.md`) and diff `f%05u.gx` against our
`NNK_GX_FIFOPC` words around the first `0x17`.
