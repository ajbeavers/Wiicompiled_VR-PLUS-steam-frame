# Headset session 12 (lane R): how far the shown poses are from the head, and two levers

After eleven sessions one problem is left: in races the image doubles on head turns, the right eye worse
than the left, standing still as well as driving. Menus are clean, and so is GalaxyFrame on the same
SteamVR. Everything inside the app has been proven correct headless (pacing, finished releases, exact
stamps and end times, both eyes from one packet and pose; lanes A to Q).

One measurement was never made in KartFrame: the one GalaxyFrame logs every 5 s and passes. How far is
each shown frame's pose from a fresh xrLocateViews for the moment it is shown? GalaxyFrame's clean run:
"left eye 0.10 mm 0.024 deg avg, 0.79 mm 0.061 deg max; right eye 0.09 mm 0.024 deg avg, 0.98 mm 0.061 deg
max". GalaxyFrame locates each set at one wake-up for the next refresh, so its poses are predicted about
one refresh plus SteamVR's lead ahead. KartFrame's sets ahead are located two refreshes ahead, and
SteamVR's predicted display time is already about 25 ms after the wake-up: about 42 ms of prediction.
If SteamVR composes each eye against a fresh prediction of its own (the right eye later), a pose that far
ahead shows as a per-eye displacement the size of the head's unpredicted motion over those tens of ms:
the zigzag of one to two refreshes of motion seen on the recordings.

## What the build logs (all three launches)

game.log, every 5 s and whenever the scene changes between menus and race:

- `vr: views of N sets: M for the display time they went out at (off X ms avg, Y max); against a locate
  then: left eye A mm B deg avg, C mm D deg max; right eye ...; eye distance E-F mm; race|menus, S s; that
  locate G ms avg H max`. GalaxyFrame's check (the same words): at every xrEndFrame, one xrLocateViews for
  the display time being ended, in the layer's space, after the release; per eye the distance (mm) and the
  angle (deg) between the pose the shown images were rendered from and that locate. Counted once per set,
  when it first goes out. "that locate" is the check's own cost.
- `vr: poses predicted ahead (N sets, S s, race|menus): display time minus the set's locate p50 X ms, max Y`.
- XRF lines (KARTFRAME_FRAME_LOG=1) end in `aL=<deg> aR=<deg>`: the same angle for every frame, repeats too.

`frame/tools/session-report.py <session>` shows all of it for menus and race separately.

## Install

Build: lane R's branch, `install --dev`. Put `01-measure.env`, `02-one-ahead.env` and `03-fresh-pose.env`
into `$KARTFRAME_HOME/session.queue/`; the wrapper takes one per launch, in name order. Each records the
panels (`KARTFRAME_RECORD=raw`) and logs every frame (`KARTFRAME_FRAME_LOG=1`).

**Routine, each launch:** menus, then straight into a Time Trial. During the countdown (kart standing
still) look at a track-side edge and turn the head slowly left and right for about ten seconds, hold still
five; then drive and do the same for ten seconds, hold still five. Say: doubled or not, which eye,
standing still and while driving, and whether the menus are still clean.

## Launch 1: `01-measure.env` (build defaults)

What I should see: the race as in sessions 1 to 11. This launch is the measurement.

game.log: `poses predicted ahead` about 42 ms in the race (two refreshes plus SteamVR's lead; the menus the
same). The `views of` lines are the result.

## Launch 2: `02-one-ahead.env` (`KARTFRAME_AHEAD_REFRESHES=1`)

Each set is located for the next refresh instead of the one after, and started at the wake-up before the
frame is ended (GalaxyFrame's order). One refresh (8.3 ms) less prediction. If sets do not fit a refresh on
the headset, late sets repeat the last frame (a stutter); judge the doubling, not the smoothness.

game.log: `OpenXR pipelined pacing: sets rendered one refresh ahead (KARTFRAME_AHEAD_REFRESHES=1), each
started at the wake-up before the frame is ended, ...`, `OpenXR eye swapchain pairs: 4`; `poses predicted
ahead` about 33 ms; the pacing lines' `late=` and the race late % in session-report.py (mock: 0.6 %).

## Launch 3: `03-fresh-pose.env` (`KARTFRAME_SUBMIT_POSE=fresh`)

Sets two refreshes ahead as in launch 1, but every race frame that shows a new set is submitted with the
poses of the check's own locate for that frame's display time. The images stay as rendered. SteamVR then
has nothing left to correct but its own compose-time delta; the prediction error of the set (what the
check measures) stays in the image as a small swim of the whole picture with the head, the same in both
eyes. Menus and repeats of the last frame are unchanged.

game.log: as launch 1; the `views of` numbers are the error the images now carry.

## Reading it

- **Launch 1, per-eye error of a degree or more** (GalaxyFrame: 0.024 avg, 0.06 max), and the zigzag
  smaller in launch 2 and/or 3: the prediction distance is the cause. Next: make the better launch the
  default (launch 2's order, or fresh poses), then look at the one that remains.
- **Launch 1 errors like GalaxyFrame's** (a few hundredths of a degree avg, under about 0.1 max while
  turning): the hypothesis is dead; launches 2 and 3 should look like launch 1. Next: an xrprobe session
  that submits KartFrame's own eye images (recorded from a race) through GalaxyFrame's loop, to split the
  images from the hand-over.
- Either way compare left and right in the `views of` lines: the doubling is worse in the right eye; a
  right eye error clearly larger than the left would point at the eye poses themselves.
- Panel analysis as before: `frame/perf/panels/analyse.sh <session>` per launch, H7 section 1's zigzag
  table (zigzag refreshes per eye, race against menus).

## Mock results (lane R)

Scripted Grand Prix (`frame/perf/hudrace.sh`, 120 Hz, `MOCK_XR_BUSY_MS=1`, `KF_MOCKRUN_END_GATE=2`). The mock's
head pose is a function of the display time, so the check reads 0 except for SteamVR-like prediction
jitter (0.02 ms) and sets shown later than their own time.

Defaults, 180 s, `MOCK_XR_STAMP=1 KARTFRAME_FRAME_LOG=1`:

| check | result |
| --- | --- |
| mock errors | 0; releases changed after the GPU passed them 0 of 42799 |
| stamps for their poses' locate | 34938 of 34946 (8 unstamped), 0 for another locate, 0 with the other eye's |
| end times exact | 21475 of 21475; ends > 1 ms after the wake-up 0.8 % (gate 2 %) |
| lane P's counters | eye poses 0, uniform probe 0 / 0, pose stamps 34938 < 0.01 deg |
| `views of` lines, steady menus and race | 0.00 mm 0.000 deg avg, 0.00 mm 0.002 deg max per eye; off 0.01 avg 0.04 max ms |
| `views of`, the two windows at the race start | sets shown up to 233 ms after their own time (loading stalls): up to 5.4 mm 9.6 deg |
| `poses predicted ahead` | 41.4 ms p50, 41.6 max (the mock's lead is SteamVR's 3 refreshes, 24.9 ms, plus 2 refreshes) |
| the check's locate | 0.001 ms avg, 0.1 ms max (mock; the headset's is in the line) |

ctest 32 of 32.

`KARTFRAME_AHEAD_REFRESHES=1` against the default, interleaved, 90 s each, both locks (run.sh):

| run | errors | end p50 / p99 ms | ends > 1 ms | race late sets | gpu p50 / p99 ms | predicted | GPU clock avg, SoC max |
| --- | --- | --- | --- | --- | --- | --- | --- |
| two ahead 1 | 0 | 0.18 / 0.80 | 0.6 % | 0.3 % | 4.11 / 5.13 | 41.5 ms | 780 MHz, 96 C |
| one ahead 1 | 0 | 0.25 / 0.85 | 0.7 % | 0.7 % | 4.08 / 5.15 | 33.2 ms | 781 MHz, 95 C |
| two ahead 2 | 0 | 0.19 / 0.69 | 0.4 % | 0.1 % | 4.06 / 5.10 | 41.5 ms | 786 MHz, 95 C |
| one ahead 2 | 0 | 0.25 / 0.91 | 0.9 % | 0.7 % | 4.09 / 5.05 | 33.2 ms | 767 MHz, 88 C |

One ahead, started before the end: 58 and 61 late sets of about 10500 (lane M's order: about half late).
With stamps (120 s): 0 errors, 20486 of 20496 stamps exact, end times exact.

`KARTFRAME_SUBMIT_POSE=fresh`, `MOCK_XR_STAMP=fresh` (fresh-pose layers must carry a locate for their own
display time; each image is checked against its own stamp's locate), 180 s: 0 errors; 17448 layers with new
images, all posed by a locate for their display time; their images rendered for that display time 34856,
1 to 2 refreshes before 10, 3 or more 26 (race start); pose stamps 34916 < 0.01 deg; ends > 1 ms 0.3 %.
Validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, 80 s, menus and race, fresh poses and
stamps): 0 messages, 0 mock errors.

Scratch: `$HOME/.cache/kf-laneR/runs/`.
