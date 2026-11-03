#!/usr/bin/env bash
# Regenerate (or reuse) the nnktool dispatch image C, split it into one TU per
# 32 KiB PC band, compile those TUs in a bounded parallel pool with a
# content-hash object cache, and link the runner.
#
# usage: tools/build_split.sh [output-binary]
#
# INVOCATION (Phase-4L): this script is bash, not POSIX sh.  Run it directly
# (`tools/build_split.sh out/split/arm9_gx`, respecting the shebang) or as
# `bash tools/build_split.sh ...`.  It also re-execs itself under bash when it
# is started by a POSIX shell (`sh tools/build_split.sh`), because `set -o
# pipefail` and `[[ ]]` are bash-only and dash dies on them with
# "set: Illegal option -o pipefail".
#
# WHY: nnktool recompile-all emits ONE C program whose run() carries a single
# flat switch (pc) with one case per translated instruction — 73,120 cases /
# 23.6 MB / 634,616 lines for the current Phase-3A Thumb image.  cc needs
# minutes and multiple GiB of RAM to build that as one TU, which has killed
# worker runs.  tools/split_dispatch.py cuts it into nnk_main.c (runtime
# prelude + band table) plus nnk_part_NNN.c per PC band; each part is a small
# TU that compiles in seconds and can be cached independently.
#
# This is the Ni no Kuni port of DQ8's tools/build_split.sh (source of truth:
# /home/hermes/DQ8/tools/{split_dispatch.py,build_split.sh}).  Design points
# preserved from DQ8:
#   (1) parts are keyed by PC ADDRESS band, so adding a recomp root that
#       introduces a new translated PC rebuilds ONE part + the band table,
#       never every part;
#   (2) nnk_rt.h always declares ALL NNK_BAND_COUNT possible band functions,
#       so the header's bytes (part of every .o's cache key) do not change
#       when a new band appears;
#   (3) per-object stamp = hash(source) + hash(shared header) + exact cc
#       command line + cc --version, stored in sidecar .stamp files;
#   (4) separate flag sets: guest parts -O0 (stable objects, fast cc), the
#       native runtime TU -O2;
#   (5) a bounded parallel compile pool.
#
# UNCHANGED TUs ARE NOT RECOMPILED.  A no-change rebuild re-stats every source,
# finds every stamp still matching, and only relinks.
#
# KNOBS (all optional):
#   NNK_IMAGE_C=path        generated dispatch image C to split
#                           (default: <main-checkout>/work/phase3a/arm9_thumb.c;
#                           the generated image lives in the MAIN checkout,
#                           work/ here is gitignored build state)
#   SPLIT_DIR=path          split output + object dir (default work/split/c)
#   BUILD_JOBS=N            parallel pool size (default 6)
#   NNK_CC=cc               compiler (default cc)
#   NNK_SPLIT_CFLAGS="..."  guest part flags   (default "-std=c11 -O0 -g0 -w")
#   NNK_SPLIT_RUNTIME_CFLAGS="..."
#                           runtime TU flags   (default "-std=c11 -O2 -g0 -w")
#   NNK_CFLAGS_EXTRA="..."  appended to BOTH flag sets, so a change there
#                           invalidates everything (e.g. -DTRACE_LIMIT=...)
#   NNK_SPLIT_SKIP_REGEN=1  skip split_dispatch.py and rebuild only the C
#                           from the existing $SPLIT/*.c — iterate on flags
#   NNK_MONO=1              build the ORIGINAL single-TU image instead of the
#                           split (same flags, no cache) — the mono baseline
#                           used by the mono-vs-split equivalence check
#
# Every flag set is hashed into the per-TU cache key (the exact cc command
# line is part of it), so flipping guest flags recompiles only the parts and
# flipping runtime flags recompiles only nnk_main.c.
# ---- re-exec under bash when started by a POSIX shell ---------------------
# `sh tools/build_split.sh` (dash) dies on `set -o pipefail` below; bash-only
# constructs are used throughout, so hand off instead of half-running.
if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# ---- locate the main checkout (the worktree's git common dir) -------------
GIT_COMMON=$(cd "$(dirname "$(git rev-parse --git-common-dir)")" && pwd)

IMAGE=${NNK_IMAGE_C:-$GIT_COMMON/work/phase3a/arm9_thumb.c}
SPLIT=${SPLIT_DIR:-work/split/c}
OUT=${1:-work/split/arm9_split}
JOBS=${BUILD_JOBS:-6}
CC=${NNK_CC:-cc}
# Guest dispatch parts (nnk_part_NNN.c) vs native runtime TU (nnk_main.c).
GFLAGS=${NNK_SPLIT_CFLAGS:--std=c11 -O0 -g0 -w}
RFLAGS=${NNK_SPLIT_RUNTIME_CFLAGS:--std=c11 -O2 -g0 -w}
EXTRA=${NNK_CFLAGS_EXTRA:-}
# NNK_GX=1 builds the runtime with the live gx_display bridge
# (-DNNK_GX): the runtime TU then forwards 2D/display/VRAM/OAM MMIO and
# keypad reads into gxrender and presents one frame per emulated VBlank.
# The guest parts stay untouched, so only nnk_main.o rebuilds when this flips.
GX=${NNK_GX:-0}
GXDIR=$(pwd)/gxrender
GX_LDFLAGS=()

# shellcheck disable=SC2206
gflags=($GFLAGS $EXTRA)
# shellcheck disable=SC2206
rflags=($RFLAGS $EXTRA)
ldflags=(-lm)

# ---- Phase-5AB: a frozen dispatch image must not drop a committed prelude fix
# `NNK_IMAGE_C` names a *build artifact* (`nnktool recompile-all` output under
# the gitignored work/ tree), while the canonical guest-accessor prelude lives
# in nnktool/src/recompile.rs.  An image frozen before a prelude fix therefore
# builds a runtime WITHOUT that fix, silently.  Measured (Phase-5AB): the frozen
# work/phase3a/arm9_thumb_ovl_itcm_p5u.c predates the Phase-5X VRAM/palette/OAM
# read shadow, and the 13b3c45 binary built from it halts at 0x020E1B32 with
# r4=0xE3A03102 — `blx r4` after reading the vptr stored at 0x06006004 back as
# 0 — while the SAME guest parts (md5-identical nnk_part_*.c) built with the
# shadow run past it.  Apply the transform the canonical source already carries
# to any image that lacks it; the spliced image is a build artifact next to the
# split output, so the content-hash cache key stays stable across rebuilds.
#
# Phase-5AG: every splice registers the manifest features it installs, and
# tools/prelude_audit.py below proves they (and every other required prelude
# feature) are present in the runtime TU that cc is about to compile.  A
# missing feature FAILS the build instead of shipping silently.  For the
# regression proof, NNK_PRELUDE_DISABLE_SPLICE=<feature-id>[,<id>] skips the
# matching splice on purpose so the audit can be seen failing.
SPLICED_FEATURES=()
NO_SPLICE=${NNK_PRELUDE_DISABLE_SPLICE:-}
splice_disabled() {
  case ",$NO_SPLICE," in *",$1,"*) return 0 ;; esac
  return 1
}
if [[ -f "$IMAGE" ]] && ! grep -q 'gxd_sh_rd' "$IMAGE"; then
  if splice_disabled 5x_vram_read_shadow; then
    echo "== prelude splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5X read shadow (negative control) =="
  else
    SPLICED_IMAGE="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).prelude.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE")"
    echo "== prelude splice: $IMAGE predates the Phase-5X read shadow -> $SPLICED_IMAGE =="
    python3 tools/p5x_runtime_splice.py "$IMAGE" "$SPLICED_IMAGE"
    IMAGE=$SPLICED_IMAGE
    SPLICED_FEATURES+=(5x_vram_read_shadow 5x_vram_write_shadow)
  fi
fi
# Phase-5AE: the frozen image predates the per-step speed-ups in the prelude
# (cached vcount()).  Same policy as the Phase-5X splice above: the canonical
# change is in nnktool/src/recompile.rs, and the frozen artifact gets the
# identical transform so the guest parts stay byte-identical to previous phases.
if [[ -f "$IMAGE" ]] && ! grep -q 'nnk_probe_mask' "$IMAGE"; then
  if splice_disabled 5ae_vcount_cache; then
    echo "== prelude splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AE vcount cache (negative control) =="
  else
    SPLICED_IMAGE2="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5ae.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE2")"
    echo "== prelude splice (Phase-5AE): cached vcount() -> $SPLICED_IMAGE2 =="
    python3 tools/p5ae_runtime_splice.py "$IMAGE" "$SPLICED_IMAGE2"
    IMAGE=$SPLICED_IMAGE2
    SPLICED_FEATURES+=(5ae_vcount_cache 5ae_gxd_tick_skip 5ae_probe_gate)
  fi
fi
# Phase-5AF: the frozen image carries no overlay knowledge at all (it predates
# the Phase-4G registry), so the table-driven overlay map -- the ROM's 21-entry
# OVT with base/size/bss/ctor, the load-event registry, the arena dumps and the
# untranslated-pc classification -- is spliced in from the canonical block in
# nnktool/src/recompile.rs.  Same policy: no-op when it is already there.
if [[ -f "$IMAGE" ]] && ! grep -q 'io_ovl_pc_report' "$IMAGE"; then
  if splice_disabled 5af_overlay_registry; then
    echo "== prelude splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AF overlay registry (negative control) =="
  else
    SPLICED_IMAGE3="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5af.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE3")"
    echo "== prelude splice (Phase-5AF): table-driven overlay registry -> $SPLICED_IMAGE3 =="
    python3 tools/p5af_ovl_table_splice.py "$IMAGE" "$SPLICED_IMAGE3"
    IMAGE=$SPLICED_IMAGE3
    SPLICED_FEATURES+=(5af_overlay_registry 5af_overlay_pc_report 5af_overlay_load_note
                       5af_overlay_arena_dump 5af_overlay_arena_dump_exit
                       5af_overlay_table_dump)
  fi
fi
# Phase-5AJ: the card image is the only input.  The frozen image loads its
# guest image from argv[1] with fopen/fread and only fills the card model from
# NNK_ROM_PATH, so a clean directory holding ONLY the runner and the ROM cannot
# boot ("cannot open guest image arm9_stored_padded.bin").  The canonical
# prelude derives every image from argv[1] via tools/nnk_rom.inc; splice that
# into the frozen one.  Runs LAST: it wraps io_load_rom's body, whose tail the
# 5AF splice above has already rewritten with the io_ovl_init() call.
if [[ -f "$IMAGE" ]] && ! grep -q 'nnk_rom_image_load' "$IMAGE"; then
  if splice_disabled 5aj_rom_only_input; then
    echo "== prelude splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AJ ROM-only input (negative control) =="
  else
    SPLICED_IMAGE4="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5aj.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE4")"
    echo "== prelude splice (Phase-5AJ): ROM-only input (argv .nds) -> $SPLICED_IMAGE4 =="
    python3 tools/p5aj_rom_splice.py "$IMAGE" "$SPLICED_IMAGE4"
    IMAGE=$SPLICED_IMAGE4
    SPLICED_FEATURES+=(5aj_rom_only_input 5aj_rom_derived_image 5aj_rom_smc_segments)
  fi
fi

# Phase-5AP: the frozen image masks bit0 in the interworking note of a
# single-register ARM `ldmfd sp!,{pc}`.  For A_POP the loaded word IS the
# hardware's target (a Thumb `blx` leaves LR = ret|1), so the mask made ov17's
# Thumb callback body re-enter in ARM state, execute `stmiapl sl!` over its own
# bytes and destroy sp (halt step 560,192,510).  Same policy as the splices
# above: the canonical fix is in nnktool/src/recompile.rs, the frozen artifact
# gets the identical transform; skip when the marker is already there.
if [[ -f "$IMAGE" ]] && ! grep -q 'Phase-5AP pop-note splice applied' "$IMAGE"; then
  if splice_disabled 5ap_pop_note_interwork; then
    echo "== prelude splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AP pop-note (negative control) =="
  else
    SPLICED_IMAGE5="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5ap.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE5")"
    echo "== splice (Phase-5AP): ARM pop {{pc}} keeps bit0 -> $SPLICED_IMAGE5 =="
    python3 tools/p5ap_pop_note_splice.py "$IMAGE" "$SPLICED_IMAGE5"
    IMAGE=$SPLICED_IMAGE5
    SPLICED_FEATURES+=(5ap_pop_note_interwork)
  fi
fi

# Phase-5AT: a frozen image carries the MODE TABLE of the phase that emitted
# it.  One row of that table is wrong (`0x02062C44` is Thumb -- ov17's Thumb
# 0x020FEB94 calls it with `bl`, and the bytes are `push {r4,lr}` -- but
# Ghidra's seed had insn_len = -1, so it defaulted to ARM) and the ARM reading
# of the Thumb bytes is `cdpne` + a trap at 0x02062C48 ("undefined instruction
# 0xF94AF000", cold-boot step 508,793,671).  The committed override file
# `tools/nnk_mode_overrides.txt` names the correction; the replacement body is
# generated by `nnktool recompile-cases` (the recompiler's own emitter), never
# hand-written, and the same file drives a future
# `recompile-all --mode-overrides`.  Same policy as the splices above: no-op
# when the marker is already there, NNK_PRELUDE_DISABLE_SPLICE=5at_mode_override
# is the negative control.
if [[ -f "$IMAGE" ]] && ! grep -q 'Phase-5AT mode-override splice applied' "$IMAGE"; then
  if splice_disabled 5at_mode_override; then
    echo "== splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AT mode override (negative control) =="
  else
    if [[ ! -x nnktool/target/release/nnktool ]]; then
      echo "== phase-5at: building nnktool (the splice needs the recompiler's emitter) =="
      (cd nnktool && cargo build --release) || {
        echo "phase-5at: nnktool build failed; cannot apply the mode override" >&2; exit 2; }
    fi
    SPLICED_IMAGE6="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5at.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE6")"
    echo "== splice (Phase-5AT): wrong-mode function body corrected -> $SPLICED_IMAGE6 =="
    python3 tools/p5at_mode_splice.py "$IMAGE" "$SPLICED_IMAGE6" || {
      echo "phase-5at: mode-override splice failed" >&2; exit 2; }
    IMAGE=$SPLICED_IMAGE6
    SPLICED_FEATURES+=(5at_mode_override)
  fi
fi

# Phase-5AW: the frozen image's compiled ARM exception-return cases restore the
# mode byte and NZCV from SPSR but drop the T bit, and the dispatcher has no
# other way to classify the dynamic `lr-4` target.  Measured: 0x0200729C
# `subs pc, lr, #4` (SPSR 0x2000001F) resumed the ITCM font renderer at
# 0x01FFC684, whose previous live decode was Thumb, so the sticky Thumb guess
# walked ARM blit code as Thumb into the codec data table at 0x022C9F38 and
# halted at 0x022CA20C (`dispatch to untranslated pc`, word 0xFFFFFFD3).
# The canonical fix is in nnktool/src/recompile.rs + tools/nnk_smc.inc; this
# transform applies it to the frozen case bodies.  Same policy as the splices
# above: no-op when the marker is already there, and
# NNK_PRELUDE_DISABLE_SPLICE=5aw_exc_return_state is the negative control.
if [[ -f "$IMAGE" ]] && ! grep -q 'Phase-5AW exception-return state splice applied' "$IMAGE"; then
  if splice_disabled 5aw_exc_return_state; then
    echo "== splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5AW exception-return state (negative control) =="
  else
    SPLICED_IMAGE7="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5aw.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE7")"
    echo "== splice (Phase-5AW): exception return carries the SPSR T bit -> $SPLICED_IMAGE7 =="
    python3 tools/p5aw_exc_return_splice.py "$IMAGE" "$SPLICED_IMAGE7" || {
      echo "phase-5aw: exception-return splice failed" >&2; exit 2; }
    IMAGE=$SPLICED_IMAGE7
    SPLICED_FEATURES+=(5aw_exc_return_state)
  fi
fi

# Phase-5BB: the frozen image synthesises SPSR_irq from cpsr_ctl, which is
# state-free by design, so every IRQ taken over Thumb code told the guest its
# interrupted thread was in ARM state; the restored context then resumed ARM at
# a Thumb address (0x02137A92) and walked into the heap (halt SWI #0x65 at
# 0x02270CDC).  The canonical fix is in nnktool/src/recompile.rs (irq_enter /
# bios_irq_return) + tools/nnk_smc.inc (nnk_state_of_next); this transform
# applies it to the frozen prelude.  Same policy as the splices above: no-op
# when the marker is already there, and
# NNK_PRELUDE_DISABLE_SPLICE=5bb_irq_state is the negative control.
if [[ -f "$IMAGE" ]] && ! grep -q 'Phase-5BB IRQ-state splice applied' "$IMAGE"; then
  if splice_disabled 5bb_irq_state; then
    echo "== splice DISABLED by NNK_PRELUDE_DISABLE_SPLICE: Phase-5BB IRQ state (negative control) =="
  else
    SPLICED_IMAGE8="$(dirname "$SPLIT")/$(basename "$IMAGE" .c).p5bb.c"
    mkdir -p "$(dirname "$SPLICED_IMAGE8")"
    echo "== splice (Phase-5BB): SPSR_irq carries the interrupted T bit -> $SPLICED_IMAGE8 =="
    python3 tools/p5bb_irq_state_splice.py "$IMAGE" "$SPLICED_IMAGE8" || {
      echo "phase-5bb: IRQ-state splice failed" >&2; exit 2; }
    IMAGE=$SPLICED_IMAGE8
    SPLICED_FEATURES+=(5bb_irq_state)
  fi
fi

if [[ "$GX" == "1" ]]; then
  [[ -f "$GXDIR/Makefile" ]] || { echo "NNK_GX=1 but $GXDIR/Makefile missing" >&2; exit 2; }
  echo "== gxrender: building libgxrender.a + gx_display.o + gx_gl.o =="
  make -C "$GXDIR" -j"${BUILD_JOBS:-6}" build/libgxrender.a build/gx_display.o build/gx_gl.o >/dev/null
  rflags+=(-DNNK_GX -I"$GXDIR/src")
  # shellcheck disable=SC2207
  GX_LDFLAGS=("$GXDIR/build/gx_display.o" "$GXDIR/build/gx_gl.o" "$GXDIR/build/libgxrender.a")
  # shellcheck disable=SC2206
  GX_LDFLAGS+=($(pkg-config --libs sdl2) -lGL)
  ldflags+=("${GX_LDFLAGS[@]}")
fi

if [[ -n "${NNK_MONO:-}" ]]; then
  # Mono baseline: the untouched single-TU image, no cache, no split.
  [[ -f "$IMAGE" ]] || { echo "missing $IMAGE" >&2; exit 2; }
  echo "== MONO build: $IMAGE =="
  echo "== flags: ${gflags[*]}"
  mkdir -p "$(dirname "$OUT")"
  /usr/bin/time -f 'MONO wall %e s, maxrss %M KB' \
      "$CC" "${gflags[@]}" -o "$OUT" "$IMAGE" "${ldflags[@]}"
  ls -l "$OUT"
  echo "BUILD OK: $OUT"
  exit 0
fi

# ---- split (or reuse the existing split output) ---------------------------
# Phase-5AP: the ROM-derived pc->form map is a generated build artifact under
# the gitignored work/ tree; tools/gen_pcform.py produces it.  Default to the
# standard location so a build after generating it picks it up automatically.
# An absent map is generated from the ROM; if that fails the build stops
# (NNK_ALLOW_NO_PCFORM=1 opts into a deliberately degraded image).
if [[ -z "${NNK_PCFORM_MAP:-}" && ! -f work/phase5ap/nnk_pcform_map.h && -f tools/gen_pcform.py ]]; then
  # Phase-5AP: never build without the state authority by accident.  A missing
  # map used to yield an EMPTY map (the pre-5AP behaviour) -- that is the same
  # silent-degradation class as 5AB/5AG, so generate it from the ROM instead.
  echo "== Phase-5AP: pcform map missing -- generating it from the ROM"
  ROM_FOR_MAP="${NNK_ROM_PATH:-Ni No Kuni.nds}"
  python3 tools/gen_pcform.py --rom "$ROM_FOR_MAP" --outdir work/phase5ap \
      --tool nnktool/target/release/nnktool >/dev/null 2>&1 || true
fi
if [[ -z "${NNK_PCFORM_MAP:-}" && -f work/phase5ap/nnk_pcform_map.h ]]; then
  NNK_PCFORM_MAP=$PWD/work/phase5ap/nnk_pcform_map.h
fi
if [[ -z "${NNK_PCFORM_MAP:-}" && "${NNK_ALLOW_NO_PCFORM:-0}" != "1" ]]; then
  echo "ERROR: work/phase5ap/nnk_pcform_map.h is absent and could not be generated" >&2
  echo "       (set NNK_ALLOW_NO_PCFORM=1 to build a degraded image deliberately)" >&2
  exit 2
fi
export NNK_PCFORM_MAP="${NNK_PCFORM_MAP:-}"
if [[ -n "$NNK_PCFORM_MAP" ]]; then
  echo "== Phase-5AP: pcform map $NNK_PCFORM_MAP"
fi
if [[ -z "${NNK_SPLIT_SKIP_REGEN:-}" ]]; then
  [[ -f "$IMAGE" ]] || { echo "missing $IMAGE" >&2; exit 2; }
  echo "== split dispatch: $IMAGE -> $SPLIT =="
  python3 tools/split_dispatch.py "$IMAGE" "$SPLIT"
else
  echo "== split skipped (NNK_SPLIT_SKIP_REGEN=1): reusing $SPLIT/*.c =="
  [[ -f "$SPLIT/nnk_main.c" && -f "$SPLIT/nnk_rt.h" ]] || {
    echo "no split output in $SPLIT" >&2; exit 2; }
fi
for f in nnk_main.c nnk_rt.h; do
  [[ -f "$SPLIT/$f" ]] || { echo "split output incomplete: missing $SPLIT/$f" >&2; exit 2; }
done

# ---- Phase-5AQ: savestate field table -------------------------------------
# The savestate must carry EVERY mutable runtime static; the list is derived
# from the split output (nnk_main.c up to run() + the *.inc fragments) instead
# of being hand-maintained, so a static added by a later phase is carried
# automatically.  Regenerated on every build; it is part of the runtime TU and
# of the object cache key (HDR_HASH below).
echo "== Phase-5AQ: savestate field table =="
python3 tools/nnks_gen_state_fields.py "$SPLIT" > "$SPLIT/nnk_state_fields.h"
echo "   $(grep -c 'NNKS_F(' "$SPLIT/nnk_state_fields.h") fields in $(grep -c 'NnkStateField nnks_fields_' "$SPLIT/nnk_state_fields.h") regions"

# ---- Phase-5AG: prelude feature audit -------------------------------------
# The runtime TU ($SPLIT/nnk_main.c + the *.inc fragments next to it, after the
# splices above and split_dispatch.py's injections) must carry EVERY feature
# tools/nnk_prelude_manifest.txt marks `required`.  This is the guard that the
# Phase-5AB (VRAM read shadow) and Phase-5AF (overlay registry) class of silent
# regression cannot repeat: a prelude fix that the frozen image predates and no
# splice installs FAILS the build here, before cc spends minutes compiling, and
# the failure names the missing feature and marker.
spliced_list=""
if (( ${#SPLICED_FEATURES[@]} )); then
  spliced_list=$(IFS=,; printf '%s' "${SPLICED_FEATURES[*]}")
fi
if ! python3 tools/prelude_audit.py --split "$SPLIT" --spliced "$spliced_list"; then
  echo "" >&2
  echo "BUILD FAILED: the prelude audit rejected $SPLIT (a required prelude" >&2
  echo "feature is missing from the image; see PRELUDE_AUDIT.md).  Debug with:" >&2
  echo "  python3 tools/prelude_audit.py --split $SPLIT" >&2
  exit 1
fi

OBJDIR="$SPLIT/obj"
mkdir -p "$OBJDIR"
STATS="$OBJDIR/.build-stats"
: > "$STATS"

echo "== compile ($JOBS jobs, cached by content hash) =="
echo "== flags: guest   \"${gflags[*]}\""
echo "== flags: runtime \"${rflags[*]}\""

# Drop cached objects whose source part no longer exists (stale leftovers
# from a previous, larger manifest): they must not be compiled or linked.
pruned=0
for o in "$OBJDIR"/nnk_part_*.o "$OBJDIR"/nnk_part_*.o.stamp; do
  [[ -e "$o" ]] || continue
  base=$(basename "$o"); base=${base%.stamp}; base=${base%.o}
  if [[ ! -f "$SPLIT/$base.c" ]]; then
    echo "  prune    $base (no $base.c in manifest)"
    rm -f "$o"
    pruned=$((pruned + 1))
  fi
done

# Everything that must invalidate a cached .o is captured once here.
CC_ID=$("$CC" --version | head -1)
HDR_HASH=$(cat "$SPLIT/nnk_rt.h" "$SPLIT"/nnk_smc_map.h "$SPLIT"/nnk_smc.inc "$SPLIT"/nnk_boot.inc "$SPLIT"/nnk_dma.inc "$SPLIT"/nnk_vramcnt.inc "$SPLIT"/nnk_input.inc "$SPLIT"/nnk_mobiclip.inc "$SPLIT"/nnk_mobiclip_dec.inc "$SPLIT"/nnk_rom.inc "$SPLIT"/nnk_state.inc "$SPLIT"/nnks_container.inc "$SPLIT"/nnk_state_fields.h "$SPLIT"/nnk_pcform_map.h 2>/dev/null | sha256sum | cut -d' ' -f1)

# compile_cached <source.c> <target.o> <flag-array-name>
# Reuse <target.o> when its sidecar stamp still matches the current content
# hash; otherwise compile it and refresh the stamp atomically.  A failed
# compile deletes the object and its stamp so a stale .o can never be linked.
# The exact cc command line — including the flag set — is part of the cache
# key, so guest parts are never rebuilt because the runtime flags changed
# (or vice versa) and no stale object survives a flag flip.
compile_cached() {
  local src=$1 obj=$2
  local -n flagset=$3
  local -a cmd=("$CC" "${flagset[@]}" -I"$SPLIT" -c "$src" -o "$obj")
  local base key
  base=$(basename "$obj")
  key=$(printf 'cmd=%s\ncc=%s\nhdr=%s\nsrc=%s\n' \
        "$(printf '%q ' "${cmd[@]}")" "$CC_ID" "$HDR_HASH" \
        "$(sha256sum "$src" | cut -d' ' -f1)" | sha256sum | cut -d' ' -f1)
  if [[ -f "$obj" && -f "$obj.stamp" && "$(<"$obj.stamp")" == "$key" ]]; then
    echo "  reuse    $base"
    printf 'reused %s\n' "$base" >> "$STATS"
    return 0
  fi
  echo "  compile  $base"
  if ! "${cmd[@]}"; then
    rm -f "$obj" "$obj.stamp"
    printf 'failed %s\n' "$base" >> "$STATS"
    return 1
  fi
  printf '%s\n' "$key" > "$obj.stamp.tmp"
  mv -f "$obj.stamp.tmp" "$obj.stamp"
  printf 'compiled %s\n' "$base" >> "$STATS"
}

compile_cached "$SPLIT/nnk_main.c" "$OBJDIR/nnk_main.o" rflags
running=0
for part in "$SPLIT"/nnk_part_*.c; do
  base=$(basename "$part" .c)
  compile_cached "$part" "$OBJDIR/$base.o" gflags &
  running=$((running + 1))
  if (( running >= JOBS )); then wait -n || true; running=$((running - 1)); fi
done
wait

if grep -q '^failed ' "$STATS"; then
  echo "compile FAILED: $(grep '^failed ' "$STATS" | awk '{print $2}' | tr '\n' ' ')" >&2
  exit 1
fi
n_compiled=$(grep -c '^compiled ' "$STATS" || true)
n_reused=$(grep -c '^reused ' "$STATS" || true)
echo "== compile summary: $n_compiled compiled, $n_reused reused (of $((n_compiled + n_reused)) TUs) =="

echo "== link =="
mkdir -p "$(dirname "$OUT")"
# shellcheck disable=SC2012
"$CC" "$OBJDIR"/nnk_main.o "$OBJDIR"/nnk_part_*.o -o "$OUT" "${ldflags[@]}"
ls -l "$OUT"
echo "BUILD OK: $OUT"
echo "FLAGS OK: guest=\"${gflags[*]}\" runtime=\"${rflags[*]}\""
