#!/bin/bash
# Runs the game against the mock OpenXR runtime (xrmock), headless: no window, no sound.
#
#   frame/tools/mockrun.sh [--flat] [--seconds N] [--game DIR] [--exe PATH]
#
# --flat runs without the mock: VR off, desktop renderer (title screen check).
# Environment: MOCK_XR_* settings of xrmock (MOCK_XR_SECONDS, MOCK_XR_HZ, MOCK_XR_TRACE, ...),
# KF_BUILD (build dir, default <repo>/build-linux), KF_MOCKRUN_HOME (scratch user dir, default
# ~/.cache/kf-mockrun). The game reads its user state from $XDG_DATA_HOME/WiiCompiled (Config.toml,
# NAND, Cache), which this script points at the scratch dir and seeds with a Config.toml naming the
# extracted game (~/Games/KartFrame/data/game or --game).
#
# Safety: XR_RUNTIME_JSON must name an existing, non-empty mock runtime, else the script aborts. An
# empty XR_RUNTIME_JSON once fell through to the real SteamVR in the headset.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
FLAT=0; SECONDS_RUN=60; GAME=$HOME/Games/KartFrame/data/game
EXE=${KF_BUILD:-$REPO/build-linux}/kartframe
while [ $# -gt 0 ]; do
  case $1 in
    --flat) FLAT=1 ;;
    --seconds) SECONDS_RUN=$2; shift ;;
    --game) GAME=$2; shift ;;
    --exe) EXE=$2; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "mockrun: $*" >&2; exit 1; }
[ -x "$EXE" ] || die "no game binary at $EXE"
[ -f "$GAME/.complete" ] || die "no extracted game ($GAME/.complete is missing)"
DATA=$GAME; [ -f "$GAME/sys/fst.bin" ] || DATA=$GAME/DATA
[ -f "$DATA/sys/fst.bin" ] && [ -d "$DATA/files" ] || die "$GAME is not an extracted DATA directory (sys/fst.bin, files/)"

# Never next to a running game or the owner's session.
if pgrep -af 'SteamLaunch|kartframe|lepton|waydroid' | grep -v "pgrep\|mockrun" >/dev/null; then
  pgrep -af 'SteamLaunch|kartframe|lepton|waydroid' | grep -v "pgrep\|mockrun" >&2
  die "a game is running; wait for it"
fi

OUT=${KF_MOCKRUN_HOME:-$HOME/.cache/kf-mockrun}
mkdir -p "$OUT/data/WiiCompiled" "$OUT/logs"
cat > "$OUT/data/WiiCompiled/Config.toml" <<TOML
[paths]
dvd_root = "$DATA"

[video]
widescreen = true
display_mode = "windowed"

[vr]
enabled = $([ $FLAT = 1 ] && echo false || echo true)
required = $([ $FLAT = 1 ] && echo false || echo true)
TOML
LOG=$OUT/logs/$(date +%Y%m%d-%H%M%S)-$([ $FLAT = 1 ] && echo flat || echo mock).log

# Headless: nothing on the owner's displays, nothing audible.
unset DISPLAY WAYLAND_DISPLAY
export SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
export XDG_DATA_HOME=$OUT/data

if [ $FLAT = 0 ]; then
  MOCK_DIR=${KF_XRMOCK_DIR:-$HOME/.cache/kf-xrmock}
  [ -s "$MOCK_DIR/libxrmock.so" ] || bash "$HERE/xrmock/build.sh" "$MOCK_DIR" >/dev/null
  export XR_RUNTIME_JSON=$MOCK_DIR/runtime.json
  [ -n "${XR_RUNTIME_JSON}" ] && [ -s "$XR_RUNTIME_JSON" ] || die "XR_RUNTIME_JSON is empty or missing: $XR_RUNTIME_JSON"
  LIBPATH=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime"]["library_path"])' "$XR_RUNTIME_JSON")
  [ -s "$LIBPATH" ] || die "mock runtime library missing or empty: $LIBPATH"
  export MOCK_XR_SECONDS=${MOCK_XR_SECONDS:-$SECONDS_RUN}
  export MOCK_XR_TRACE=${MOCK_XR_TRACE:-$OUT/logs/$(basename "$LOG" .log).trace}
else
  unset XR_RUNTIME_JSON
fi
echo "mockrun: $EXE ($([ $FLAT = 1 ] && echo flat || echo "mock runtime $XR_RUNTIME_JSON")), ${SECONDS_RUN}s, log $LOG"
set +e
timeout -s INT --kill-after=15 $((SECONDS_RUN + 120)) "$EXE" > "$LOG" 2>&1 &
PID=$!
if [ $FLAT = 1 ]; then sleep "$SECONDS_RUN"; kill -INT $PID 2>/dev/null; fi
wait $PID; RC=$?
echo "mockrun: exit $RC"
grep -a 'xrmock: summary' "$LOG" || true
exit $RC
