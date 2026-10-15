# Lane S: is the race's double image already in the eye images KartFrame renders? No.

## Method

Scripted mock race (`frame/perf/mockdump/dumprace.sh`, wrapping hudrace.sh/run.sh, mockrun ok in every run):
Moo Moo Meadows, the head still until 51 s, then a 25 deg / 1.5 s yaw sway (`MOCK_XR_SWAY`, about 2 px a
refresh at 432 px across, up to 3.5); the kart stands until A is held from 56 s (`MOCK_XR_PRESS` hold). Five
windows of 240 consecutive sets (both eyes, every release) were dumped without stalling the app
(`MOCK_XR_DUMP_QUEUE`: blit to 432 px, written when the dumps pause; end after wake-up p99 1.1 ms during the
windows; 0-1 refreshes without a set per window, 12 in the first). Each image's stamp (eye, display time, scene words) is in
index.txt. Phases checked on the images: 48.5 s kart standing after GO, 52.5 s standing, 58.5 s leaving the
start line, 62.5 and 66.5 s driving.

`frame/perf/mockdump/zigzag.py`, per eye and window:
- **view**: global shift from column projections of the upper image (motion.py's fit), H7's zigzag rule as in
  session-h12's zig2.py (moving: |speed| >= 2 px; zigzag: |pos(n) - mean(pos(n+-1))| >= 1 px and >= 0.75 refresh).
- **betweenness** (catches any motion, forward too): r = D(n-1, n+1) / mean(D(n, n-1), D(n, n+1)), RMS image
  differences of the upper image and the ground left and right of the kart.
- lagdiff (each set against the same eye 1 and 2 sets before), eye phase, the scene stamp's steps.
- Positive control `ZZ_INJECT=2`: every odd set of the left eye replaced by the image of two sets before.

Run s3 (`KF_MOCKRUN_RENDER_SCALE=0.75`: the race at 60 fps, scene stamps exactly 0.25/0.75 as on the
headset's XRS lines, GPU 4.0 ms p50) and s2 (scale 1.0, GPU 6.7 ms, game 50-53 fps: the scene clock clamps,
steps sd 0.3 game frames).

## Results (view track; moving sets / zigzag)

| window | phase | s3 L | s3 R | s2 L | s2 R | |dev| p95 px (s3) |
| --- | --- | --- | --- | --- | --- | --- |
| 48.5 | kart and head still (noise floor) | 0 moving; 0 of 236 sets >= 1 px | same | same | same | 0.00 / 0.00 |
| 52.5 | standing, head swaying | 112 / 0 | 107 / 0 | 110 / 0 | 105 / 0 | 0.20 / 0.17 |
| 58.5 | driving off | 103 / 0 | 108 / 0 | 92 / 0 | 98 / 0 | 0.19 / 0.18 |
| 62.5 | driving | 115 / 0 | 118 / 0 | 113 / 0 | 115 / 0 | 0.14 / 0.16 |
| 66.5 | driving | 137 / 0 | 143 / 0 | - | - | 0.10 / 0.11 |
| control (62.5, 66.5) | odd L sets = n-2 | 87 / 87, 101 / 100 (1.9 refreshes, lag-1 autocorr -1.00) | 0 % | | | 6.1 / 0.16 |

- Zigzag share 0.0 % in both eyes, standing and driving, at both loads; the deviation stays under 0.1 refresh
  of motion (p95 0.10-0.20 px at 1.9-2.2 px a refresh). Lag-1 autocorrelation of the deviation -0.01 to -0.60
  (white position noise gives -0.67; a period-2 jump gives -1.00 with 2-refresh amplitude, as the control).
  L/R deviations +0.15 to +0.46 (no alternation to phase).
- Betweenness, driving with an even scene clock (62.5, 66.5): r < 1 in 0 (upper) and 0-15 (ground) of 238 sets
  per eye, lag-1 autocorrelation of r +0.88 to +1.00 (smooth); the control: upper r < 1 in 17-28 sets and
  autocorrelation -0.31/-0.35 in the injected eye only.
- lagdiff: no image identical to or nearer an older set while moving (3 of 238 at a sway reversal, both eyes).
- Scene stamps: the two eyes carry the same scene words in 2160 of 2160 sets. Where the mock's game ran under
  60 fps (s2; s3 at 58.5 s) the track-side strips followed the stamp's uneven steps in both eyes in phase
  (step correlation +0.2 to +0.67, L/R +0.31 to +0.86): the scene clock's own judder, identical per eye, absent
  on the headset (XRS weights 0.25/0.75 there). The strip track itself is too noisy for H7's rule while
  driving (fit outliers; not used for the verdict).

## Verdict

The eye images KartFrame renders and releases are smooth in both eyes, with the kart standing and driving and
the head turning: no period-2 jump, no stale or older-set image, no eye difference, at the noise floor (0 %),
while the same test flags an injected period-2 jump in 99-100 % of moving sets. Within what a mock can show,
the double image is made after the release: in what SteamVR does with the layers (or in a timing that only
the headset has). Not covered: the headset's own GPU/driver timing (the mock ran at two GPU loads, 4.0 and
6.7 ms a set, both clean).

## Next

`frame/perf/session-h13/README.md`: an xrprobe replay of these recorded race images through GalaxyFrame's
loop, with A/B cases for the per-launch eye and the KartFrame hand-over details.
