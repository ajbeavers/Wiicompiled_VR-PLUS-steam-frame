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
