#!/bin/bash
# analyse.sh SESSION_DIR [OUT_DIR] [RACE_FROM_S]: the whole panel analysis of a session recorded with
# KARTFRAME_RECORD=raw (panels.mkv): decode per eye (extract.sh), compare each refresh with the 8 before
# it (lagdiff.py), global image motion (motion.py), then cadence.py's summary into OUT_DIR/cadence.txt.
# About 4 minutes for a 2.5 minute recording; OUT_DIR (default ~/.cache/kf-panels/<session>) holds 4 GB.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
S=${1:?session dir}; OUT=${2:-$HOME/.cache/kf-panels/$(basename "$S")}; FROM=${3:-30}
[ -s "$S/panels.mkv" ] || { echo "no $S/panels.mkv" >&2; exit 1; }
"$HERE/extract.sh" "$S/panels.mkv" "$OUT"
python3 -I "$HERE/lagdiff.py" "$OUT" > "$OUT/lagdiff.txt"
python3 -I "$HERE/motion.py" "$OUT" > "$OUT/motion.txt" 2>/dev/null
python3 -I "$HERE/cadence.py" "$OUT/lagdiff.txt" "$OUT/motion.txt" "$FROM" | tee "$OUT/cadence.txt"
