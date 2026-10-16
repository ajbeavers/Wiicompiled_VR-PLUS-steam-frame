# Lane W: the compositor's phase, and a lock against it

Sessions 16 to 18: ending the frame about 4 ms after xrWaitFrame's wake-up was clean once (session 16 launch 3)
and doubled in the right eye on repeat (session 18 launch 1), with identical app logs. Working model: SteamVR's
compositor takes each eye's image at its own point in the refresh (the right eye later), and the relation between
the app's xrWaitFrame grid and those points differs per launch and drifts. This lane makes that relation
observable per frame and designs the lock. The lock itself waits for session 19's data (below: why).

## What is logged now

- **vrframes.log** (session folder; `KARTFRAME_VRTIMING_FRAMES=1`, written by frame/tools/vrtiming.py in its own
  process, 4 polls a second, read-only background OpenVR app as before). One `F` line per compositor frame:
  index, `m_flSystemTimeInSeconds` (openvr.h: "absolute time reference ... aligns with the vsync that running
  start is relative to"), presents, mispresented, dropped, reprojection flags, then, in ms after that system
  time (openvr.h: "relative to this frame's SystemTimeInSeconds"), WaitGetPoses called, new poses ready, new
  frame ready, compositor update start, update end, render start; then present / wait-for-present / idle CPU ms,
  compositor GPU ms, client interval. SteamVR keeps 128 frames (1.07 s at 120 Hz); each poll writes the frames it
  has not written, except the newest two (they may still be filling in); `G` lines name any frames missed.
  One `P` line per poll: CLOCK_MONOTONIC, _RAW and BOOTTIME read around `IVRSystem::GetTimeSinceLastVsync`, its
  seconds-since-vsync and counter, and the newest frame's index and system time.
- **XRF lines** (game.log, `KARTFRAME_FRAME_LOG=1`) end in `m=` (the wake-up, CLOCK_MONOTONIC ms; steady_clock is
  CLOCK_MONOTONIC on Linux), `r=` (eye images released, ms after the wake-up, -1 on a retained repeat) and `t=`
  (the frame's predicted display time, CLOCK_MONOTONIC ms through `xrConvertTimeToTimespecTimeKHR`). The end call
  is `m + e`. One `XRF clock:` line says how t= was converted and XrTime minus CLOCK_MONOTONIC.

## How the clocks are matched

1. Without any matching (view A of phase.py): SteamVR measures the app itself. The existing vrtiming summaries
   already show it for OpenXR apps: "poses -> frame ready" per second equals our end delay (session 16 launch 3:
   4.60 ms, session 18: 4.53 / 5.00 / 4.21 ms at 4000 / 4500 / 3500 us, session 17 launch 2: 6.52 at 6000). So
   `posesready` is our wake-up and `frameready` our xrEndFrame, both in SteamVR's own frame-relative ms; the
   compositor's update and render start are in the same frame. This alone gives the phase.
2. With matching (view B): `GetTimeSinceLastVsync` returns seconds since the last vsync and a counter; CLOCK_MONOTONIC
   read around the call (the call is a few us) gives the vsync in CLOCK_MONOTONIC; if the counter is the F frame
   index, `offset = vsync_mono - sys` exactly, else modulo one refresh (all a phase needs). phase.py prints the
   offset, its spread and drift (0 means SteamVR's system time is CLOCK_MONOTONIC itself; a constant means the
   same clock with another zero; a drift means _RAW, and the P lines carry _RAW to confirm). Check: XRF end
   minus the nearest frame-ready should be about 0 (plus vrclient's submit time).
   Headless: no compositor in the mock, so only the format is proven: frame/tools/openvr-stub.c (a fake
   libopenvr_api.so, system time = CLOCK_MONOTONIC - 1000 s) through `KARTFRAME_STEAMVR_DIR`, beside a mock run.

## Reading phase.py

`python3 -I frame/tools/phase.py SESSION_DIR [...]`, per launch (race only unless `--all`):
- A: p5/p50/p95 of poses ready, frame ready, update start / end, render start (ms after the vsync reference), and
  the **margin** = update start minus frame ready (how long before the compositor's update our frame was ready),
  with histograms of frame ready and of the margin.
- B: wake-up, end, release and display time minus the nearest vsync, update start and render start (signed,
  +-period/2), histograms for the end against each anchor and the wake-up against the vsync.
- Drift table (`--every S`): circular means of end-vsync, end-update, end-render, wake-vsync, margin.
A clean launch and a doubled one at the same delay must differ in some of these; the model predicts: the same
end-minus-wake-up (e) but a different wake-up-vs-vsync, so a different end-vs-update/render start, with the
doubled launch's end piled near one anchor (or a margin near 0). If nothing differs, the per-eye pick-up is not
tied to anything GetFrameTimings reports, and the next lever is the panel recording against vrframes.log.

## The lock (design; build it after session 19)

The game times its end against the compositor's clock instead of its own wake-up: end at
`anchor + phase_target`, where anchor is the compositor frame's vsync reference (or update start), extrapolated
from the newest known one by whole periods, and phase_target is the value session 19 shows clean (e.g. "1.5 ms
before update start"), bounded to [wake-up + 0.5 ms, wake-up + period - pacing margin]. Where the grid comes
from, in order of preference:

1. **From xrWaitFrame's display time, in process, no helper.** If session 19 shows `display time minus the
   nearest vsync` (B) constant across launches (clean and doubled alike), the compositor's grid is the display
   time minus a constant, and `end = t - K` replaces `wake + D`. One line in the pacing thread, nothing to fail.
   The h18 logs argue against it a little (locate-to-display offsets equal while the outcome differed), so this
   is the case to rule in or out first.
2. **Published by the helper (the expected case).** vrtiming.py (already polling, own process) writes a 64-byte
   record into a file in `$XDG_RUNTIME_DIR` (mmap, seqlock: seq odd while writing): the latest vsync in
   CLOCK_MONOTONIC ns (via the P-line offset), the period in ns (from 128 frames: sub-us), the medians of update
   start, render start and frame ready over the last second, and the write time. The game mmaps it once and reads
   it every frame (no syscall), uses it only when fresh (written < 1 s ago, period within 1 % of
   xrWaitFrame's), else falls back to the fixed `KARTFRAME_END_DELAY_US`. Switch: `KARTFRAME_END_LOCK=<anchor>:<us>`
   (vsync|update|render, signed offset), logged per XRF line as the chosen target.
3. **OpenVR in process: no.** The OpenXR runtime already runs SteamVR's vrclient in our process; a second
   `VR_Init` there shares its globals, and `VR_Shutdown` could take down the OpenXR session's connection.

Risks: (a) the eye pick-up points are not among the reported fields (the compositor may take the eyes at fixed
offsets after render start that only the recording shows); then phase_target is found by a sweep of offsets
against the compositor's anchor instead of the wake-up (one session, 3 values, each repeated) and the lock still
removes the per-launch variation; (b) SteamVR re-phases its running start after a hitch: the helper follows
within a poll (0.25 s; 0.1 s when the lock is on), a few hundred ms of doubling at worst; (c) sleep precision
(about 0.05-0.1 ms here) against a clean band that sessions 16-18 suggest is about 1 ms wide; (d) the end moves
later inside the refresh: no change to the display time or the poses (the set is located two refreshes ahead
either way), only less slack before the compositor, so late ends must stay counted (end_late=); (e) the helper
dies or SteamVR restarts: stale record, fixed-delay fallback, logged once.

Session 19 is needed first: it decides between 1 and 2, and which anchor and offset the lock aims at.
