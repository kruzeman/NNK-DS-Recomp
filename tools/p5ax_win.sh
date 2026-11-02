#!/usr/bin/env bash
# Phase-5AX: the operator's window path from the operator's savestate.
# Boots the state in NNK_GX=window, presses A through xdotool (a real SDL key
# event, not the script), captures the compositor frame + the SDL window.
set -u
D=/tmp/ax/win
mkdir -p "$D"
cd /home/hermes/NiNoKuni/worktrees/intro-distorted || exit 1
export NNK_GX=window
export NNK_SAVESTATE=work/repro_5ax/operator_pre_intro.nnks
export NNK_SAVESTATE_IGNORE_BUILD=1
export NNK_GX_SHOT="$D/c/f"
export NNK_GX_SHOT_EVERY=25
export NNK_GX_WINDOW_PPM="$D/cap.ppm"
export NNK_STEP_LIMIT=2600000000
"$2" '/home/hermes/NiNoKuni/Ni No Kuni.nds' > "$D/log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null' EXIT
for i in $(seq 1 300); do
  grep -q "state: loaded" "$D/log" && break
  sleep 0.1
done
echo "state loaded: $(grep -c 'state: loaded' "$D/log")"
wid=""
for i in $(seq 1 100); do
  wid=$(xdotool search --name "Ni no Kuni" 2>/dev/null | head -1)
  [ -n "$wid" ] && break
  sleep 0.1
done
echo "window id: ${wid:-NONE}"
sleep 1.0
xdotool key --window "$wid" --clearmodifiers a 2>/dev/null || xdotool key --clearmodifiers a
echo "A pressed at $(date +%T)"
wait $PID
echo "runner done rc=$?"
grep -E "movie:|done steps|frames presented" "$D/log" | tail -12
