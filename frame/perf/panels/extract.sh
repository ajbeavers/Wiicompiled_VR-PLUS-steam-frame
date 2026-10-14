#!/bin/bash
# extract.sh PANELS.MKV OUTDIR [W H]: decodes a framecorder --view raw recording (both panels side by side)
# once into per-eye grayscale raw files (OUTDIR/L.gray, OUTDIR/R.gray, W x H bytes a frame, default 480x270,
# the eye halves scaled with area averaging) and OUTDIR/pts.txt (one presentation time in seconds per frame,
# the panel vblank the frame was grabbed at). Frames the recorder missed leave gaps in pts.txt.
set -euo pipefail
in=$1; out=$2; W=${3:-480}; H=${4:-270}
mkdir -p "$out"
ffprobe -v error -select_streams v:0 -show_entries frame=pts_time -of csv=p=0 "$in" > "$out/pts.txt" &
ffmpeg -hide_banner -loglevel error -threads 3 -i "$in" -an -fps_mode passthrough -filter_complex \
  "[0:v]format=gray,split[a][b];[a]crop=iw/2:ih:0:0,scale=$W:$H:flags=area[l];[b]crop=iw/2:ih:iw/2:0,scale=$W:$H:flags=area[r]" \
  -map '[l]' -f rawvideo -fps_mode passthrough "$out/L.gray" -map '[r]' -f rawvideo -fps_mode passthrough "$out/R.gray" -y
wait
echo "$W $H" > "$out/geometry.txt"
echo "frames: $(wc -l < "$out/pts.txt"), L $(stat -c %s "$out/L.gray") bytes"
