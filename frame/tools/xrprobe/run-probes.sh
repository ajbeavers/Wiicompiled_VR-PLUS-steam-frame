#!/bin/bash
# Probe session: runs the five xrprobe cases one after another, 30 s each
# with a 5 s pause between, and collects every log into one dated directory.
# Put the headset on, start SteamVR, then run this from a terminal:
#   tools/xrprobe/run-probes.sh [extra xrprobe options, e.g. --hz 90]
# Environment: PROBE_SECONDS (30), PROBE_PAUSE (5), PROBE_OUT (output directory),
#   XRPROBE (binary), VRTIMING (vrtiming.py; skipped when missing).
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${XRPROBE:-$HERE/build/xrprobe}
RUN_SECONDS=${PROBE_SECONDS:-30}
PAUSE=${PROBE_PAUSE:-5}
OUT=${PROBE_OUT:-$HOME/Games/GalaxyQuest/probe/probes-$(date +%Y%m%d-%H%M%S)}
VRTIMING=${VRTIMING:-$HOME/Games/MoonlightXRNative/src/tools/vrtiming.py}

if [ ! -x "$BIN" ]; then
    echo "xrprobe not built. Build it with:"
    echo "  cmake -S $HERE -B $HERE/build -G Ninja -DCMAKE_C_COMPILER=clang"
    echo "  flock ${GQROOT:-~/Games/GalaxyQuest}/.build.lock cmake --build $HERE/build"
    exit 1
fi
if ! pgrep -x vrcompositor >/dev/null; then
    echo "SteamVR is not running (no vrcompositor). Start SteamVR, put the headset on, then run this again."
    exit 1
fi
mkdir -p "$OUT"
touch "$OUT/.start"

# name | xrprobe arguments | what to watch
CASES=(
"1-pace|--pace|Read the frame number on the panel. Does it ever flicker, tear (part of the panel from another frame), jump backwards or freeze on a stale number? Does the ticker bar under it stutter or jump back?"
"2-pace-fence|--pace=fence|Same panel, the control case. Same questions: any flicker, tearing, backwards or stale numbers? Compare with case 1."
"3-synth|--synth|A checkerboard slides left and right (new frames only 30 times a second). Is the motion smooth, or does it judder, double or smear? Turn your head a little too."
"4-synth-no-mv|--synth --no-mv|The same checkerboard and frame rate without motion vectors. Smoother or rougher than case 3?"
"5-depth-only|--depth-only|The checkerboard at full frame rate with depth submitted. Smooth? Any warping or wobble around its edges when you move your head?"
)

echo "Probes: ${#CASES[@]} cases of ${RUN_SECONDS} s, logs in $OUT"
[ -f "$VRTIMING" ] || echo "(vrtiming.py not found at $VRTIMING: compositor counters will be missing)"
{
    echo "Probe session $(date -Iseconds)"
    echo "extra options: $*"
} > "$OUT/session.txt"

i=0
for c in "${CASES[@]}"; do
    i=$((i + 1))
    IFS='|' read -r name args watch <<< "$c"
    echo
    echo "=== Next: case $i of ${#CASES[@]}: $name (xrprobe $args $*)"
    echo "    Watch: $watch"
    for s in $(seq "$PAUSE" -1 1); do
        printf '\r    starting in %d s ' "$s"
        sleep 1
    done
    printf '\r    running for %s s       \n' "$RUN_SECONDS"

    vt_pid=""
    if [ -f "$VRTIMING" ]; then
        env -u LD_LIBRARY_PATH python3 -I "$VRTIMING" $((RUN_SECONDS + 5)) "$OUT/$name.vrtiming.log" \
            > /dev/null 2>&1 &
        vt_pid=$!
    fi
    # shellcheck disable=SC2086  # $args holds several options on purpose
    timeout -k 5 $((RUN_SECONDS + 45)) "$BIN" $args "$@" --seconds "$RUN_SECONDS" --log "$OUT/$name.log" \
        > "$OUT/$name.stdout" 2>&1
    rc=$?
    if [ -n "$vt_pid" ]; then
        kill "$vt_pid" 2>/dev/null
        wait "$vt_pid" 2>/dev/null
    fi
    echo "$name rc=$rc" >> "$OUT/session.txt"
    if [ "$rc" -ne 0 ]; then
        echo "    case $name exited with code $rc; last lines:"
        tail -n 5 "$OUT/$name.stdout" | sed 's/^/      /'
    else
        grep -h 'SUMMARY' "$OUT/$name.stdout" | sed 's/^/    /'
    fi
done

# SteamVR's client logs for these runs, if it wrote any
find "$HOME/.local/share/Steam/logs" -maxdepth 1 -name 'xrclient_*' -newer "$OUT/.start" \
    -exec cp {} "$OUT/" \; 2>/dev/null
[ -f "$HOME/.local/share/Steam/logs/perfcriteria.txt" ] && cp "$HOME/.local/share/Steam/logs/perfcriteria.txt" "$OUT/"
rm -f "$OUT/.start"

echo
echo "Done. Logs in $OUT"
echo "Tell the agent, for cases 1 and 2: did the numbers ever flicker, tear or look stale?"
echo "For cases 3, 4 and 5: which looked smooth?"
