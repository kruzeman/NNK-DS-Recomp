# Phase-4E — translation sweep + live SDL2 display

Follows Phase-4D (merged `6ab7c9e`).  Scope: close the "guest executes a PC
with no dispatch case" coverage class in ONE pass, push the boot, and run the
native boot through the live `gx_display` path with a real SDL2 window.

## 0. Headline

* **The display bridge is live.**  The recompiled guest's display MMIO,
  palette, VRAM-window and OAM writes are forwarded into `gx_display` with the
  guest's write width, and one frame is rendered + presented per emulated
  VBlank into a single SDL2/GL window holding **both DS screens**
  (1024x384 = 2 x 256x192 at scale 2).  112 frames were presented in the
  60 M-step run; `NNK_GX_WINDOW_PPM` captured the window back buffer
  (`p4e_window.ppm`, P6 1024x384).
* **The frames are black, and that is the guest's real state**: the guest
  writes `DISPCNT = 0` (engine A and B) at `0x02002C14`/`0x02000CE0` — the same
  write melonDS's IO trace shows at ts=6.19 M — and never enables the display
  before it aborts (see §4).  So no picture exists yet; the honest stop-report
  below names the exact barrier.
* **The coverage class is closed**: 0 untranslated PCs remain against the
  30 M-PC melonDS ground-truth trace — 1,755 static-image gap PCs plus 6,340
  runtime-overlay PCs, all translated in one sweep, not one crash per opcode.

## 1. The Phase-4D "barrier 0x0200DA34" was a reporting artefact

`halt_pc=` is the **last traced PC**, not the untranslated one:

```
nat_p4d3_hle.log:  untranslated pc=0x02139e48
                   halt state ... r14=0200DA39
                   done ... halt_pc=0x0200DA34 halt=dispatch to untranslated pc
```

`0x0200DA34` is `blx r0` and **has had a dispatch case since Phase-3A3**
(`case 0x0200DA34u:` in the image C).  The PC that had no case was the
`blx` target, `0x02139E48` — in main RAM **outside the static ARM9 image**,
because the boot had just called into a NITRO overlay module it loaded from
the card.  The image case range ends at `0x0206DA04`; `0x021xxxxx` is
runtime-loaded code.

## 2. Translation sweep (one pass, both halves of the class)

New ground truth: a **30 M-PC** melonDS PC trace plus a per-PC **ARM/Thumb
mode trace** (`tools/add_arm9mode_trace.py`, `MELONDS_ARM9MODE_PATH=...`).
The mode trace matters: a 4-aligned PC inside Thumb code is otherwise
mis-decoded as ARM (the alignment rule can only prove that a 2 (mod 4) PC is
Thumb).

* **Static image gaps** (`tools/mk_trace_seeds.py` + mode trace):
  1,755 trace PCs had no case → seeds → `73,120` → **`77,758`** cases;
  re-running the gap detector on the new image reports **0** gaps.
* **Runtime-loaded overlays** (`tools/mk_region_seeds.py`): 6,340 distinct
  executed PCs in 8 occupied 64 KiB bands —
  `0x020C0000`, `0x020D0000`, `0x020E0000`, `0x020F0000`, `0x02100000`,
  `0x02120000`, `0x02130000`, `0x02150000` — of which **3,202 ARM / 3,138
  Thumb**, recompiled from a memory dump of the loaded slots
  (`work/phase3a/ovl_p4e.bin`, 0x020C0000..0x02180000) → **48,776** cases,
  spliced into the dispatch switch with `tools/merge_itcm_cases.py`
  (generalised with optional `lo hi` region args) → 126,534 cases total.
* The whole sweep is `tools/p4e_sweep.sh <tag>`.

Result: the run no longer stops on an untranslated PC anywhere in the first
30 M melonDS PCs.

## 3. Live display bridge (`nnktool/src/recompile.rs`, `-DNNK_GX`)

Compiled in only with `NNK_GX=1` (default builds keep zero gxrender
dependency, so every pre-existing acceptance check is untouched).

| runtime piece | behaviour |
|---|---|
| `io_write32/16/8` | forward the write to `gxd_io_write()` with the guest's width, then run the runtime's own register model via the new `*_sw` variants (no double-width clobber of the display state) |
| `st32/st16/st8` | palette `0x05000000-7FF`, VRAM windows `0x06000000-0x0689FFFF`, OAM `0x07000000-7FF` route to `gxd_io_write` instead of being dropped |
| `io_read16` | `KEYINPUT` (0x04000130) from `gxd_io_read` (live SDL keypad); headless default `0x03FF` = no key pressed (was 0 = every key pressed) |
| `trace_pc()` | VBlank edge detector (`vcount()` crossing 192) → `gxd_frame()` = render 3D + composite both screens + present + pump SDL events |
| `main()` | `gxd_open()` under `NNK_GX=window\|headless`; final frame + captures on exit |

Env: `NNK_GX=window|headless`, `NNK_GX_SHOT=<base>` (per-frame PPMs),
`NNK_GX_SHOT_EVERY=n`, `NNK_GX_WINDOW_PPM=<path>` (window back buffer),
`NNK_TRACE_FROM=<step>` (late trace window).
Build: `SPLIT_DIR=... NNK_GX=1 tools/build_split.sh out/split/arm9_gx`.

Proof the bridge does not perturb the guest: the strict common prefix against
the Phase-4D capture is **unchanged at 3,643,483** PCs.

## 4. Honest stop-report: the guest aborts, so no picture exists yet

With the overlays translated the guest runs 60 M steps without an untranslated
PC, but it never reaches a display enable.  It ends in the SDK terminal loop:

```
0x020090D8  push {r3,lr}          ; call "fini" callback if set (0x0206CA14)
0x020090FC  bl   0x02009108       ; cache flush
0x02009110  bl   0x02009118       ; <- 60M steps end here
0x02009114  b    0x02009110       ;   (infinite, melonDS never executes it)
```

First entry into that path (single-shot register dump, `NNK_DEBUG_AT_PC`):

```
pc=020090D8 r0=00000000 r1=FFFFFFFF r2=00000000 r3=00000200
            r4=027E3AB0 r5=0213DD04 r6=00000200 r7=021466D8
            r13=027E3AA0 r14=0210E38C          <- return address (overlay)
```

i.e. overlay code called it directly.  The call site (dump of the loaded
overlay slot):

```
0x0210E360  push {r3,r4,r5,lr}
0x0210E36C  mov  r0,r4 ; bl 0x0200CF10
0x0210E378  mov  r0,r4 ; mov r1,r5 ; bl 0x0200D4D0
0x0210E380  cmp  r0,#0
0x0210E384  popne {r3,r4,r5,pc}     ; <- melonDS takes THIS (returns)
0x0210E388  bl   0x020090D8         ; <- native takes THIS (abort)
```

`0x0200D4D0` is a veneer to `0x0200D0E8` (`mov r2,#1; bx 0x0200D0E8`), which
formats/parses into a string and returns 0 when its first inner call
`0x0200C638` returns 0; `0x0210E388` is the "unrecoverable, stop" arm of that
SDK helper.

Ground truth: melonDS executes the **same** call site and takes the
non-abort branch — its trace continues `0x0210E384 -> 0x0210514C ...` and
**never executes `0x0210E388`** in 30 M PCs (`grep -c = 0`), while the whole
overlay PC sequence of the native run matches melonDS in order
(**14,614 / 14,615** native overlay PCs, 17 resync events, all of them the
melonDS "both halfwords of a 32-bit Thumb instruction" logging artefact).

So the remaining divergence is **not** a dispatch gap and **not** a mistranslated
overlay: the translated code walks the emulator's path and then takes the abort
arm because `0x0200D0E8` returned 0.  That is a **data / IO-model**
difference inside an SDK format step (its argument block sits in DTCM at
`0x027E3AB0`), and it happens before the game ever enables the display —
`DISPCNT` stays 0, so both compositor outputs are legitimately black.

Next task (bounded, not done here):
1. capture melonDS state at the `0x0200D4D0` call (register + memory hook) and
   diff the argument block and the `0x0200C638` return against the native run;
2. fix the modelling gap; then
3. re-run `NNK_GX=window` — the display-enable writes are the next milestone
   and the frame capture path is already wired.

## 5. Numbers (all measured)

| metric | value |
|---|---|
| static-image trace gaps closed (30 M-PC trace) | 1,755 PCs, 0 left |
| static image cases | 73,120 -> 77,758 |
| overlay PCs translated | 6,340 (3,202 ARM / 3,138 Thumb) in 8 bands |
| overlay cases spliced | 48,776 -> 126,534 image cases |
| frames presented (60 M-step run, windowed) | 112 |
| window back buffer | 1024x384, P6 (`p4e_window.ppm`) |
| gxd writes forwarded but not owned | 2,677 |
| strict prefix vs Phase-4D capture (10 M PCs) | **3,643,483** (unchanged) |
| strict prefix vs 30 M-PC capture | 3,634,037 (different capture: the divergence is the known card-poll timing offset, so the number is capture-dependent) |
| native overlay sequence vs melonDS | 14,614 / 14,615 in order |

## 6. Reproduce

```sh
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# ground truth: PC trace + ARM/Thumb mode trace (needs the ARM.cpp hook)
python3 tools/add_arm9mode_trace.py && (cd /home/hermes/tools/melonDS/build-ptrace && make -j12 melonDS)
MELONDS_ARM9TRACE_PATH=$S/melonds_p4e_pc.txt MELONDS_ARM9MODE_PATH=$S/melonds_p4e_mode.txt \
  MELONDS_ARM9TRACE_N=30000000 xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "<ROM>"

# translation sweep + GX build (writes work/phase3a/*, gitignored)
S=$S sh tools/p4e_sweep.sh p4e2

# run the boot with the live display, windowed, under a virtual X server
NNK_ROM_PATH="<ROM>" NNK_GX=window NNK_GX_SHOT=out/shots/w NNK_GX_SHOT_EVERY=20 \
  NNK_GX_WINDOW_PPM=$S/p4e_window.ppm NNK_STEP_LIMIT=60000000 \
  xvfb-run -a out/split/arm9_gx work/phase3a/arm9_stored_padded.bin

# honest comparisons
python3 tools/cmp_pc_traces.py  $S/melonds_irq_pc.txt <native-trace>
python3 tools/cmp_overlay_seq.py $S/melonds_p4e_pc.txt <native-trace> 0x020C0000 0x02180000
```

## 7. Known limitations

* Overlay content is recompiled from a **snapshot** of the loaded slots.  Slots
  are reused when the game swaps overlays, so a later swap needs a fresh dump +
  re-run of `tools/p4e_sweep.sh` (the mechanism is the same, the input dump
  changes).
* Guest **reads** of VRAM/palette/OAM still return 0 (gx_display has no read
  API); only writes are wired.
* VBlank/HBlank-timed DMA is still not triggered (`io_do_dma` returns early for
  timing != 0); immediate DMA is.
* melonDS's PC trace logs both halfwords of a 32-bit Thumb instruction; tools
  comparing PC sequences must account for that (`tools/cmp_overlay_seq.py`
  reports the resync count instead of hiding it).
