#!/bin/bash
# abgp.sh PREFIX ROUNDS SECONDS "A env..." "B env..." [common env...]: A/B on the scripted Grand Prix (Luigi Circuit)
# Each run: frame/perf/hudrace.sh (deterministic: the same course every time, unlike the demo races),
# KARTFRAME_FPSLOG=2 segments (gpuprof.py), pacing (pacing-parts.py), GPU clock and temperature
# (run.sh gpufreq.tsv). Interleave A and B so they share the clock and thermal state (lane I).
P=$1; R=$2; S=$3; A=$4; B=$5; shift 5
HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE/../.."
for r in $(seq 1 $R); do for v in A B; do
  E=$A; [ $v = B ] && E=$B
  KF_PERF_OUT=${KF_AB_DIR:-$HOME/.cache/kf-ab}/runs KF_PERF_HOME=${KF_AB_DIR:-$HOME/.cache/kf-ab}/home-race frame/perf/hudrace.sh $P-$v$r $S KARTFRAME_FPSLOG=2 MOCK_XR_DUMP_AT=999 $E "$@" > /dev/null 2>&1
  d=${KF_AB_DIR:-$HOME/.cache/kf-ab}/runs/$P-$v$r
  echo "== $P-$v$r ($E)"
  python3 frame/perf/pacing-parts.py $d | grep '| race '
  python3 frame/perf/gpuprof.py $d | grep -E '^. part|race 1p' | cut -c1-330
  grep -a 'VR eyes:' $d/game.log | cut -c40-260
  awk 'NR==1{t0=$1} {t=$1-t0; if(t>50){n++; f+=$2; if($4>tm)tm=$4}} END{printf "gpu MHz avg %.0f, max temp %.1f C\n", f/n/1e6, tm/1000}' $d/gpufreq.tsv
done; done
