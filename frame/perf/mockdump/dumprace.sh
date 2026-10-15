#!/bin/bash
# Consecutive eye images of a scripted mock race (lane S): frame/perf/mockdump/dumprace.sh NAME [VAR=value ...]
#
# hudrace.sh's Grand Prix to Moo Moo Meadows with: menu taps until 46 s (they also skip the course intro),
# the head still until 51 s and then swaying 25 deg with a 1.5 s period (MOCK_XR_SWAY), A held from 56 s
# (driving), and 240-set windows of both eyes' released images (MOCK_XR_DUMP_QUEUE: no stalls) at
# 48.5 s (kart and head still: the noise floor), 52.5 s (kart standing, head swaying), 58.5, 62.5 and
# 66.5 s (driving). Images in $KF_DUMP_DIR (default ~/.cache/kf-laneS/dump-NAME), 432 px across
# (render scale 0.75, every 3rd pixel, linear filter: the game runs its race at 60 fps, as on the headset;
# at 1.0 the mock's race drops to 50-53 fps and the scene clock clamps). Then:
#   python3 -I frame/perf/mockdump/zigzag.py <dump dir>
# Env: SECS (70), DUMP_AT (the window times), KF_PERF_OUT / KF_PERF_HOME as for run.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?name}; shift
DD=${KF_DUMP_DIR:-$HOME/.cache/kf-laneS/dump-$NAME}
rm -rf "${DD:?}"; mkdir -p "$DD"
export KF_PERF_OUT=${KF_PERF_OUT:-$HOME/.cache/kf-laneS/runs} KF_PERF_HOME=${KF_PERF_HOME:-$HOME/.cache/kf-laneS/home-race}
bash "$HERE/../hudrace.sh" "$NAME" "${SECS:-70}" MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KARTFRAME_FRAME_LOG=1 \
  "MOCK_XR_PRESS=/user/hand/right/input/a/click@12/1.5/0.15/46;/user/hand/right/input/a/click@56/1000/1000" \
  MOCK_XR_SWAY=25,1.5,51 KF_MOCKRUN_RENDER_SCALE=0.75 MOCK_XR_DUMP="$DD" MOCK_XR_DUMP_AT="${DUMP_AT:-48.5,52.5,58.5,62.5,66.5}" \
  MOCK_XR_DUMP_FRAMES=80 MOCK_XR_DUMP_STEP=3 MOCK_XR_DUMP_QUEUE=512 "$@"
echo "dumprace: $(wc -l < "$DD/index.txt") images in $DD"
