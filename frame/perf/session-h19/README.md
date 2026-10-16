# Headset session 19: the same 4.0 ms end twice, with the compositor's phase logged

Sessions 16 and 18 ended the frame 4000 us after the wake-up: clean in both eyes once, right eye doubled on repeat,
with identical app logs. This session repeats that setting with every SteamVR compositor frame logged
(`KARTFRAME_VRTIMING_FRAMES=1`: vrframes.log in the session folder) and the app's wake-up, end, release and
display time in CLOCK_MONOTONIC (XRF `m=`, `r=`, `t=`), so the launch that looks clean and the one that doubles
can be compared by where the end lands inside the compositor's frame. Design and clock matching:
frame/perf/LANE_W.md.

Needs: the lane W build installed (`install --dev`; XRF lines end in `m= r= t=`) and lane W's
frame/tools/vrtiming.py in the app dir (with only the new vrtiming.py, view A below still works).

| launch | file | setting |
|---|---|---|
| 1 | `01-end-delay-4000-phase.env` | `KARTFRAME_END_DELAY_US=4000`, frame log, compositor frames, recording |
| 2 | `02-end-delay-4000-phase.env` | the same |

Queue: copy both into `$KARTFRAME_HOME/session.queue/`. Routine as always: menus, Time Trial, head turns at the
countdown, then driving, hold still, quit. Owner: per launch and per eye, clean or doubled (and whether it
changed during the race, e.g. a beat every few seconds). If both launches come out the same, queue the two
files again (up to four launches): the comparison needs one clean and one doubled launch at 4000.

## What phase.py prints

`python3 -I frame/tools/phase.py $KARTFRAME_HOME/sessions/<launch 1> $KARTFRAME_HOME/sessions/<launch 2>`
(race only; `--all` for the whole launch, `--every 5` for a shorter drift table, `--hist all` for every histogram):
- the compositor frame count, missed frames (expect 0), the refresh (8.333 ms), and SteamVR's system time as
  CLOCK_MONOTONIC minus an offset (spread a few us; 0 would mean the same clock).
- A (SteamVR's own measurements, no clock matching): our wake-up (poses ready) and end (frame ready) in ms after
  each compositor frame's vsync reference, the compositor's update start / end and render start, and the margin
  from our frame being ready to the compositor's update start; histograms in 0.25 ms bins.
- B (our clock): end, release, wake-up and display time minus the nearest compositor vsync, update start and
  render start (signed, +-4.17 ms), with histograms; "end vs frame ready" should be about 0 (the clock check).
- a drift table per second: end-vsync, end-update, end-render, wake-vsync, margin.

What to look for: e (end after the wake-up) will be the same in both launches; the model says the wake-up vs
vsync (A poses ready, B wake-up vs vsync) differs, so the end vs update / render start differs, with the doubled
launch's end piled near an anchor or its margin near 0, and the clean one well clear. If the display time vs vsync
(B) is the same constant in both, the lock can be derived from xrWaitFrame's display time in process (LANE_W.md
option 1); if it differs, the helper publishes the compositor's phase (option 2). If no number differs between
the clean and the doubled launch, the eye pick-up is not tied to what GetFrameTimings reports: line the panel
recording up with vrframes.log next.
