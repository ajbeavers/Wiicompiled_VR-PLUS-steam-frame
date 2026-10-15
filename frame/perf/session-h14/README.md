# Headset session 14 (lane U): the desktop present and the queue at SteamVR's submissions

Lane U (`frame/perf/LANE_U.md`) proved headless that every set is released only after the GPU finished
writing it (Dawn's render submission precedes the timeline signal; a new strict mock check: 0 of about
143000 released images changed between release and next acquire, positive control 97 %), and that at the
release our queue is nearly empty (a submission made there completes in 0.3-0.45 ms p50), as in session 13's
clean xrprobe replay. What the live game has and the probe and every mock run lack: a desktop window
swapchain (Gamescope WSI, Mailbox) presented once per game frame, 60 Hz = every second refresh, on the
same VkQueue as the eyes and SteamVR's client. Two launches test it from both sides.

| launch | env file | change | logs to read |
| --- | --- | --- | --- |
| 1 | `01-no-desktop.env` | `KARTFRAME_HEADLESS=1`: no desktop swapchain, no presents (as in every mock run) | panels, XRF lines, vr pacing |
| 2 | `02-release-idle.env` | `KARTFRAME_RELEASE=idle`: desktop as usual; vkQueueWaitIdle on Dawn's queue before the releases and before xrEndFrame | plus `release idle drain` every 10 s: how much of ours was still queued at those points on the headset |

Both: `KARTFRAME_FRAME_LOG=1`, `KARTFRAME_RECORD=raw` (panels.mkv), `KARTFRAME_SESSION_ENV_ONCE=1`. Session 12
launch 1 (`frame/perf/session-h12/01-measure.env`) is the control if one is wanted (doubled in all 13
sessions so far).

## Install

Build lane U's branch, `install --dev` (the switch is in the binary; launch 1 needs nothing new). Copy both
env files into `$KARTFRAME_HOME/session.queue/` (default
`~/.local/share/KartFrame/session.queue/`); the wrapper takes one per launch, in name order.

Owner routine, each launch: menus, then straight into a Time Trial. During the countdown (kart standing) look at
a track-side edge and turn the head slowly left and right for ten seconds, then the same while driving, then
each with one eye closed. Say: doubled or not, which eye.

Launch 1 note: the desktop window stays empty (never configured). If Steam or gamescope objects to a window
that never presents (no image in the headset, a stuck launch), end the launch and report it; launch 2 does
not depend on it.

## Reading it

- `frame/tools/session-report.py <session>` (pacing, late, GPU ms), `frame/perf/panels/analyse.sh <session>`
  and H7's zigzag rule per eye on the race (session-h12's zig2.py); menus-like floor 4-6 %.
- **Launch 1 clean, launch 2 doubled**: the desktop present is it (and not by queue backlog at the
  releases). Fix: no desktop swapchain in VR (make KARTFRAME_HEADLESS's path the VR default, or stop presenting
  while the XR session runs). Launch 2's drain line then says whether the present at least delayed our queue.
- **Launch 2 clean** (either way for launch 1): a backlog on our queue at SteamVR's release or end submissions,
  which the mock does not have; launch 2's drain line gives its size (mean / max / >1 ms / >4 ms per 10 s;
  the mock: 0.3-0.4 ms before the releases, 0.1 before the end, that is the call's own round trip). Next:
  find what is queued there (the drain size by frame parity) and order it after the release cheaply.
- **Both doubled**: neither the present nor our queue at SteamVR's submissions. Next candidate (LANE_U.md,
  rank 3): GPU contention with SteamVR's compositor from the game's 3-5 ms a set, heavy and light sets
  alternating at 60 Hz; test with a render-scale-0.5 measurement launch or an xrprobe replay with an added
  60 Hz GPU load.
- Pacing in launch 2: expect xrEndFrame later than 1 ms after the wake-up in about 10-13 % of frames (the
  drain's round trip; mock), no late sets. Late sets above a few percent make launch 2 unreadable.

## Headset results (2026-10-15, orchestrator's reading)

Owner: launch 1 (no desktop) "bad, right eye in particular doubling"; launch 2 (release idle) "bad too, but
the left eye didn't have doubling, just a lag spike every 3-4 seconds; the right eye was bad".

Logs: launch 1 ran headless (aurora "headless: N frames presented to the offscreen target"; no desktop
swapchain), race 119.6 sets/s, 0.7 % late, GPU 5.3 ms p50, ends > 1 ms 3.3 %: the desktop present is NOT
the cause. Launch 2: the drain before the releases took 4-4.9 ms mean (> 4 ms in ~85 % of releases: the
set's own render, not a backlog), before the end 0.4-1.3 ms; cost: 36.8 % late sets, 36 % retained repeats,
ends > 1 ms 91 %, 76.6 sets/s (the lag spikes). SteamVR's compositor timing (vrtiming) is identical between
the doubled launch 1 and session 13's clean probe case 4: 120 frames/s, 0 reused/dropped, compositor GPU
0.85 vs 0.77 ms, vsyncs to photons 1.00.

Reading: candidates 1 (desktop present) and 2 (backlog at release) are out. What remains is candidate 3,
our GPU work (3-5 ms per set, heavy and light sets alternating at 60 Hz) against the compositor's per-eye
compose inside the refresh; launch 2 shifted our render by ~4 ms and cleared the LEFT eye only, which fits
a phase collision that hits one eye's compose window. Session 15 tests the load from both sides: 90 Hz at
1728 (11.1 ms refresh) and 120 Hz at 1200 px (half the work, measurement only).
