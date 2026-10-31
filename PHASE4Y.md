# Phase-4Y — Thumb `blx Rm` link fixed; main-RAM mirroring verified in place;
# stop-report: the module call at 39,001,655 is a thread-wake divergence (not
# the download itself)

Follows Phase-4X (merged `8a73a6a`).  Scope: 4 MiB main-RAM mirroring,
module-download completion through 0x02988AA0, the picture — or the honest
stop-report.  **One real decoder bug found and fixed (with live evidence), the
mirroring class closed as already-implemented; the guest still calls the
not-yet-downloaded module entry — new first-divergence pinned with the
measured melonDS post-handshake protocol; no picture (honest).**

## 0. Headline

* **Closed: main-RAM 4 MiB mirroring was ALREADY implemented and is
  byte-level correct.**  `win_off()` (nnktool/src/recompile.rs:2062) maps the
  whole `0x02000000..0x02FFFFFF` block to `gmem[addr & 0x3FFFFF]` for reads
  AND writes (same function feeds ld8/16/32, st8/16/32), and the scripted
  ARM7 writers use the same alias (`main_ram_ptr`).  Live byte-level evidence:
  the guest at pc `0x02988AA0` executes the bytes of flat `0x188AA0`
  (= `0x02988AA0 & 0x3FFFFF`), which the halt dump shows holding the game's
  file-name table (`"bin\0stone06.bin\0stone07.bin..."`); the Phase-4X
  "0x02980000..0x02990000 is ZERO" reading was a FLAT-dump probe artifact
  (flat `0x988AA0` is not the mirror-visible cell).  Phase-2A's 1000/1000
  prefix already covers mirror reads (e.g. `0x02FFFE00`).
* **Fixed: Thumb `blx Rm` linked the wrong return address.**
  nnktool/src/recompile.rs:511 emitted `r[14] = addr+4|1` for `Kind::Bx{link}`
  in BOTH states; the ARM ARM (B1.3.3 T1) says Thumb BLX Rm is a 2-byte
  instruction -> `addr+2|1` (ARM BLX Rm stays `addr+4|1`).  Measured live
  effect (p4y_tr.out + p4x_halt2.mem): the overlay dispatcher's vtable call
  `blx r3` at `0x020D1A1C` returned to `0x020D1A20` — exactly the buggy
  `addr+4|1` — skipping the epilogue `pop {r4,r5,r6,pc}` at `0x020D1A1E`,
  so every Thumb vtable call fell through into the next function with an
  unpopped frame.  Fixed authoritatively in nnktool, and patched the 16 Thumb
  blx-reg sites in the shipped image (`work/phase3a/arm9_thumb_ovl_itcm_p4j.c`,
  gitignored; backup `$S/p4y_img_backup.c`; same "image predates the fix"
  practice as Phase-4L — a future regeneration picks it up from nnktool).
  The on-demand decoder (tools/nnk_smc.inc:641) already had `+2` correct.
* **Result of the fix (60M run, p4y_fix2): the module call still happens, one
  instruction earlier.**  Halt moves from step 39,001,806 (pc 0x02988CF6,
  executing file-table garbage) to step **39,001,656, halt_pc=0x02988AA0** —
  the guest still branches to the module entry at step 39,001,655 via the
  dispatcher's DESIGN path (r4=3 message -> vtable tail `0x020D1A32..1A3E`
  -> `[obj][12] = 0x02988AA0`), so the blx link was a real bug but NOT the
  barrier.  distinct_pcs stays 15,742; the card stream is still frozen
  (`starts=9093 last_op=0xB8`).

## 1. melonDS ground truth: what actually follows the 5x 0x0000006B

From `p4q_mel_io.txt` (ARM9 IO capture), after the last handshake reply
(ts 50,498,023):

1. The ARM9 returns to the IPC mailbox loop (`pc 0x0200BB0C/BB2C/BB44`) and
   keeps consuming the p4r periodic words — `0x00008007` (ts 50,659,819),
   `0x00008087` (50,852,303), `0xC0240006` (50,980,054), ... — the same
   streams the runtime already produces.  No further ARM9->ARM7 send happens
   in the whole capture (the handshake is exactly the 6 sends / 5 replies).
2. Display bring-up #2 at ts ~51.30M: VRAMCNT A..I (`0x04000240..249` =
   8B/83/82/81/84/83/8B/03/81), engine B `DISPCNT=0x80211013`, engine A
   `DISPCNT=0x0021111B`, BG/window/BLDCNT/power regs (pcs 0x02005210..0x02041B40).
3. **The module download — the "rest of the exchange":** the card driver
   resumes at ts 51,788,894 with B7 cmd `B7 0D A4 46` (ROM `0x0DA44600`) and
   streams **ROM 0x0DA44600..0x0DA5FE00 = 0x1B7 blocks x 512 B = 111,616 B**
   (an `NPCK` container — magic visible in the ROM) at ~10,960 ts/block until
   ts 54,259,575, interleaved with the usual B8 polls; then switches to the
   script region (`B7 02 D0 5E`, ts 54,285,035).
4. The overlay-resident display switch `DISPCNT=0x0021021B` at pc
   `0x0207CB10`, ts 54,276,936 (as PHASE4X reported).
   Full B7 command list: `$S/p4y_allb7.txt` (2,279 commands, whole capture).

So at the point native calls the module entry (step 39,001,655 ~ melonDS ts
~50.5M), **melonDS has not downloaded it either** — its ARM9 is in the
mailbox wait and the download starts 1.3M ts later.  Native's call is a
control-flow divergence, not a data-availability one.

## 2. The new stop-report (first differing pc + class)

* **First differing pc:** native `0x02988AA0` at step 39,001,655 (module
  entry; mirror RAM `0x188AA0` holds the file-name table in BOTH runs at this
  point).  melonDS at the analogous point is in the IPC mailbox wait loop
  (`pc 0x0200BB0C` family) and does not call any `0x02988xxx` address anywhere
  in the capture (0 IO accesses with pc=0x02988*).
* **Class:** OS thread wake-up ordering at the IPC-mailbox boundary.  The
  overlay module-manager thread processes the module-call message immediately
  after the class-0x0B handshake completes; in melonDS that thread is still
  blocked (queue/FS-completion wait) until its card download finishes at
  ts ~54.26M.  The gate probed at `0x020D22D8` (`s16 obj[+10] < 0 -> "not
  ready"`, field is -1 at halt) does NOT gate the call — the dispatcher's
  designed path ignores it.
* **Named follow-up barrier:** capture a melonDS **ARM9 pc-trace** around
  ts 50.48M..54.3M (build: `/home/hermes/tools/melonDS/build-ptrace/melonDS`
  with `tools/melonds-arm9-ptrace.patch`, env knobs `MELONDS_*`) to see which
  wait blocks the module-manager thread and which native-modelled state lets
  it run early.  Candidates: (a) the FS file-read completion event behind the
  class-0x0B handshake (the pre-handshake B7 stream of ROM 0x02CExxx is the
  script text; the driver's file-read is still in flight when native's thread
  runs), (b) the OS message-queue sequence wake order after the 5th 0x6B
  reply.  Also: 362 blx-reg sites in the shipped image were left as-is by the
  image patch (ARM-state, or Thumb sites whose source bytes are outside
  arm9.bin/ovl_p4f4.bin — e.g. ITCM); the nnktool fix covers them on the next
  image regeneration.

## 3. Honest negatives

* Frames remain uniform (`DISPCNT A=0x0021111B`, all VRAM stores in the LCDC
  bank, store window ends step 14,063,416) — **no picture**, none claimed.
* `starts=9093 last_op=0xB8` — the card stream never resumes; the guest halts
  before its driver thread ever runs again.

## 4. Verification (branch build `out/split/arm9_gx`)

```
60M runs p4y_fix1 (pre-image-patch) / p4y_fix2 (post):
    halt step 39,001,806 pc 0x02988CF6 -> halt step 39,001,656 pc 0x02988AA0,
    distinct_pcs 15,742, card starts=9093, no picture (honest).
verify_phase2a.sh (SKIP_MELONDS=1): RESULT PASS,
    strict common prefix 1000/1000 melonDS PCs (100.0%), exit 0.
```

## 5. Reproduce

```sh
cd /home/hermes/NiNoKuni/worktrees/ram-mirror   # work -> ../work symlink
S=/home/hermes/.hermes/profiles/coder/cache/scratch
python3 $S/p4y_imgfix.py    # the 16-site image patch (already applied)
NNK_GX=1 NNK_IMAGE_C=/home/hermes/NiNoKuni/work/phase3a/arm9_thumb_ovl_itcm_p4j.c \
  bash tools/build_split.sh out/split/arm9_gx
NNK_ROM_PATH="/home/hermes/NiNoKuni/Ni No Kuni.nds" NNK_STEP_LIMIT=60000000 \
  NNK_GX=headless \
  NNK_SMC_IMAGE="0x02000000:work/ghidra_src/arm9.bin,0x020C0000:work/phase3a/ovl_p4f4.bin" \
  out/split/arm9_gx work/phase3a/arm9_stored_padded.bin
SKIP_MELONDS=1 bash tools/verify_phase2a.sh      # RESULT PASS, 1000/1000
```

Evidence: `$S/p4y_{tr,fix1,fix2,verify}.{out,err,log}`, `$S/p4y_allb7.txt`
(melonDS B7 protocol), `$S/p4y_{melcard,memlook}*.py` probes,
`$S/p4x_halt2.mem` (halt dump used for the mirror/gate/object reads).
