#!/bin/bash
# A real race in the mock, with the owner's HUD settings, and eye images to look at (lane G).
#
#   frame/perf/hudrace.sh NAME [SECONDS] [VAR=value ...]
#
# Scripted presses of the right A button (MOCK_XR_PRESS) walk from the title screen through license,
# Grand Prix, character, kart and cup to Luigi Circuit (about 30 s in; the scratch home keeps the
# license, so later runs are a little faster); the race HUD then sits on the virtual screen
# (hud_virtual_screen = true, hand_hud = false, as the owner plays). Eye images are written at 65 and
# 75 s (two frames each, every 4th pixel) and converted to PNG in the run directory's dump/.
# Look at them: TIME, LAP, the position and the course map must be on the screen in front of the kart.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NAME=${1:?name}; SECS=${2:-80}; shift; [ $# -gt 0 ] && shift
OUT=${KF_PERF_OUT:-$HOME/.cache/kf-laneH/runs}
DUMP=$OUT/$NAME/dump
export KF_PERF_OUT=$OUT KF_PERF_HOME=${KF_PERF_HOME:-$HOME/.cache/kf-laneH/home-race}
TMP=$OUT/.$NAME-dump; rm -rf "$TMP"; mkdir -p "$TMP"   # run.sh empties the run directory first
bash "$HERE/run.sh" "$NAME" "$SECS" MOCK_XR_SKIP=1 TU_DEBUG=sysmem "KF_MOCKRUN_VR=hud_virtual_screen = true
hand_hud = false" "MOCK_XR_PRESS=/user/hand/right/input/a/click@12/1.5" MOCK_XR_HANDS=1 \
  MOCK_XR_DUMP=$TMP MOCK_XR_DUMP_AT=65,75 MOCK_XR_DUMP_FRAMES=2 "$@" || true
mkdir -p "$DUMP"
mv "$TMP"/*.ppm "$DUMP"/ 2>/dev/null || true; rm -rf "$TMP"
python3 -I - "$DUMP"/*.ppm <<'PY'
import sys, zlib, struct
for src in sys.argv[1:]:
    data = open(src, 'rb').read()
    head = data.split(b'\n', 3)
    w, h = map(int, head[1].split())
    px = data[len(data) - w * h * 3:]
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(src[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                                        + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
PY
rm -f "$DUMP"/*.ppm
grep -a 'entered RaceScene\|screen clip self-check' "$OUT/$NAME/game.log" | tail -3
echo "hudrace: eye images in $DUMP"
