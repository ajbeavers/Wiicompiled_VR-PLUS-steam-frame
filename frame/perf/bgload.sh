#!/bin/bash
# Background CPU load for timing runs (lane H): frame/perf/bgload.sh SECONDS [JOBS] [NICE]
#
# JOBS (default 4) loops compiling the mock runtime's C file with clang -O2, at nice NICE (default 5,
# as kfbuild.sh builds), for SECONDS, the way a build of another project loads the Frame's cores and
# memory. Nothing is written outside a scratch directory under ~/.cache/kf-laneH.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SECS=${1:?seconds}; JOBS=${2:-4}; NICE=${3:-5}
SRC=$HERE/../tools/xrmock/xrmock.c
OUT=$HOME/.cache/kf-laneH/bgload; mkdir -p "$OUT"
INC=${OPENXR_INCLUDE:-/usr/include}
end=$((SECONDS + SECS))
for j in $(seq "$JOBS"); do
  ( while [ $SECONDS -lt $end ]; do
      nice -n "$NICE" clang -O2 -c "$SRC" -I "$INC" -I "$HERE/../tools/xrmock" -o "$OUT/load$j.o" 2>/dev/null || sleep 1
    done ) &
done
wait
