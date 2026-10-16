# Headset session 20: the end locked to the compositor's grid, after and before its update window

Session 19 per frame (frame/perf/LANE_X.md): with a fixed 4000 us delay our end lands 0.4 ms before the
compositor's **update end** (vsync + 4.8 vs + 5.3) in both launches; the doubled launch crossed that edge twice as
often (25 % vs 13 % of refreshes changed side) and had four re-phase episodes (update window 2 ms early for 0.3-3 s).
No end ever landed before its refresh's update start. This session ends the frame at a fixed phase of the
compositor's own grid, published by the timing helper, well clear of that edge on either side, with the control
setting last.

Needs: the lane X build installed (`install --dev`) and lane X's frame/tools/vrtiming.py in the app dir (it publishes
the grid only when KARTFRAME_END_PHASE is in the session.env). Check per launch: game.log has
`end phase: locked to the compositor grid` once near the start and no `fixed delay` line after it (a fallback is
logged with its reason and lasts until the grid is fresh again).

| launch | file | setting | end (expected) |
|---|---|---|---|
| 1 | `01-phase-after.env` | `KARTFRAME_END_PHASE=after` | vsync + update end p95 + 0.3 ms: about vsync + 5.8, 5.4 ms after the wake-up |
| 2 | `02-phase-before.env` | `KARTFRAME_END_PHASE=before` | vsync + update start p50 - 1.5 ms: about vsync + 1.5, 1.1 ms after the wake-up |
| 3 | `03-delay-4000.env` | `KARTFRAME_END_DELAY_US=4000` (control) | vsync + 4.8 (session 19) |

All three: `KARTFRAME_FRAME_LOG=1 KARTFRAME_VRTIMING_FRAMES=1 KARTFRAME_RECORD=raw KARTFRAME_SESSION_ENV_ONCE=1`.
Queue: copy the three into `$KARTFRAME_HOME/session.queue/`. Routine as always: menus, Time Trial, head turns at the
countdown, then driving, hold still, quit. Owner: per launch and per eye, clean or doubled, whether it changed during
the race (a beat, a burst), and whether one eye feels "more sensitive to movement" than the other (session 19
launch 2's right eye).

## Reading each launch

`python3 -I frame/tools/phase.py $KARTFRAME_HOME/sessions/<launch> [...]` (race only; `--all` for the whole launch):
- **D** (new) the game's `end phase:` lines: the mode, any fallback, and every 10 s the achieved end after the vsync
  and minus the published update start / end (p50 / p95), the published window, the end after the wake-up and the
  frames on the fixed delay (expect 0 after the first second).
- **C** (new) every end against the update window of the refresh it lands in, from SteamVR's own clock: before /
  inside / after counts with SteamVR's presents (0x / 2x+ / mispresented) per class, the class changes between
  consecutive refreshes, end minus update end, and the re-phase episodes (update start < vsync + 1.5) with times.
  Expected: after -> "after" 95 % or more and class changes under 5 % (the rest in episodes and hitches); before ->
  "before" 90 % or more (the early windows of episodes catch it: count them); control -> like session 19
  (inside 78-89 %, changes 13-25 %).
- A and B as in session 19 (the poses-ready / frame-ready margins; the app's times against the nearest anchor; B's
  "end vs frame ready" is now the working clock check, about -0.1 ms).

## What the outcomes would mean

Display-time bookkeeping: xrWaitFrame's predicted display time is unchanged by when the frame is ended (session 19:
display time = vsync + 2.59 ms in both launches); each set is located for it two refreshes ahead when it starts at
the wake-up, and SteamVR counts the frame for the compositor frame after the wake-up's refresh (poses ready -8.1 ms,
frame ready -3.5 ms against that frame's vsync). "after" ends at vsync + 5.8, still 2.7 ms before the earliest update
start of that compositor frame (vsync + 8.5): same refresh shown, same poses, no latency change; only the margin to
the compositor shrinks (6.2 -> about 5.3 ms in SteamVR's A view).
- **after clean in both eyes, right eye no longer "more sensitive"**: the model holds (the right eye was taking the
  set a refresh early while our end was inside the window). Next: make it the default (helper always publishing,
  fallback 4000 us), then check whether the vsync part can come from xrWaitFrame's display time in process (constant
  +2.59 ms in session 19, LANE_W option 1) with the helper only refining the window.
- **after doubled in the right eye**: a second sensitive point after the update end (session 17's 6000 us launch,
  about vsync + 6.9, had the right eye very bad): view C plus the verdict place it; next session sweeps after:<us>
  (0.3 / 0.8 / 1.3 ms) against the grid.
- **before clean**: if both eyes then feel "more sensitive" (the compositor taking the images at the update of the
  wake-up's own refresh, a refresh before the display time they were located for; view C shows it as presents 2x or
  mispresented, or as frame ready before the update start of the frame SteamVR counts it for), the bookkeeping fix is
  to locate each set for the display time one period earlier (one refresh, 8.3 ms, less pose prediction: lower
  latency) so poses match the shown refresh; if they feel normal, SteamVR holds the frame for its own refresh and
  nothing changes. Before is expected to double during re-phase episodes (their windows start 0.2-1.4 ms after the
  vsync); count them in view C against the verdict. Risk: the set started before the end must finish its start
  (locate, acquire, publish) within 1.1 ms; if it does not, e= in the XRF lines turns bimodal (session 17's 2500 us
  launch); view D's end-after-wake-up p95 shows it.
- **control doubled while after is clean** (or the reverse) on the same day: the strongest A/B; if all three come
  out the same, queue 01 and 03 again.

Recording note: framecorder at 30 fps in races costs 3-8 ms of GPU per recorded frame; the compositor's re-phase
episodes might be its doing. If the verdicts disagree with view C, one launch without KARTFRAME_RECORD settles it.

## Headless check (lane X build, mock runtime, 60 s each, 120 Hz grid from frame/tools/openvr-stub.c with the update
window at vsync + 3.0 to + 5.3, mock wake-up at vsync + 0.4)

| run | end after vsync p50 / p95 | e (XRF, after the wake-up) p50 | sets rendered / 60 s | late | mock errors |
|---|---|---|---|---|---|
| after | 5.66 / 5.70 (minus update end +0.36) | 5.21 | 7060 | 8 | 0 |
| before | 1.56 / 1.58 (minus update start -1.44) | 1.10 | 7076 | 5 | 0 |
| delay 4000 | (no grid) | 4.08 | 7070 | 7 | 0 |
| after, helper stopped at 15 s | locked, then `fixed delay 4000 us (grid older than 0.5 s)` at about 15 s | 5.19, then 4.06 | 4051 / 35 s | 8 | 0 |

mockrun ok in all (KF_MOCKRUN_END_GATE=100: every end is later than 1 ms by design), ctest 32/32. Cost: one open +
pread + close of the 64-byte record at most every 50 ms (1.2 us each here), nothing per frame beyond arithmetic.

## Headset results (2026-10-16, orchestrator's reading)

Owner: launch 1 (end phase after) "pretty good, no doubling; the right eye towards the end wasn't as good,
and a little jitter"; launch 2 (before) "terrible all the way through, the left eye clear for a bit then
both really bad"; launch 3 (4000 us control) "very good in the left eye, right eye bad with doubling; in
the water the left eye jittered as well".

Logs. Launch 1: locked to the grid nearly throughout (brief fallbacks to the fixed delay in the first
seconds: "grid period differs from xrWaitFrame's", 15 switch pairs, 176 of the first 1185 frames on the
fixed delay, then 0-5 per 10 s); end at vsync + 5.85 ms p50, +0.54 ms after the published update end (p95
+0.7); compositor app frames reused 238 (3.2 %), mispresented 405 (the frame misses its refresh's update
and goes out one refresh later: the jitter); the last 10 s had an early-start episode (published update
start 0.57 ms): the right eye's bad stretch. Launch 2: the "before" target (update start p50 - 1.5 ms) was
not reachable: ends at vsync + 2.3 ms p50 but p95 4.8 ms (the set started before the end pushes it), i.e.
inside the window often: 4.0 % late sets, bad. Launch 3 (control): inside 75 % / after 25 %, class changes
29.7 %, end minus update end -0.35 p50: doubled in the right eye as in session 18.

THE FACT THAT CHANGES THE MODEL: the clean probe of session 13 ended its frames 0.31 ms after its wake-up
(SteamVR's poses->frame ready column), the same phase as the doubled default game (0.33 ms); GalaxyFrame
ends at about 1.2 ms (max 2-3) and is clean. So the end's phase alone does not decide: at phase 0.3 the
probe is clean and the game doubles. The phase lock treats a symptom (it makes the per-eye pick-up order
consistent). What the game has at that phase and the probe lacks: 4-7 ms of GPU work per set submitted
right after the end on the same GPU the compositor composes on (GalaxyFrame: ~1-3 ms, clean), and the
multi-threaded CPU load. The compositor's own per-frame GPU fields (m_flCompositorRenderGpuMs,
m_flPreSubmitGpuMs, m_flPostSubmitGpuMs, m_flTotalRenderGpuMs) are in vrframes.log and have not been
compared between clean and doubled launches yet. Next: lane Y.
