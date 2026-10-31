# Phase-5B — the object field at 0x02049BDC: the ARM7's keypad mirror at 0x02FFFFA8

Follows Phase-5A (merged `36e9f6d`).  Scope: root-cause `[0x021A7555+12]` =
`0x2C00` native vs `0` melonDS at the `0x02049BDC` `beq`, fix it with measured
semantics, guard the consequence, the picture — or the honest stop-report.
**The field was never `0x2C00`.  Phase-5A read the stale `r2` of the previous
instruction; the real difference is the object's state word `[r0+12]` = 1
(native) vs 0 (melonDS), and its cause is a single ARM7 store: the ARM7
publishes `0x2C00` to main-RAM `0x3FFFA8` (ARM9 view `0x02FFFFA8`) every
~267,000 cycles, and the ARM9's keypad edge detector ORs that word into its
KEYINPUT read.  With that word missing every "keypad" derivation is off, the
edge detector believes bits 10/11/13 are held from boot, and the state machine
stays in state 1.  Fixed: the `beq` now takes melonDS's path and the run
matches melonDS for **155** PCs past the Phase-5A dispatch point (was 62).
A second, different divergence stops the run at 0x0202A15C (§6).**

## 0. Headline

* **Phase-5A's `0x2C00` was a stale register.**  `NNK_DEBUG_AT_PC` dumps
  registers at *dispatch*, before the case body runs, so at the 0x02049BD4 hit
  `r2` still held `ldrh r2,[r0,#18]` from 0x02049B88 (`0x2C00`).  The `ldr
  r2,[r0,#12]` at 0x02049BD4 then loads **1** in native and **0** in melonDS —
  the state word of the stream object at 0x021A7554, not a `0x2C00` field.
* **Root cause (measured in both emulators): the ARM7's shared keypad/touch
  mirror at main-RAM 0x3FFFA8 is never published by the native ARM7 model.**
  * melonDS memwatch (`p5b_mel_top2.txt`): `W16 cpu=7 addr=0x027FFFA8
    val=0x00002C00 pc7=0x037FE688` — first at `ts=8421838`, then every
    ~267,000 cycles (267,088 / 266,158 / 267,983 … — 267 writes before
    ts 27.0M, **every one of them 0x00002C00**).
  * melonDS 4 MB RAM dump at ts 27.0M (`p5b_melram_a.bin`, new one-shot
    `MELONDS_RAMDUMP_PATH` hook): main-RAM offset `0x3FFFA8` = `00 2C 00 00`.
    Native store-watch over `0x02FFFF80..0x03000000` (`p5b_stwatch_top.txt`):
    **no write ever**, so the word stays 0.
  * The guest's keypad edge detector (0x02049B0C) computes
    `((KEYINPUT | [0x02FFFFA8]) ^ (0x02FFFFA8 >> 12)) & (0x02FFFFA8 >> 12)`
    = `(x | w) ^ 0x2FFF & 0x2FFF`.  KEYINPUT is `0x03FF` in **both** runs
    (melonDS io trace `p4q_mel_io.txt` ts 50,498,924 = `0x03FF`; native
    `r3=0x03FF`), so melonDS gets `(0x03FF|0x2C00)^0x2FFF = 0` and native
    gets `0x03FF^0x2FFF = 0x2C00`.
* **Consequence chain (measured, `p5b_stwatch.txt` + `p5b_obj_b98.txt`):** with
  `[r0+8] = 0x2C00` the `cmp r2,#0` at 0x02049B84 leaves Z=0, so the
  `strheq/strheq/streq` chain at 0x02049B8C..0x02049B94 is skipped, the
  `ldrh r3,[r0,#8]` at 0x02049BF4 is non-zero and 0x02049C10 sets the state
  word `[r0+12] = 1` — permanently (the counter at `[r0+16]` only reaches 3 by
  step 39M because the routine is called 4 times).  At the 4th call
  (step 39,002,885) `[r0+8] == [r0+18] == 0x2C00`, so 0x02049B9C's `beq` is
  taken and 0x02049BD4 loads state **1** → `cmp r2,#1` → `beq 0x02049C18`.
  melonDS loads **0** → `beq 0x02049BF4` → `ldrh r3,[r0,#8]` = 0 → `bxeq lr`.
* **The workaround class is a data/IO-model one, not a decoder bug**: the guest
  instruction stream is identical, a *shared-RAM word written by the other CPU*
  is missing.  So this phase ships measured semantics plus a source-level
  scenario guard (no new decoder test applies; `cargo test` stays 14/14).

## 1. The fix

`tools/nnk_smc.inc` (new `arm7_publish_keypad`, the only implementation):

```c
#define NNK_KP_MIRROR 0x02FFFFA8u
static uint64_t nnk_kp_next;
void arm7_publish_keypad(void) {
    if (nnk_kp_next == 0) nnk_kp_next = 5461000ull;   /* mel ts 8,421,838 / 1.542 */
    if (cycles < nnk_kp_next) return;
    nnk_kp_next += 173150ull;                         /* mel 267,000 / 1.542 */
    st16(NNK_KP_MIRROR, 0x2C00u);   /* bits 10,11,13 = 1 (hardware-present), pen=0 */
}
```

called once per dispatched instruction from `trace_pc`
(`nnktool/src/recompile.rs`; block-scope `extern` declaration because the
definition lives in the `nnk_smc.inc` fragment spliced in after the prelude):

```c
    cycles++;
    { extern void arm7_publish_keypad(void); arm7_publish_keypad(); }
```

SEMANTICS (measured, not invented): the ARM7 owns the keypad/touch hardware and
publishes the word the ARM9 must OR into its own KEYINPUT read; `0x2C00` = bits
10,11,13 set (the keypad bits that read 1 on hardware) and bit 12 clear (no
pen down).  The value is constant in the capture, so only its presence is
observable; the schedule only has to precede the ARM9's first read at cycle
17,589,7xx (measured first read) — the model publishes from cycle 5,461,000,
~12M cycles earlier.

The frozen Phase-3A image (which carries the Thumb dispatch, overlay unions and
ITCM cases that the seed tables do not reproduce) is patched with
`tools/p5b_arm7_keypad.py` (idempotent, refuses to run twice); regenerated
images get the call from `recompile.rs` directly.

## 2. Verification — the `beq` follows melonDS

```
build: out/split/arm9_kp  (NNK_IMAGE_C=work/phase3a/arm9_p5b_kp.c, NNK_GX=1)

4th call, NNK_DEBUG_AT_PC=0x02049BD4 step=39,002,885
  r0=021A7554 r1=00000000 r2=00000000 r3=00000000 r12=00000000 nzcv=0110 (Z=1)
  object: +8 = 0x0000, +12 = 0x00000000   (was +8 = 0x2C00, +12 = 1)

PC trace after the Phase-5A dispatch point (NNK_TRACE_FROM=38987000):
  native  ... 0x02049BD4 0x02049BD8 0x02049BDC 0x02049BF4 0x02049BF8 0x02049BFC 0x02049B6C
  melonDS ... 0x02049BD4 0x02049BD8 0x02049BDC 0x02049BF4 0x02049BF8 0x02049BFC 0x02049B6C
  (Phase-5A native went 0x02049BDC -> 0x02049BE0 -> 0x02049BE4 -> 0x02049C18)
```

Strict-prefix alignment (`p5b_align.py`, native 273,001 PCs vs melonDS's
ts-gated trace `p5b_melpc50.txt`, 3,283,066 PCs, 32-bit-Thumb halfword
tolerance): **155 PCs matched** after the dispatch point, first divergence at
native index 155 (§6).  Phase-5A matched 62.

## 3. The instrument (new, reusable)

* `tools/p5b_store_watch.py` + the runtime's per-store hook: an image patcher
  that turns every guest store into a logged `(addr, value, pc, step)` event
  when it lands in `NNK_ST_WATCH="lo:hi"` — logs **before** the write, so a
  store the memory model drops is still visible.  This is what identified the
  writer/reader relationship in the object (`p5b_stwatch.txt`, 59 writes) and
  proved that nothing ever writes 0x02FFFFA8 natively (24 writes in the
  top-of-RAM window, none at 0x3FFFA8).
* `tools/melonds_ramdump_at_ts.py`: one-shot `MELONDS_RAMDUMP_PATH` dump of
  melonDS's 4 MiB main RAM at the first ARM9 PC inside the trace window.  It
  answers "what VALUE does melonDS hold when no write-watch ever fires" — the
  question that broke the Phase-5A analysis.  (`p5b_melram_a.bin` ts 27.0M,
  `p5b_melram_b.bin` ts 50.4M.)

## 4. The picture (honest)

`NNK_STEP_LIMIT=60000000 NNK_GX_SHOT` on the fixed build: frames 60 and 70 are
still **uniform white (1 distinct value, both screens)** — identical to
melonDS's frames 0..101 (PHASE5A §4), so nothing about the display path
regressed; the run simply does not reach melonDS frame ~102 (the DISPCNT
switch at 0x0207CB10) because it halts at step **39,262,401**,
`halt_pc=0x02178E9E` (the same honest "smc: unsupported live instruction" halt
as Phase-5A), reached through the *second* divergence below.
PNGs: `/home/hermes/NiNoKuni/shots/p5b_kp.p5b_sh.000{60,70}.{top,bot}.png`
(source PPMs `p5b_sh.000{60,70}.*.ppm` in the scratch dir).

Per-pixel diff against melonDS's composed frame (`p5b_framediff.py`), frame 70:
`p5b_sh.00070.top.ppm` vs `f00070.scr` **0 differing pixels, max channel
delta 0**; `p5b_sh.00070.bot.ppm` vs `f00070.scr` **0 differing pixels** — the
display path is still pixel-exact with melonDS at this boot stage.

## 5. Numbers

```
native (fixed)   60M run: steps 39,262,401  distinct_pcs 16,331  halt pc 0x02178E9E
                 (Phase-5A:   steps 39,262,401  distinct_pcs 16,360*)
                 * the 5A figure counted its own 260,641-step tail; the fixed
                   run reaches the same halt with 155 aligned PCs instead of 62
strict prefix    155/155 native PCs == melonDS after the dispatch point
                 (Phase-5A: 62)
object state     [0x021A7560] = 0 at the 4th call (was 1); key word
                 [0x021A755C] = 0x0000 (was 0x2C00)
cargo test       14 passed (unchanged — data/IO-model class, no decoder change)
verify_phase2a   RESULT PASS, rc 0 on this branch (SKIP_MELONDS=1):
                 strict common prefix 1000/1000 melonDS PCs (100.0%),
                 window-filtered 1000/1000, in-order after resync 1000/1000
                 (log `p5b_verify.log`)
frames           f70 top+bot vs melonDS f00070.scr: 0 differing pixels
```

## 6. The next barrier (named, measured)

After the fixed `beq` the two runs stay identical for 155 PCs and then diverge
at **0x0202A15C `pop {r3, pc}`** (r13 = 0x027E3A90 at the earlier hits,
r14 = 0x02029E90):

```
native  ... 0x02029F54 0x0202A15C -> 0x020FC2A4 ...
melonDS ... 0x02029F54 0x0202A15C -> 0x02029350 ...   (ts=49081783)
```

melonDS executes **no** PC at 0x020FC2A4 in its 49M..55M-ts window, i.e. native
returns into a path melonDS never takes.  The popped word comes off the stack
([r13] at the pop), so this is a *stack-content* divergence: the same call
chain pushes different values, and melonDS's stack area (main-RAM 0x3E3Axx in
`p5b_melram_b.bin`) holds different data from native's.  That divergence — not
the object field — is what sends the native run into the 0x0214xxxx-0x0217xxxx
overlay band (25,284 trace PCs there, 0 melonDS PCs in that range even in the
55M-ts window) and then into the 0x02178E9E halt.

Follow-up should start at the callee that owns 0x02029E90/0x02029F58 (its
`bl`s return through 0x0202A15C) and diff the pushed return value at
0x027E3A9x between the two runs — with the two instruments this phase added
(`NNK_ST_WATCH` for the native store history, `MELONDS_RAMDUMP_PATH` for the
melonDS word).

## 7. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/obj-field     # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
# the image with the ARM7 keypad publish (and optionally the store watch)
python3 tools/p5b_arm7_keypad.py work/phase3a/arm9_thumb_ovl_itcm_p4j.c work/phase3a/arm9_p5b_kp.c
NNK_GX=1 NNK_IMAGE_C=work/phase3a/arm9_p5b_kp.c bash tools/build_split.sh out/split/arm9_kp
# divergence point + the branch
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=39260000 \
  NNK_GX=headless NNK_TRACE_FROM=38987000 NNK_DEBUG_AT_PC=0x02049BD4 NNK_DEBUG_N=4 \
  NNK_DEBUG_MEM="0x021A7554:32,0x02FFFFA0:16" NNK_DEBUG_MEM_PATH=$S/p5b_kp_bd4.txt \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_kp work/phase3a/arm9_stored_padded.bin
# melonDS ground truth: the ARM7 writer, then the RAM word
MELONDS_MEMWATCH_PATH=$S/p5b_mel_top2.txt MELONDS_MEMWATCH_N=4000 \
  MELONDS_MEMWATCH_LO=0x027FFFA0 MELONDS_MEMWATCH_HI=0x02FFFFB0 \
  MELONDS_ARM9TRACE_PATH=$S/p5b_melpc_c.txt MELONDS_ARM9TRACE_TSLO=26990000 \
  MELONDS_ARM9TRACE_TSHI=27000000 MELONDS_ARM9TRACE_EXIT=1 \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
python3 tools/melonds_ramdump_at_ts.py && (cd /home/hermes/tools/melonDS/build-ptrace && make -j12 melonDS)
MELONDS_ARM9TRACE_PATH=$S/p5b_melpc_a.txt MELONDS_ARM9TRACE_TSLO=27000000 \
  MELONDS_ARM9TRACE_TSHI=27020000 MELONDS_ARM9TRACE_EXIT=1 \
  MELONDS_RAMDUMP_PATH=$S/p5b_melram_a.bin \
  xvfb-run -a /home/hermes/tools/melonDS/build-ptrace/melonDS "Ni No Kuni.nds"
(cd nnktool && cargo test)                       # 14 tests
SKIP_MELONDS=1 bash tools/verify_phase2a.sh      # this branch
```

Evidence (`$S`): `p5b_mel_top2.txt` (the ARM7's 267 publishes), `p5b_melram_a/b.bin`,
`p5b_stwatch.txt` / `p5b_stwatch_top.txt` (native store history, object and
top-of-RAM), `p5b_obj_b98.txt`, `p5b_b4c.txt` (object + KEYINPUT at the first
call), `p5b_kp_trace.out` (273,001 PCs), `p5b_melpc50.txt` (melonDS ts-gated
trace), `p5b_align.py`, `p5b_pop*.txt`, `p5b_run60.*`, `p5b_sh.*`, `p5b_verify.log`,
`p4q_mel_io.txt` (melonDS KEYINPUT = 0x03FF).

Changed in this phase: `tools/nnk_smc.inc` (arm7_publish_keypad),
`nnktool/src/recompile.rs` (the per-dispatch call), `tools/p5b_arm7_keypad.py`,
`tools/p5b_store_watch.py`, `tools/melonds_ramdump_at_ts.py`, `PHASE5B.md`.
