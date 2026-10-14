# Headset session 11 (lane Q): the projection layer alone, and GalaxyFrame's first-write barrier

Replaces lane P's plan (its eye-pose instrumentation stays in the build: with `KARTFRAME_FRAME_LOG=1` the
XRS/XRP lines and the "VR eye poses (10 s)" / "VR eye uniform probe (10 s)" summaries are still logged and
must still read 0).

Inside the app everything is proven correct headless (lane P: both eyes from the same packet and pose;
lanes before: pacing, finished releases, exact stamps and end times), and the hand-over was made
GalaxyFrame's way (format, usage, end locate, extensions; H9.md) without effect. Two candidates are left:

- **A, the layer list.** KartFrame's menus (a projection layer, the menu environment, plus quad layers:
  screen, panel) are clean; its races (a projection layer alone) double. GalaxyFrame always submits its
  projection with quads (HUD, pointer) and is clean; PrimeFrame submits a projection alone and doubles.
  If SteamVR on the Frame takes a frame whose only layer is a projection on another path (a per-eye
  direct path with its own timing), the right eye could suffer there.
  `KARTFRAME_RACE_QUAD=1` adds one more layer to every such frame: a 16x16 fully transparent quad
  (alpha 0, `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`), in the app space 2 m ahead, 1 cm across,
  after the projection. Its three images are cleared once (acquired, cleared, the GPU waited for, released);
  every frame then shows the last released one. Invisible.
- **B, the first write.** GalaxyFrame's first write to an eye image after xrWaitSwapchainImage waits on
  ALL prior commands and memory writes (SteamVR's client's acquire work included); Dawn's tracked barrier
  waits on colour-attachment output only. `KARTFRAME_FIRST_WRITE_BARRIER=all` submits that barrier
  (src ALL_COMMANDS / MEMORY_WRITE, dst ALL_COMMANDS / MEMORY_READ|MEMORY_WRITE, layout
  COLOR_ATTACHMENT_OPTIMAL kept, queue families ignored) on every image of a set (eyes; in menus the screen,
  the environment, the panel) right after their waits, on Dawn's queue under its lock, before Aurora's set.
  No Dawn change (a barrier's first scope is everything earlier in submission order on the queue).

Build defaults: `KARTFRAME_RACE_QUAD=1`, `KARTFRAME_FIRST_WRITE_BARRIER=all` (both on). Other values:
`KARTFRAME_RACE_QUAD=0` (none), `=cycle` (also acquire, wait and release one quad image every frame, no
GPU work: GalaxyFrame's per-frame pattern); `KARTFRAME_FIRST_WRITE_BARRIER=dawn` (Dawn's barrier alone).
Each launch below switches exactly one of the two on.

## Install

Build: lane Q's branch, installed with `install --dev`. Put both `01-race-quad.env` and `02-barrier.env`
into `$KARTFRAME_HOME/session.queue/`; the wrapper takes them one launch each, in name order. Both record
the panels (`KARTFRAME_RECORD=raw`) and log every frame (`KARTFRAME_FRAME_LOG=1`).

**Routine, each launch:** menus, then straight into a Time Trial. During the countdown (kart standing
still) look at a track-side edge and turn the head slowly left and right for about ten seconds, hold still
five; then drive and do the same for ten seconds, hold still five. Say: doubled or not, which eye,
standing still and while driving, and whether the menus are still clean.

## Launch 1: `01-race-quad.env` (race quad on, Dawn's barrier)

What the owner should see: nothing new (the quad is transparent and 1 cm at 2 m). If hypothesis A holds,
the race is clean like the menus.

game.log:
- `OpenXR Vulkan swapchains ready: ... first-write barrier dawn (KARTFRAME_FIRST_WRITE_BARRIER), race quad
  on (KARTFRAME_RACE_QUAD), ...`
- once, at the first projection-only frame (the first-run guide, a loading screen or the race start):
  `OpenXR race quad layer ready: 16x16 VkFormat 43, 3 images cleared to transparent in <about 1.5> ms; a
  1 cm quad 2 m ahead after every projection-only layer (KARTFRAME_RACE_QUAD=1)`. No `race quad could not
  be made` warning (with it, the frames went out with the projection alone and the launch tests nothing).
- XRF lines end in `n=<layers>`: `n=2` on every race frame (projection + quad); menu frames `n=2` (environment
  + screen) or `n=3` (with the panel). No `n=1` once the race quad is ready.
- pacing as session 10 (`mode=pipelined`, `end_ms` p50 about 0.2, late near 0); lane P's counters 0.

`~/.local/share/Steam/logs/xrclient_kartframe.txt`:
- one more swapchain: `CreateSwapchain` with format 43 and 16x16, and its `Created remote Vulkan image ...,
  dimensions 16 x 16, format 43` lines (three images), after the eye swapchains and menu environment.
- if SteamVR logs layers per frame, a quad beside the projection on race frames. It may turn the quad into an
  overlay as it did the menus' screen (`VRMsg_CompositorNewSharedImage`, `openxr_quadlayer_<n>`, H9.md
  section 1); note whether it does: GalaxyFrame's quads were not overlays.

## Launch 2: `02-barrier.env` (no race quad, GalaxyFrame's barrier)

What the owner should see: the race as in sessions 1 to 10 (projection alone), unless hypothesis B holds.

game.log:
- `... first-write barrier all (KARTFRAME_FIRST_WRITE_BARRIER), race quad off (KARTFRAME_RACE_QUAD), ...`;
  no `first-write barrier could not be submitted` warning; no `race quad layer ready` line.
- XRF lines: `n=1` on every race frame.
- pacing as launch 1 (the barrier is one small submission per set; mock: no timing change).

`xrclient_kartframe.txt`: no 16x16 swapchain; otherwise as session 10.

## What it decides

- Launch 1 clean, launch 2 doubled: the projection layer alone is the cause (A). Keep the race quad on
  (default) and tell PrimeFrame (it submits a projection alone and doubles too).
- Launch 2 clean, launch 1 doubled: the first write ordered only after colour-attachment output was the
  cause (B). Keep the barrier on (default) and consider making it Dawn's own barrier.
- Both clean: either one is enough; both defaults stay on; a later session can tell them apart with
  `KARTFRAME_RACE_QUAD=0 KARTFRAME_FIRST_WRITE_BARRIER=dawn` as the control.
- Both doubled: neither. What is left from H9.md: the order of the calls (acquire and render the next set
  before releasing and ending, GalaxyFrame's order) and the eye size (render_scale 0.9 measured for the
  record only). Panel analysis as before: `frame/perf/panels/analyse.sh <session>` per launch, H7 section
  1's zigzag table (zigzag refreshes per eye, race against menus).

## Mock results (lane Q, build 6596f27)

`frame/perf/hudrace.sh q-race 180 MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KARTFRAME_FRAME_LOG=1
KF_MOCKRUN_END_GATE=2` (scripted Grand Prix, 120 Hz, defaults: race quad on, barrier all):

| check | result |
| --- | --- |
| mock errors | 0 |
| releases changed after the GPU passed them | 0 of 42792 |
| stamps for their poses' locate | 33144 of 33148 (4 unstamped), 0 for another locate, 0 with the other eye's |
| end times exact | 21481 of 21481 |
| ends later than 1 ms after the wake-up (gate 2 %) | 0.3 % |
| layer lists per xrEndFrame | PQ 16574 (race: projection + quad), EQ 4867 (menus: environment + screen), none 40; P alone 0; quads on a never-released swapchain 0 |
| XRF lines | 21441, all `n=2` |
| race quad made | 16x16 VkFormat 43, usage 0x31, 3 images cleared in 1.4 ms |
| lane P's counters | eye poses 0, uniform probe 0 / 0 |
| pacing | late 32 of 21363 ticks, end_ms p50 0.15, gpu 4.08 ms p50 |

Validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, 80 s, menus and race with stamps, the
barrier on every set and the quad's clears): 0 messages (best-practices warnings in a check run showed the
layer's output reaches game.log), 0 mock errors. ctest 32 of 32. Eye images at 65 and 75 s as before.

Race timing, interleaved, 90 s each, `MOCK_XR_BUSY_MS=1`, no stamps (old = `KARTFRAME_RACE_QUAD=0
KARTFRAME_FIRST_WRITE_BARRIER=dawn`, the behaviour before this lane; new = the defaults):

| run | errors | end p50 / p99 ms | ends > 1 ms | late sets | gpu p50 / p99 ms | layer lists |
| --- | --- | --- | --- | --- | --- | --- |
| old 1 | 0 | 0.17 / 0.65 | 0.4 % | 21 | 4.06 / 5.03 | P 6683, EQ 3960 |
| new 1 | 0 | 0.18 / 0.64 | 0.3 % | 23 | 4.02 / 5.03 | PQ 6685, EQ 3958 |
| old 2 | 0 | 0.18 / 0.70 | 0.5 % | 24 | 3.97 / 5.00 | P 6697, EQ 3954 |
| new 2 | 0 | 0.17 / 0.70 | 0.4 % | 27 | 4.04 / 5.08 | PQ 6680, EQ 3960 |

No timing change. Scratch: `$HOME/.cache/kf-laneQ/runs/`.
