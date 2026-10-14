# Headset session 11 (lane P): the head pose each eye was rendered with

Hypothesis tested: the pacing thread publishes every set's packet into one buffer (`published_frame_`), and
a publish of the next set landing between the left and the right eye's uniform fill would give the right
eye the next set's head pose, one refresh ahead: a right-eye image placed a refresh or two of motion off,
alternating with timing. The bridge's stamp could not see it (it stamps the set's display time, not what
each eye's draws used).

## What the app does (read, then measured)

- The frame worker takes each set's packet **once**, by value, under `published_mutex_`
  (`openxr_integration.cpp` ProvideStereoFrame: exchange + copy under the lock; `aurora.cpp`
  request_stereo_frame returns an `AuroraStereoFrame` by value). Both eyes' transforms, the display time,
  the cockpit, the panel and menu placement of that set come from that one copy
  (`make_stereo_replay_frame`); the seal path and the refresh sets (`run_retained_stereo_frame`) each consume
  one packet and nothing re-reads the published buffer mid-set. The pacing thread publishes the next packet
  only after the last set reached Aurora's queue (AheadCycle polls it first). The stamp's display time is
  that copy's (`run_stereo_sink(..., input->displayTimeNanos)`). So the snapshot the hypothesis asks for is
  already the design; no change was needed in the publish path.
- Per-eye uploads in the eye passes (cockpit vertices, window mask, panel overlay uniform, stamp block) each
  have their own per-eye destination; sets are encoded and submitted one at a time on the frame worker.
- The one shared GPU resource: every refresh set of a game frame rewrites the same `g_uniformBuffer` region
  (the sealed frame's eye ranges) with a copy at its start. Order between set k's eye draws and set k+1's
  copy rests on Dawn's barrier in queue order. Probed (below).

## Instrumentation (lane P, in the build)

- At both uniform fill sites (the seal's eye pair, `prepare_late_stereo_replay`), per eye: the packet
  (display time, token) and the eye transform written, the eye frustum, and a count of draws of that eye
  filled with another packet or transform (`gfx::EyeSceneRecord::pose*`).
- `KARTFRAME_FRAME_LOG=1`: the per-set `XRS` line carries `XRP tL= tR= kL= kR= yL= yR= mL= mR= same=` (packet
  time per eye in steady ms, token, eye yaw in degrees, mixed draws, 1 when both eyes used the set's own
  packet). Always: `VR eye poses (10 s): N sets, eyes from different packets 0, an eye not from the set's
  packet 0, an eye filled with more than one transform 0`.
- GPU uniform probe (`KARTFRAME_FRAME_LOG=1` or `KARTFRAME_UNIFORM_PROBE=1`): after both eyes of a set,
  each eye's first perspective range is copied back from `g_uniformBuffer` and compared with the bytes its
  fill wrote: `VR eye uniform probe (10 s): N sets read back after their eyes, left eye's range not what
  its fill wrote 0, right eye's 0`. A set k+1 copy reaching set k's eye ranges early would show here.
- Test stamp (`KARTFRAME_FRAME_STAMP=1`, mock only): 64x15 pixels, one pixel a bit (was 4x4-pixel cells),
  rows 7-14 the pose words; `xrmock: pose stamps` compares each eye with the layer's submitted pose and
  field of view and the eyes of a set with each other.

## Mock results (scripted Grand Prix, 120 Hz, head sway 0.3 rad at 0.5 Hz, default loop)

`frame/perf/hudrace.sh NAME 180 MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KARTFRAME_FRAME_LOG=1 KF_MOCKRUN_END_GATE=2`,
build dac9971 (scratch `$HOME/.cache/kf-laneP/runs/p-final`):

| check | result |
| --- | --- |
| sets with both eyes stamped | 17468 |
| eyes filled from different packets | **0** |
| eye images whose fill pose is off the submitted pose (>= 0.01 deg) | **0** of 34960 (right only 0, left only 0) |
| eye images whose packet is not the layer's locate | 0 |
| eye images with draws filled from another packet or transform | 0 |
| eye frustum not the submitted field of view (or the other eye's) | 0 (worst tangent error 0.00005, the 16-bit rounding) |
| GPU uniform probe: sets read back, left / right range not what the fill wrote | 16679, **0 / 0** |
| XRP lines `same=1`, `mL=0 mR=0` | 17471 of 17471 |
| releases changed after the GPU passed them | 0 of 42854 |
| stamps for their poses' locate; end times exact | 34960 of 34964 (4 unstamped); 21485 of 21485 |
| mock errors; end gate (ends > 1 ms after the wake-up) | 0; 0.2 % (gate 2 %) |
| pacing | 120 sets/s, late 20 of 21480, end_ms p50 0.14, gpu 4.25 ms p50 |

The first instrumented run (d2c67ba, same 3 minutes) gave the same: 17459 sets, 0 with different packets,
0 of 34944 eye images off the submitted pose. Validation layer, 100 s with stamps and the probe: 0 messages,
0 mock errors; ctest 32 of 32.

Timing against the baseline (f20f221, `wt/native`), interleaved A B A B, 90 s each, with stamps: the
first version of the larger stamp made "ends later than 1 ms" 2.3 to 3.1 % against 0.7 to 1.6 %; without
stamps the two builds were equal (0.5 / 0.4 % against 0.4 / 0.3 %), so it was the test stamp, not the
instrumentation. The stamp is now one pixel a bit, and xrmock does its read-back bookkeeping before the
sleep in xrWaitFrame rather than after the wake-up it measures from: 0.5 / 0.5 % (baseline) against
0.5 / 0.4 % (lane P), end p50 0.22-0.23 against 0.20-0.22 ms.

**So the hypothesis does not hold in the app, at least at the mock's timing**: both eyes of every set were
filled from the set's one packet copy, with exactly the pose and field of view the layer submitted, and the
uniform bytes the GPU held after the eyes were the bytes written. By construction (one copy per set taken
under the mutex) a later publish cannot reach a set in progress at any timing; the headset launch below
checks that with the same counters on the headset's own timing.

Inside the app, what else could place one eye differently, and where it stands:
- per-eye projection matrices / the asymmetric field of view on the wrong eye on alternate sets: measured,
  0 (each eye's frustum is the submitted fov of that eye, never the other's);
- the replay's per-eye uniform ranges swapped or reused: lane L's counters (drawn with the recorded
  uniform 0, with a range outside the set's fill 0) and the GPU probe (0 of 16679 sets, both eyes);
- viewport: each eye renders its own full 1728x1728 target, the same every set (static; cannot alternate
  with head motion);
- the scene state (interpolation): lane L, identical in both eyes;
- the eye transforms swapped left/right: would be a fixed 64 mm stereo error on every set, not a head-turn
  zigzag (not stamped: the mock's eyes share one orientation, so only the frustum check would see a swap).

## The launch

Install `01-eye-poses.env` into `$KARTFRAME_HOME/session.queue/`, build: this branch installed with
`install --dev`. One launch, recorded, every set logged.

**Routine:** menus, then straight into a Time Trial. During the countdown (kart standing still) look at a
track-side edge and turn the head slowly left and right for about ten seconds, hold still five; then drive
and do the same for ten seconds, hold still five. Say: doubled or not, which eye, standing and driving.

What the logs must show:
- game.log, every 10 s in the race: `VR eye poses (10 s): ~1200 sets, eyes from different packets 0, an
  eye not from the set's packet 0, an eye filled with more than one transform 0` and `VR eye uniform probe
  (10 s): ~1200 sets read back ..., left ... 0, right eye's 0 (0 sets not probed ...)`.
- Every `XRS ... XRP` line: `same=1`, `tL=tR=` the line's `t=`, `kL=kR=`, `mL=0 mR=0`; `yL` and `yR` the
  two eyes' yaw (they differ by the eyes' fixed canting, the same every set; on the Frame also by the gaze
  offsets lane M logged), stepping smoothly from set to set through head turns.
- Pacing as session 10 (`mode=pipelined`, `end_ms` p50 about 0.2, late near 0).

What it decides:
- Any non-zero count in either line (or a `same=0` / `mL`,`mR` above 0 line) on the headset: the
  hypothesis holds under the headset's timing after all (the mock did not reach it); the XRP lines name
  the sets, which line up with the recording's refreshes through the XRF lines (H7 section 1's alignment).
- All zero, yet still doubled: each eye was rendered with the set's own head pose, the same pose both
  eyes' layer views were submitted with (the mock's pose check), and the uniform bytes the GPU held
  after the eyes were the ones written. Then the doubling is not in what the app renders per eye.
- Zigzag analysis of the recording as before (`frame/perf/panels/analyse.sh <session>` and H7 section 1's
  table: zigzag refreshes per eye, race against menus). With the XRP lines, take each set's `yR - yL` and
  the set-to-set step of `yL`: on the right eye's zigzag refreshes these must be as smooth as on the
  others (no ±1-refresh alternation in the app's poses), while the panel image alternates. That puts
  the alternation after the hand-over to SteamVR, where H9's remaining candidates are (the order of
  calls, GalaxyFrame's first-write barrier, the quad layers, the eye size).
