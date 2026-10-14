# Headset session 9 (lane M): per-eye placement

The eighth session's launch 2 (`$KARTFRAME_HOME/sessions/20261014-045742`, sets two refreshes ahead, every
frame ended at the wake-up) had the cleanest pacing so far, yet the image still doubled on head turns,
mostly in the right eye (zigzag on 41.6 % of moving refreshes, left 30.7 %), with the kart standing
still too. What is left is where each eye's image is placed: the per-eye poses we locate, render and
submit against what SteamVR uses when it composes each eye. SteamVR on the Frame has gaze-dependent eye
positions on (`steamvr.gazeDepEyeToHeadEnable`; not to be touched): each eye's pose from xrLocateViews
moves with the tracked gaze, and the eyes counter-rotate during head turns.

Install the files into `$KARTFRAME_HOME/session.queue/` (the wrapper takes them in name order, one
per launch; 03 is optional, see below). All record the panels (`KARTFRAME_RECORD=raw`) and log every frame and every locate
(`KARTFRAME_FRAME_LOG=1`).

**Routine, each launch:** menus, then straight into a race (Time Trial). During the countdown, or with
the kart braked to a standstill, look at a track-side edge and turn the head slowly left and right for
about ten seconds, hold still five; then drive and do the same for ten seconds, hold still five. Say for
each launch: doubled or not, which eye, standing still and while driving.

## 01-located.env: the default, measured

`KARTFRAME_EYE_OFFSETS=located` (the runtime's own eye poses, as in session 8).

- Owner: as session 8's launch 2 (doubled on turns, right eye more).
- Log: "OpenXR eye offsets: located (the runtime's per-locate eye poses); logged per second (vr eyes)";
  one `XRE ... m=L` line per locate; one `vr eyes: ... mode=located` line a second.
- **What it answers** (`frame/tools/session-report.py <session>`, rows "eye offset change per locate"
  and "eye rotation change", menus/race, all seconds and turning seconds = head p99 above 30 deg/s):
  - offsets that move: in turning seconds `dpos_mm` p50 clearly above the still seconds' (more than
    about 0.02 mm a locate) and/or `drot_deg` above about 0.01 deg, right eye larger than left: the gaze
    reaches the eye poses every refresh. The XRE lines show the per-refresh steps against the head (`q`).
  - offsets that do not move (`dpos_mm` and `drot_deg` p99 at the noise floor, both eyes, turning or
    not): the gaze candidate is out; launch 2 should then look like launch 1.
  - `dfov_deg` above 0: the fields of view also follow the gaze (`fixed` keeps the captured ones).

## 02-fixed.env: fixed eye-to-head offsets

`KARTFRAME_EYE_OFFSETS=fixed`: every locate also locates the VIEW space (the head) for the same display
time; after 120 focused locates (one second, in the menus) each eye's offset from the head (position,
orientation) and its field of view are frozen at their medians, and from then on each eye is rendered and
submitted at head pose x fixed offset with the frozen field of view. The submitted pose is the rendered
pose, as before. The gaze cannot reach the images; SteamVR reprojects our eye pose to its own (a steady
few-millimetre offset at most).

- Owner: if the gaze was the cause, the zigzag is gone or much weaker, both eyes alike.
- Log: "OpenXR eye offsets: fixed (...)", then once "OpenXR eye offsets fixed (median of 120 focused
  locates): IPD <mm>, eye midpoint from the head <x,y,z> mm, |left x| - |right x| <mm>; left: ...;
  right: ... captured spread mm p50/max ..." (the asymmetry between the eyes is in that line);
  `vr eyes: ... mode=fixed fixed=<locates a second> nohead=0`; XRE lines `m=C` for the first second,
  `m=F` after (their offsets are still the LOCATED ones, so launch 1's measurement repeats here).
  Pacing as in launch 1 (no extra cost: one xrLocateSpace per locate).
- `nohead` above 0: the head locate was not valid for those locates and they used the located poses.

## 03-fixed-1ahead.env (optional): fixed offsets and one refresh less prediction

`KARTFRAME_EYE_OFFSETS=fixed KARTFRAME_AHEAD_REFRESHES=1`: each set is located for the next refresh's
display time instead of two ahead (about 33 ms of prediction instead of 42; the set has one refresh less
for its encode and GPU work).

- Caveat from the mock (below): in the race about half the sets do not finish within that one refresh.
  They are not repeats: the loop settles into showing each set one refresh after the time it was located
  for (late counts 120 a second in those seconds, repeats stay near 0), which is a two-refresh prediction
  with a pose one refresh off. So launch 3 tests the shorter prediction only where sets fit (menus, light
  race seconds); install it only if launch 2 leaves the question open.
- Owner: compare with launch 2: cleaner still means the prediction distance mattered too.
- Log: "OpenXR pipelined pacing: sets rendered one refresh ahead (KARTFRAME_AHEAD_REFRESHES=1) ...";
  pacing lines: `late` up to 120 a second in the race, `repeats` low; XRF `l=` 1.00 instead of 0.00 on
  the frames shown a refresh late.

## Mock (lane M, build of lane/M, 180 s each at 120 Hz, the attract-mode race from t=98 s)

`MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KARTFRAME_FRAME_LOG=1 KF_MOCKRUN_END_GATE=2`, run in the order fixed,
located, fixed+1 ahead. The mock's eye offsets are constant (no gaze), so `fixed` must give the same
images as `located`; it tests the plumbing and the exact checks, not the effect.

| run | errors | releases changed after the GPU | stamps for their poses' locate | end times off | ends > 1 ms | late sets (race) | race sets/s | pose matches within 1e-6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| fixed | 0 | 0 of 43064 | 10084 of 10086 (2 unstamped) | 0 of 21601 | 0.9 % | 14 (0.0 %) | 120.6 | 10938 of 21544 |
| located | 0 | 0 of 42819 | 10042 of 10046 (4 unstamped) | 0 of 21601 | 1.2 % | 146 (2.7 %) | 116.0 | 0 |
| fixed, 1 ahead | 0 | 0 of 43046 | 10110 of 10118 (8 unstamped) | 0 of 21601 | 0.8 % | 5002 (49.7 %) | 119.3 | 11549 of 21546 |

- `fixed` passes every check; half its layers match their locate to rounding (the head pose composed with
  the captured offset) instead of bit for bit. Captured: IPD 64.00 mm, asymmetry 0.00 mm, spread 0.
- located vs fixed late sets: GPU state between runs (race GPU p50 8.13 against 6.24 ms; an earlier
  located run had 12 late sets in all); one xrLocateSpace a locate costs nothing measurable.
- 1 ahead: 18.9 % of all sets late, 49.7 % in the race (race GPU p50 8.14 ms, encode 3.5: a set needs more
  than the one refresh); every frame still ended at the wake-up (0.8 % later than 1 ms). ctest 32 of 32.

## Afterwards

Panel analysis per launch (`frame/perf/panels/analyse.sh <session>`, then the zigzag table of H7.md
section 1, aligned by the black refreshes). Decisions:

- located doubled, fixed clean: the gaze-dependent eye poses were the cause; make `fixed` the default.
- both doubled, located offsets moving: the gaze is not (or not only) the cause; compare launch 3.
- launch 3 clean, launch 2 not: prediction distance; weigh `AHEAD_REFRESHES=1` against its late sets.
- all three doubled: the cause is in what SteamVR does with the layer, not the poses we give it.
