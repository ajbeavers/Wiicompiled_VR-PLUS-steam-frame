# Headset session 16

Three launches, each one a single test of a CPU-side timing cause for the right-eye double image: 01 thread
priority boost and utilisation clamp off (01-noprio.env), 02 each set's render start delayed 4 ms after the
wake-up (02-ahead-start.env, KARTFRAME_AHEAD_START_US), 03 each frame ended 4 ms after the wake-up
(03-end-delay.env, KARTFRAME_END_DELAY_US). Routine for each: menus, Time Trial, head turns at the countdown,
driving, hold still, quit; analyse the KARTFRAME_RECORD=raw recording with frame/perf/panels/analyse.sh.

## Lane V mock check (launch 03)

Scripted race, frame/perf/hudrace.sh 90 s, MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KARTFRAME_FRAME_LOG=1
KF_MOCKRUN_END_GATE=100 KARTFRAME_END_DELAY_US=4000, against the same race with no delay (60 s):

| | end delay 4000 us | default |
|---|---|---|
| mock errors | 0 | 0 |
| end after wake-up p50 / p99 | 4.38 / 4.92 ms | 0.28 / 0.85 ms |
| ends with the predicted display time | 10659 of 10659 | 7198 of 7198 |
| sets rendered a second (late, repeats) | 118 of 120 (0 late, ~0 repeats) | 120 (0, 0) |
| end_late= a second | all (by design; the end gate was set to 100) | 0 |

The delayed end starts the next set before the sleep, as KARTFRAME_AHEAD_REFRESHES=1 does, which holds one
more eye pair until the end: the switch therefore defaults to 4 eye pairs. With 3 pairs the early start
found no free pair, the set started after the end instead and the race fell to 80 sets a second with 40
repeats and pairwait=40 a second.

## Headset results (2026-10-15, orchestrator's reading)

Owner: launch 1 (priority boost and uclamp off) "both eyes bad and doubling"; launch 2 (set start delayed
4 ms) "left eye good, right eye doubling"; launch 3 (frame ended 4 ms after the wake-up) "BOTH EYES GOOD,
NOT DOUBLING; the right eye had a slight jitter, more sensitive to head movements, the left eye dead on".

The first clean race since session 3. Logs, launch 3: ends 4.50 ms after the wake-up p50 (XRF e: 7083 of
7324 race frames in 4-5 ms, 210 in 5-6, 31 later than 6), race 120.3 sets/s, 0.2 % late, 2.3 % retained
repeats, 4 eye pairs; GPU per set 7.0-7.3 ms p50 (launch 1: 4.8; the next set now starts before the end).
Launch 2: 117.9 sets/s, 3.2 % late. Launch 1: 119.8, 0.7 % late, ends at 0.35 ms.

Reading: WHEN the frame is ended inside the refresh decides the doubling. Ended at the wake-up (within
~0.4 ms; every doubled session 4-15): doubled. Ended ~4.5 ms after the wake-up: clean in both eyes. Session
14's drained launch (ends 5.5 ms, left eye clean) and session 3 (late ends, clean) were the same effect.
Model: SteamVR's compositor on the Frame picks up the app's layer for each eye at its own point inside the
refresh, the right eye later than the left; an end that lands near a pick-up point is seen by one eye for
this refresh and by the other for the next, so consecutive refreshes disagree (the period-2 zigzag, worse in
the eye whose pick-up it straddles, which eye depending on where the end lands). The right eye's residual
jitter at 4.5 ms says its pick-up is near there. Session 17 sweeps the delay (2.5 ms and 6 ms) to find the
window; then the clean value becomes the default and its latency cost is measured.
