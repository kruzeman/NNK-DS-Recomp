# Phase-5BO — the bottom screen's missing picture: measured, attributed, and
# refuted premise

**Question (card t_4d6b640b).** On the operator's savestate the bottom screen
draws its dialogue text but no picture: it is black where melonDS shows the
street scene. The card's hypothesis was that engine B's BG0/BG1 render as a
black field — a wrong VRAM bank, a wrong extended-palette bank, or a BG that
is enabled but draws nothing.

**Answer.** No 2D register, bank, palette or tile is wrong. At the operator's
moment the whole 2D stack of *both* engines is **byte-identical to melonDS's
savestate** (registers, palette, all four VRAM flat views, OAM region content),
and every enabled BG writes pixels. The missing picture is the **3D layer**,
which renders nothing: 424,853 polygons submitted, **0 drawn** (Phase-5BP).
The bottom screen is engine A (`screen1`) and its scene comes from engine A's
3D layer, not from a BG.

Two premises of the card are also refuted, with measurements below: engine B
(the *top* screen) is not black and its BG0/BG1 do not render a black field —
they draw the Morton Street map correctly; and the "full-screen plasma pattern"
on our top screen is that same map (the crossing diagonal roads), not garbage.

## 1. Ground truth: the melonDS state for the same moment

`work/repro_5bo/melonds_oliver.mln` (MELN v13). `tools/melds_2dregs.py` +
`tools/melds_gpu.py` + the new `tools/melds_bg.py` (which uses melonDS's own
`VRAMMap_*` block maps from the state, not a re-derivation):

```
GP2A DISPCNT=0x00211718 BGCNT=0x0003 0x0700 0x2600 0x0888
GP2B DISPCNT=0x40011330 BGCNT=0x0686 0x0701 0x0089 0x1880
VRAMCNT 83 8b 84 93 83 81 82 82 82
ABG      0x20 . 0x20 ...          (bank F, blocks 0 and 2 -> 0x06000000)
AOBJ     0x40 . 0x40 ...          (bank G, blocks 0 and 2 -> 0x06400000)
BBG      0x04 x8                  (bank C  -> 0x06200000)
BOBJ     0x100 x8                 (bank I  -> 0x06600000)
BBGEXT   0x80 x4                  (bank H  -> BBG ext-palette window)
TEX      0x01 0x02 0x08 0x00      (A, B, D)
TEXPAL   0x10 x4                  (E)
```

Per-layer decode of the reference (`tools/melds_bg.py`, palette + tile fetch
exactly as the renderer does it):

| engine | layer | BGxCNT  | mode | char  | screen | depth | coverage |
|--------|-------|---------|------|-------|--------|-------|----------|
| A (screen1/bottom) | BG0 | 0x0003 | text 16c | 0x00000 | 0x00000 | 16 | 8.3% |
| A | BG1 | 0x0700 | text 16c | 0x00000 | 0x03800 | 16 | 0.5% |
| A | BG2 | 0x2600 | text 16c extpal | 0x00000 | 0x03000 | 16 | 37.4% (dialogue box) |
| B (screen0/top)    | BG0 | 0x0686 | text 256c | 0x04000 | 0x03000 | 256 | 100% (the map) |
| B | BG1 | 0x0701 | text 16c | 0x00000 | 0x03800 | 16 | 100% |

So the reference bottom screen's street scene and portrait have **no BG
source** (engine A BG0+BG1+BG2 only cover 8.3 + 0.5 + 37.4 % and the 37.4% is
the dialogue box), and engine A's OAM has only nine on-screen sprites — the
continue arrow, the speech tail and three 32x16 pieces at y=106, i.e. dialogue
UI, none of which covers the scene or the portrait. The picture on the bottom
screen can only be the engine A 3D layer.

## 2. Our side at frame 3350, from the operator's save

`NNK_GX_STATE_DUMP=/tmp/p5bo/state3350.bin NNK_GX_STATE_DUMP_FRAME=3350`
(`state_dump()` writes the exact GX2D state image `gx2d_state_load()` is about
to consume). Diffed with the new `tools/nnk_2dcheck.py` against the melonDS
extraction:

```
engine 0 DISPCNT=0x00211718 BGCNT=0x0003 0x0700 0x2600 0x0888  -> MATCH
engine 1 DISPCNT=0x40011330 BGCNT=0x0686 0x0701 0x0089 0x1880  -> MATCH
palette   EQUAL (0/2048)          ABG      EQUAL (0/524288)
BBG       EQUAL (0/131072)        BBGEXT   EQUAL (0/32768)
AOBJ      EQUAL (0/262144)        banks A..D EQUAL (0/131072 each)
```

The only memory differences are in banks E..I *outside* the views the 2D
renderer consumes (texture palette, and the raw-bank copy of this dump, whose
per-bank stride does not match the dump's flat offset — a dump-format detail,
not a renderer input: every flat view the renderer reads is equal).

And the per-layer coverage at that frame (new diagnostic,
`NNK_GX_2DINFO=3350`, which counts pixels each layer actually writes):

```
2dinfo frame=3350 engine=A -> screen1(bottom) DISPCNT=0x00211718 mode=1 bg_en=0x17 obj_en=1 win0=0 win1=0 objwin=0 extpal_bit30=0 bgwin_abs=0x06000000 bgwin_banks=F
2dinfo   BG0 en=1 text prio=3 char=0x00000 screen=0x00000 depth=16 size=256x256 extpal_bit13=0 extpal_slot=-1 -> char@0x06000000 map@0x06000000 scroll=0,0 map[0..3]=0000 0000 0000 0000 px=0
2dinfo   BG1 en=1 text prio=0 char=0x00000 screen=0x03800 depth=16 size=256x256 extpal_bit13=0 extpal_slot=-1 -> char@0x06000000 map@0x06003800 scroll=0,0 map[0..3]=0000 0000 0000 0000 px=231
2dinfo   BG2 en=1 text prio=0 char=0x00000 screen=0x03000 depth=16 size=256x256 extpal_bit13=1 extpal_slot=-1 -> char@0x06000000 map@0x06003000 scroll=0,0 map[0..3]=1000 1000 1000 1000 px=18400
2dinfo   BG3 en=0 affine prio=0 char=0x08000 screen=0x04000 depth=256 ... px=0
2dinfo   OBJ px=1007  3D-layer px=0
2dinfo frame=3350 engine=B -> screen0(top) DISPCNT=0x40011330 mode=1 bg_en=0x13 obj_en=1 win0=0 win1=0 objwin=0 extpal_bit30=1 bgwin_abs=0x06200000 bgwin_banks=C
2dinfo   BG0 en=1 text prio=2 char=0x04000 screen=0x03000 depth=256 size=256x256 extpal_bit13=0 extpal_slot=0 -> char@0x06204000 map@0x06203000 scroll=0,0 map[0..3]=0000 0001 0002 0003 px=49152
2dinfo   BG1 en=1 text prio=1 char=0x00000 screen=0x03800 depth=16 size=256x256 extpal_bit13=0 extpal_slot=1 -> char@0x06200000 map@0x06203800 scroll=0,0 map[0..3]=0000 0001 0002 0003 px=49152
2dinfo   BG2 en=0 text prio=1 char=0x08000 screen=0x00000 depth=256 ... px=0
2dinfo   OBJ px=2972  3D-layer px=0
```

(`bgwin_banks` is decoded from the VRAMCNT copy in the state: engine A BG =
bank F with MST 1; engine B BG = bank C with MST 4. That is exactly melonDS's
own `VRAMMap_ABG`/`VRAMMap_BBG` from the reference state.)

Interpretation, answering card item 5 for every layer: nothing here is
"skipped" and nothing is "drawn with the wrong bank" — every enabled BG writes
pixels (231 / 18400 / 49152 / 49152) and their content matches melonDS's
byte-for-byte inputs. The only layer that writes **0 px** is the 3D layer, on
the engine A screen that the operator sees as black.

## 3. The bottom screen is engine A, not engine B

`DISPCNT B=0x40011330` belongs to engine B whose BBG bank is C — that engine
is the **top** screen in this title (POWCNT1 swap off: our run's `vrc:` line
prints `POWCNT=0x020F`, bit 15 clear; `screen = swap ? n : 1 - n` routes
engine B to screen 0 = top). Its BG0 is the Morton Street map, drawing
49152/49152 pixels from bank C at char 0x04000 / screen 0x03000 — exactly the
reference's values. The bottom screen is engine A (bank F window at
`0x06000000`, char 0x00000, screen 0x03000 for BG2), showing the dialogue box.

## 4. The "plasma" on our top screen is the map

`work/repro_5bo/ours_f3350.top.ppm` (mean luma 154, 100 % lit) and the capture
from this run are **byte-identical**, and the image is the Morton Street map:
the two pale diagonal roads crossing in an X are what reads as a "diagonal /
plasma-looking pattern" at a glance. There is no top-screen defect. Likewise
the operator's window capture `ours_071815.png` shows the same correct map on
the left panel.

## 5. Root cause and cross-reference

* Bottom screen (engine A, screen 1): the picture is the **3D layer**; our 3D
  rasteriser draws 0 of 424,853 submitted polygons and the 3D colour buffer is
  uniformly black (measured by the operator with the `NNK_GX_FB3D` dump added
  in `ca1bc54`). That is **Phase-5BP**, a separate card — not a 2D bank bug.
* The 2D picture on the bottom screen (dialogue box, nameplate, text, continue
  arrow) is correct; the 2D inputs are identical to melonDS.
* Secondary observation, not the missing picture: of the 128 OAM entries in
  the reference, only nine are on-screen (`tools/oam_dump.py`: the continue
  arrow at 235,176, the speech tail at 124..140,109 and three 32x16 UI pieces
  at y=106) and 119 are parked at `y=192` (off-screen). Our OAM keeps zeros in
  those 119 slots where melonDS holds `y=192,x=0,a2=0x00C0`; the same slots are
  invisible in the reference and AOBJ tile 0 is empty (`00` bytes), so nothing
  visible changes at this frame. Worth its own look if sprite lists (town NPCs)
  ever go missing — it is a state-shadow difference, not this defect.

## 6. New diagnostics (what the next person needs)

* `NNK_GX_2DINFO=<frame>[,<frame>...]` — per engine and per BG: enable bit,
  text/affine/bitmap, priority, char/screen base **as the renderer computes
  them**, absolute LCDC address, colour depth, size, BGxCNT bit-13 ext-palette
  slot, the VRAM banks backing that engine's BG window decoded from VRAMCNT,
  scroll registers, the first tilemap words, and the pixels the layer wrote in
  that frame. Output goes to stderr, one `2dinfo` group per engine, with the
  screen routing (`-> screenN(top|bottom)`).
* `tools/melds_bg.py <mel_ref_dir> <outdir> <state.mln>` — per-layer PPMs and
  coverage for a melonDS state, using melonDS's own `VRAMMap_*` block maps.
* `tools/nnk_2dcheck.py <nnk_dump> <mel_ref> [outdir]` — register/memory diff
  of our `NNK_GX_STATE_DUMP` against the melonDS extraction, plus our per-layer
  render.
* `tools/oam_cmp.py`, `tools/oam_dump.py`, `tools/ppm_stats.py` — sprite-list
  diff/decode and PPM luma/lit statistics.

## 7. Gates run on this build

| gate | result |
|------|--------|
| `cargo test --release` (nnktool) | 65 passed, 0 failed |
| `tools/verify_phase2a.sh` | 1000/1000 melonDS PCs (100.0%) |
| savestate round trip 5M->6M, `NNK_SAVESTATE_STRICT=1` | `fnv=0x39951F98 crc=0xC6A28280` both runs |
| savestate round trip 30M->32M, `NNK_SAVESTATE_STRICT=1` | `fnv=0xC64564A1 crc=0x147E9C9C` both runs |
| 200M steps, ROM-only, headless | 9.31 s (baseline 9.1 s, +2.3 %, within 10 %) |
| rendering unchanged | frame-3350 captures byte-identical to the pre-change build |
