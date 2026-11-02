#!/usr/bin/env bash
# Phase-5AR window F5/F9 proof.  Run under xvfb:
#   xvfb-run -a -s "-screen 0 1024x768x24" bash tools/nnks_win_f5f9.sh <dir> <rom> <runner> <state>
# It boots the window from <state>, presses F5 through the real SDL filter,
# lets the guest move on, presses F9, and compares the frame-numbered captures
# the runtime writes (same frame index => byte-identical) plus the actual SDL
# window captures.  Prints a PASS/FAIL summary and leaves the PPM/PNG evidence
# in <dir>.
set -u
D=$1
ROM=$2
BIN=$3
STATE=$4
mkdir -p "$D/c" "$D/win"
export NNK_GX=window
export NNK_SAVESTATE="$STATE"
export NNK_SAVESTATE_DIR="$D/win"
export NNK_GX_SHOT="$D/c/w"
export NNK_GX_SHOT_EVERY=5
export NNK_GX_WINDOW_PPM="$D/win/cap.ppm"
# no NNK_STEP_LIMIT: booting from a state gives the resumed run a full
# compile-time budget (nnk_state.inc adjusts it; an explicit value is absolute)
"$BIN" "$ROM" > "$D/win/log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT

wait_log() {  # wait_log <pattern> <timeout-seconds>
  local i
  for i in $(seq 1 $(( $2 * 10 ))); do
    grep -q "$1" "$D/win/log" && return 0
    sleep 0.1
  done
  return 1
}
wait_log "state: loaded" 30 || { echo "FAIL: boot state never loaded"; exit 1; }

wid=""
for i in $(seq 1 100); do
  wid=$(xdotool search --name "Ni no Kuni" 2>/dev/null | head -1)
  [ -n "$wid" ] && break
  sleep 0.1
done
[ -n "$wid" ] || { echo "FAIL: no SDL window"; exit 1; }
echo "window id $wid"
xdotool windowfocus "$wid"
sleep 1.5            # let a few frames present so F5 has a visible scene

cp "$D/win/cap.ppm" "$D/win/window_saved.ppm" 2>/dev/null

xdotool key --clearmodifiers F5
wait_log "state: saved" 20 || { echo "FAIL: F5 did not save"; exit 1; }
SAVE=$(grep "state: saved" "$D/win/log" | tail -1)
echo "F5: $SAVE"
F=$(echo "$SAVE" | sed -n 's/.*frame=\([0-9]*\).*/\1/p')
F0=$(( (F / 5) * 5 ))          # frame-numbered capture at/below the save point
echo "saved frame=$F capture base=$F0"

# the pre-F9 pass: copy the capture at F0+5 (written by the continued run) and
# a later one as the "moved on" scene.
for i in $(seq 1 100); do [ -f "$D/c/w.$(printf %05d $((F0+5))).top.ppm" ] && break; sleep 0.1; done
cp "$D/c/w.$(printf %05d $((F0+5))).top.ppm" "$D/pass1_f$((F0+5)).top.ppm"
cp "$D/c/w.$(printf %05d $((F0+5))).bot.ppm" "$D/pass1_f$((F0+5)).bot.ppm"
sleep 3                       # move on: the guest presents more frames
LAST=$(ls "$D"/c/w.*.top.ppm | sed 's/.*w\.\([0-9]*\)\.top.ppm/\1/' | sort -n | tail -1)
cp "$D/c/w.$LAST.top.ppm" "$D/moved_f$LAST.top.ppm"
cp "$D/win/cap.ppm" "$D/win/window_moved.ppm" 2>/dev/null
echo "moved on to frame $LAST"

T9=$(date +%s)
xdotool key --clearmodifiers F9
wait_log "F9 restored" 20 || { echo "FAIL: F9 did not restore"; exit 1; }
echo "F9: $(grep "F9 restored" "$D/win/log" | tail -1)"
sleep 2

# the post-F9 pass: the same frame index is re-rendered by the restored run
T=$(printf %05d $((F0+5)))
for i in $(seq 1 100); do
  [ -f "$D/c/w.$T.top.ppm" ] && [ "$(stat -c %Y "$D/c/w.$T.top.ppm")" -ge "$T9" ] && break
  sleep 0.1
done
cp "$D/c/w.$T.top.ppm" "$D/pass2_f$((F0+5)).top.ppm"
cp "$D/c/w.$T.bot.ppm" "$D/pass2_f$((F0+5)).bot.ppm"
cp "$D/win/cap.ppm" "$D/win/window_restored.ppm" 2>/dev/null

echo "--- comparisons"
RC=0
cmp "$D/pass1_f$((F0+5)).top.ppm" "$D/pass2_f$((F0+5)).top.ppm" && echo "PASS frame $((F0+5)) top identical" || RC=1
cmp "$D/pass1_f$((F0+5)).bot.ppm" "$D/pass2_f$((F0+5)).bot.ppm" && echo "PASS frame $((F0+5)) bot identical" || RC=1
cmp -s "$D/pass1_f$((F0+5)).top.ppm" "$D/moved_f$LAST.top.ppm" && echo "WARN moved scene equals saved scene" || echo "PASS moved frame $LAST differs from saved frame $((F0+5))"
cmp -s "$D/win/window_saved.ppm" "$D/win/window_restored.ppm" && echo "note window captures identical" || echo "note window captures differ (host-time frame offset)"
echo "SUMMARY rc=$RC"
exit $RC
