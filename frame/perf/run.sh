#!/bin/bash
# One measured mock run (lane E): frame/perf/run.sh NAME SECONDS [VAR=value ...]
#
# Runs frame/tools/mockrun.sh under the shared build lock (one heavy GPU/CPU job at a time) with the
# given environment, samples the game's read I/O beside it (iosample.py), and keeps the game log, the
# mock trace and the samples in $KF_PERF_OUT/NAME (default ~/.cache/kf-laneE/runs). The user state is
# one scratch home ($KF_PERF_HOME, default ~/.cache/kf-laneE/home), so Dawn's and Aurora's pipeline
# caches are warm after the first run; KF_PERF_COLD=1 starts from an empty home instead.
# Variables of interest: TU_DEBUG=sysmem, KARTFRAME_PACING, KARTFRAME_REFRESH_HZ, MOCK_XR_HZ,
# KF_MOCKRUN_RENDER_SCALE, DISABLE_VULKAN_FDM_INJECTION_LAYER=1, MOCK_XR_SKIP=1.
# Then: frame/tools/session-report.py <dir>/game.log; frame/perf/hitches.py <dir>.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?name}; SECS=${2:?seconds}; shift 2
OUTROOT=${KF_PERF_OUT:-$HOME/.cache/kf-laneE/runs}
HOMEDIR=${KF_PERF_HOME:-$HOME/.cache/kf-laneE/home}
D=$OUTROOT/$NAME
rm -rf "$D"; mkdir -p "$D"
[ "${KF_PERF_COLD:-0}" = 1 ] && rm -rf "$HOMEDIR"
mkdir -p "$HOMEDIR"
for kv in "$@"; do export "$kv"; done
export KF_MOCKRUN_HOME=$HOMEDIR KARTFRAME_PACING_SAMPLES=$D/samples.txt KARTFRAME_FPSLOG=${KARTFRAME_FPSLOG:-1}
printf '%s\n' "$@" > "$D/env.txt"
EXE=${KF_BUILD:-$(cd "$HERE/../.." && pwd)/build-linux}/kartframe
# A game process (the binary itself, a Steam launch, Lepton/Waydroid), not a shell whose command line
# merely mentions one (an agent's "kfbuild.sh . kartframe" kept this waiting forever).
busy() { pgrep -x kartframe || pgrep -f '^[^ ]*reaper SteamLaunch' || pgrep -x waydroid || pgrep -f '^/bin/bash [^ ]*/lepton '; }
exec 9>"$HOME/Games/KartFrame/.build.lock"
while :; do
  while busy >/dev/null; do echo "perf: $NAME: a game is running, retrying in 5 min"; sleep 300; done
  echo "perf: $NAME waiting for the build lock"; flock 9
  busy >/dev/null || break
  flock -u 9
done
echo "perf: $NAME running ${SECS}s ($*)"
KF_IOSAMPLE_EXE=$EXE python3 -I "$HERE/iosample.py" "$D/io.tsv" &
SAMPLER=$!
set +e
bash "$HERE/../tools/mockrun.sh" --seconds "$SECS" --exe "$EXE" > "$D/mockrun.out" 2>&1
RC=$?
set -e
kill $SAMPLER 2>/dev/null || true
LOG=$(ls -t "$HOMEDIR"/logs/*-mock.log 2>/dev/null | head -1)
[ -n "$LOG" ] || { cat "$D/mockrun.out"; exit 1; }
cp "$LOG" "$D/game.log"
[ -f "${LOG%.log}.trace" ] && mv "${LOG%.log}.trace" "$D/mock.trace"
rm -f "$LOG"
tail -3 "$D/mockrun.out"
exit $RC
