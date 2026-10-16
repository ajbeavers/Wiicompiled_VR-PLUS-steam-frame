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

## Headset results (2026-10-16, orchestrator's reading)

Owner: launch 1 (session 012613) "bad, doubled in the right eye, very subtle in the left and only in
water"; launch 2 (012746) "very very good, no doubling, but the right eye much more sensitive to movement,
the left dead on". Same setting (end 4000 us after the wake-up, compositor frames logged). phase.py:

| ms | doubled 012613 | clean 012746 |
| --- | --- | --- |
| wake-up vs vsync | +0.34 p50 | +0.42 p50 |
| end vs vsync (signed, nearest) | -3.66 p50, i.e. vsync + 4.67 | -3.61, i.e. vsync + 4.72 |
| compositor update start after vsync | p5 +0.22, p50 +2.83, p95 +3.17 | p5 +0.62, p50 +3.03, p95 +3.20 |
| compositor update end after vsync | p5 +3.37, p50 +5.25 | p5 +4.74, p50 +5.32 |
| end minus nearest update start | p5 -3.07, p50 +1.87, p95 +3.28 | p5 +1.17, p50 +1.74, p95 +3.09 |
| release minus nearest update start | p5 -3.03, p50 +1.76 | p5 +1.21, p50 +1.69 |
| app frames presented 0x / 2x+ / mispresented | 140 / 129 / 301 | 42 / 45 / 90 |
| display time vs vsync | +2.59 | +2.59 |

Reading: in both launches our release and end land INSIDE the compositor's update window (update start
about vsync + 3.0, end about vsync + 5.3; our release at vsync + 4.6). In the clean launch that relation
held every frame (end 1.2-3.1 ms after the update start, never before it). In the doubled launch the
compositor's update started early (near vsync + 0.2) in at least 5 % of frames, so our release then fell
3 ms BEFORE the update instead of inside it: the eye-by-eye pick-up order flipped between refreshes.
A consistent relation gives the owner's "right eye more sensitive to movement" (one eye consistently takes
the newer image: the compositor takes each eye's latest released image when it reaches that eye in its
update, and our release of the next set lands between the two eyes); an inconsistent one gives the
doubling. Fix direction: never release inside the update window, and release at a phase that is the same
every frame against the compositor's clock (published by the timing helper), e.g. right after the update
end or safely before the update start; both eyes then always show the same set.
