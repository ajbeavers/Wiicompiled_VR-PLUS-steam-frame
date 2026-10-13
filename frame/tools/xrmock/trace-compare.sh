#!/bin/bash
# Runs the real galaxyframe launcher of two builds against the mock OpenXR
# runtime and compares the call traces (what the app does with swapchains and
# frames, see xrmock.c).
#
#   tools/xrmock/trace-compare.sh <build dir A> <build dir B> [modes...]
#
# A build dir holds galaxyframe (galaxyquest in builds from before the rename) and libgame.so (build-linux).  Modes (default
# all three): paired (120 Hz, a game frame over two refreshes), unpaired
# (72 Hz, both eyes each refresh), warp (120 Hz with SpaceWarp: motion vector
# and depth swapchains).  Exit status 0 when every mode's traces match.
#
# Environment:
#   XRMOCK_SECONDS  length of each run (15)
#   XRMOCK_SCRATCH  scratch folder (~/.cache/gq-xrmock): the mock, a scratch
#                   GALAXYFRAME_HOME (game files linked, saves and shader cache
#                   copied, nothing in ~/.local/share/GalaxyFrame is written),
#                   and the traces
#   XRMOCK_GAME / XRMOCK_NAND / XRMOCK_SHADERS / XRMOCK_INI   sources for the
#                   scratch home (default: ~/.local/share/GalaxyFrame/...)
#   IGNORE_EXT      extension names (comma separated) that may be in the
#                   instance extension list of one build only
#   GFX             PETARI_GFX for both runs (gles; the game's own default is Vulkan)
# The traces are compared line by line without frame numbers, session events
# and swapchain image indices: the setup lines as they are, then the frames
# from a common point of the steady state (see run()), up to the length of
# the shorter run.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
A=$(cd "${1:?build dir A}" && pwd); B=$(cd "${2:?build dir B}" && pwd); shift 2
MODES=("$@"); [ ${#MODES[@]} -eq 0 ] && MODES=(paired unpaired warp)
SECS=${XRMOCK_SECONDS:-15}
S=${XRMOCK_SCRATCH:-$HOME/.cache/gq-xrmock}
DATA=${GALAXYFRAME_HOME:-$HOME/.local/share/GalaxyFrame}
HOME_DIR=$S/home
pgrep -x "galaxyframe|galaxyquest" > /dev/null && { echo "the game is running; quit it first" >&2; exit 1; }
RT=$("$HERE/build.sh" "$S/mock")
mkdir -p "$S"
# The launcher under another name: scripts of other agents refuse to run
# while a process called galaxyframe exists.
cp "$A/galaxyframe" "$S/gqmock" 2> /dev/null || cp "$A/galaxyquest" "$S/gqmock"

run() { # build tag mode
  local build=$1 tag=$2 mode=$3 out=$S/trace_${2}_$3
  rm -rf "$out" "$HOME_DIR"; mkdir -p "$out" "$HOME_DIR"
  ln -s "${XRMOCK_GAME:-$DATA/game}" "$HOME_DIR/game"
  cp -r "${XRMOCK_NAND:-$DATA/nand}" "$HOME_DIR/nand"
  cp "${XRMOCK_SHADERS:-$DATA/shaders.bin}" "$HOME_DIR/shaders.bin" 2> /dev/null || true
  cp "${XRMOCK_INI:-$DATA/petari_vr.ini}" "$HOME_DIR/petari_vr.ini" 2> /dev/null || true
  local env=()
  case $mode in
    paired) env=(MOCK_XR_HZ=120) ;;
    unpaired) env=(MOCK_XR_HZ=72) ;;
    warp) env=(MOCK_XR_HZ=120 MOCK_XR_SPACEWARP=1) ;;
    *) echo "unknown mode $mode" >&2; return 1 ;;
  esac
  env "${env[@]}" EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy XR_RUNTIME_JSON="$RT" MOCK_XR_TRACE="$out/trace.txt" MOCK_XR_SECONDS=$SECS \
    GALAXYFRAME_HOME="$HOME_DIR" PETARI_SAVE_DIR="$HOME_DIR/nand" PETARI_GFX=${GFX:-gles} GALAXYFRAME_LIB="$build/libgame.so" \
    timeout $((SECS + 40)) nice -n 5 "$S/gqmock" > "$out/stdout.txt" 2>&1 || echo "  run $tag $mode exited with status $?"
  cp "$HOME_DIR/petari_log.txt" "$out/petari_log.txt" 2> /dev/null || true
  # Normalized: no frame numbers, no events, no swapchain image indices (their
  # rotation depends on how many frames the start took); the ignored
  # extensions removed.
  sed 's/^f[0-9]* //; s/ idx=[0-9]*//' "$out/trace.txt" | grep -v '^event' > "$out/norm.txt"
  if [ -n "$IGNORE_EXT" ]; then
    for e in ${IGNORE_EXT//,/ }; do sed -i "/^createInstance/s/ $e\\b//" "$out/norm.txt"; done
  fi
  # The setup (up to the first frame) is compared as it is.  The frames start
  # with the game's first picture, whose time depends on the machine: they are
  # compared from the first set of eye images rendered after the first panel
  # layer appeared (the game's screen), when the frame loop is in a steady state.
  awk '/^waitFrame/ {exit} {print}' "$out/norm.txt" > "$out/setup.txt"
  local q start
  q=$(grep -n 'quad\[' "$out/norm.txt" | head -1 | cut -d: -f1)
  start=1
  [ -n "$q" ] && start=$(awk -v q="$q" 'NR > q && /^acquire sc0$/ {print NR; exit}' "$out/norm.txt")
  tail -n +"${start:-1}" "$out/norm.txt" > "$out/frames.txt"
}

status=0
for m in "${MODES[@]}"; do
  run "$A" A "$m"; run "$B" B "$m"
  da=$S/trace_A_$m; db=$S/trace_B_$m
  if ! diff "$da/setup.txt" "$db/setup.txt" > "$S/diff_setup_$m.txt"; then
    echo "$m: SETUP DIFFERS, see $S/diff_setup_$m.txt"; head -n 10 "$S/diff_setup_$m.txt"; status=1
  fi
  la=$(wc -l < "$da/frames.txt"); lb=$(wc -l < "$db/frames.txt")
  # Up to the shorter run, without its last frame (a run ends mid-frame).
  n=$((la < lb ? la : lb)); n=$((n > 8 ? n - 8 : n))
  if [ "$n" -lt 100 ]; then echo "$m: traces too short ($n lines): see $S/trace_*_$m"; status=1; continue; fi
  if diff <(head -n "$n" "$da/frames.txt") <(head -n "$n" "$db/frames.txt") > "$S/diff_$m.txt"; then
    echo "$m: frames identical over $n lines ($(grep -c '^endFrame' "$da/frames.txt") / $(grep -c '^endFrame' "$db/frames.txt") frames)"
  else
    echo "$m: FRAMES DIFFER, see $S/diff_$m.txt"; head -n 10 "$S/diff_$m.txt"; status=1
  fi
done
exit $status
