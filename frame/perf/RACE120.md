# RACE120 (lane I): fitting the race into 120 Hz at 1728 per eye

All numbers: mock runtime (xrmock), real Turnip GPU, 1728x1728 per eye, 120 Hz, pipelined, TU_DEBUG=sysmem,
`hud_virtual_screen = true`, both build locks. SteamVR's own vrserver/vrcompositor were running (idle,
no app) during the A/B pairs and the soaks, as they always are beside the game in the headset; the SoC
ran at 92-95 C with the GPU at its top clock (880-903 MHz, devfreq). GPU ms are per eye set.

## How it was measured

- `KARTFRAME_FPSLOG=2` now times every set, not only the game frame's encode: a "game-frame set" (the
  game frame's native render and its uploads) and each "refresh set" (both eyes, the panel, the copy into
  the swapchain). Render passes carry pass timestamps; inside the eye passes Dawn's experimental in-pass
  timestamps (requested only with FPSLOG=2) split them by draw class: world (scene), 2D on the virtual
  screen (hud2d), full-view fades (fade), framebuffer effects (efbfx), clears; plus the uniform upload and
  the sink (the eyes' way into the swapchain images). `frame/perf/gpuprof.py RUN [FIRST_S LAST_S]` makes
  the table (ms per set, averaged over sets). The marks cost a little; the absolute times of
  FPSLOG=2 runs are slightly high.
- The demo races pick their course at random, so two demo runs are not an A/B. `frame/perf/abgp.sh` runs
  A and B interleaved (A B A B) on the scripted Grand Prix (frame/perf/hudrace.sh: Luigi Circuit, the same
  every time), each with the GPU clock and the hottest thermal zone sampled (run.sh's gpufreq.tsv).
- Run-to-run and state-to-state spread is large: the same build's refresh set measured 4.7 ms on a cooler
  machine early on and 7.1 ms at 93 C with the compositor busy. Only interleaved pairs count.

## Where a race set's GPU time went (before)

Demo, first 1-player race (t = 100-145 s), build `native` 2fe42c4 + the profiler only (run P0, cooler
machine, ms per set):

| set kind | sets | native (mono) | efb copies | snapshots | eye load+clear | scene | 2D on screen | full-view fade | upload | sink (copy) | sum |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| game-frame set (60 Hz) | 2701 | 0.93 | 0.04 | 0.03 | - | 0.01 | - | - | - | - | 1.07 |
| refresh set (120 Hz) | 5399 | - | - | - | 0.27 | 2.55 | 0.16 | 0.30 | 0.03 | 1.38 | 4.73 |

Hot machine, compositor running, scripted GP (AB4-A1, the copy as shipped): refresh set 7.16 ms = scene
3.67, sink 2.35, fade 0.37, 2D 0.28, load 0.40; game-frame set native 0.16 (cut already in) / 1.31 (AB5-A1,
whole native render). 21 % of race sets late.

Reading: the 2D layer on the screen is 56 to 58 draws a frame in these races (not ~400; that count was
menus) and costs 0.16 to 0.28 ms of a set. The copy into the swapchain, which nobody had timed, was the
largest item after the world itself. The native render drew the whole scene every game frame.

## Changes, each measured

| change | evidence (interleaved pairs, same clock/temperature) | saving |
| --- | --- | --- |
| Eyes reach the swapchain through an sRGB-decoding draw into an sRGB view; the swapchain images lose MUTABLE_FORMAT, so the driver may compress them (`KARTFRAME_SINK=copy` restores the copy) | AB4: sink 2.35 / 2.28 -> 0.47 / 0.48 ms; refresh set 7.16 / 7.08 -> 5.14 / 5.10; race late 21.3 / 19.4 % -> 1.6 / 1.6 %; sets/s 94.8 / 97.0 -> 118.4 / 118.5 | ~1.9 ms every refresh set |
| Native render of an immersive race frame (Linux too) stops after the last copy the eyes sample, clipped to the copies' rectangles (here a 6x4 depth copy at the sun for the lens flare, after the whole scene); the desktop window shows black in races (`KARTFRAME_MONO_FULL=1`, `KARTFRAME_NATIVE_CLIP=0`) | AB5 pair 1: native 1.31 -> 0.17 ms a game frame, race late 4.7 -> 2.5 % (pair 2's A run was stopped by the headset marker) | ~1.1 ms a game frame (every second set) |
| Eyes rendered straight into the swapchain images (no copy), chosen per session by measuring both with GPU timestamps on the first race | AB3 (copy into MUTABLE images): direct 7.20 / 7.02 vs copy 7.24 / 7.12 ms: the copy saved 2.2 ms but every eye pass got slower on the uncompressed image (load +0.9, fade +0.55, scene +0.5). With the sRGB sink the images are not mutable and direct is impossible; the trial never applies | none (kept for KARTFRAME_SINK=copy/blit) |
| HUD as a quad layer (task 2a) | not done: the 2D layer costs 0.16-0.28 ms a set; a quad layer would add its own copy every set and give up the per-eye clip/depth behaviour | - |
| Framebuffer effects per eye (task 2b) | efbfx is 0.00 in all the 1p races measured (MKW's race passes have no EFB-sampling 2D effect in the eye); nothing to cut | - |
| loadOp/storeOp (task 2c) | eyes are one render pass each (eye plan), clear+load segment 0.12-0.20 ms per eye: the clears of colour and depth/stencil, which are needed | - |

Full-view fade: one untextured blended quad a frame (0.27-0.39 ms a set) stays; lane I's 90 Hz fix
(H4.md) moved every textured full-frame quad off that path.

## After

Soak, 10 minutes at 120 Hz through the demo cycle, MOCK_XR_BUSY_MS=1, MOCK_XR_STAMP=1, compositor
running, GPU 890 MHz average, SoC up to 92 C (run SOAK120, lane/I 66da38a):

| part | s | sets/s | late % | GPU p50 / p99 | encode p50 / p99 | game fps |
| --- | --- | --- | --- | --- | --- | --- |
| menus | 428 | 120.4 | 0.02 | 2.45 / 3.05 | 0.58 / 1.19 | 60.0 |
| race 1p (2 races) | 84 | 119.6 | 0.81 | 4.52 / 6.56 | 2.36 / 3.30 | 59.3 |
| race 2p | 43 | 120.6 | 0.00 | 4.07 / 5.01 | 2.29 / 3.14 | 58.7 |
| race 4p (30 fps logic) | 42 | 115.9 | 3.83 | 5.32 / 6.53 | 1.96 / 3.19 | 29.7 |

Segments of the 1p race refresh set after: scene 2.95, sink 0.59, fade 0.27, 2D 0.17, load 0.25, upload
0.03 = 4.32 ms; game-frame set 0.64 (native 0.50: this soak's courses have a half-size scene copy for
the eyes, which needs the whole scene). Mock: 0 errors; 143278 releases checked, 0 unfinished; 40224
stamped eye images, all rendered for their poses' locate; end times 0 off.

Same soak at 90 Hz (MOCK_XR_HZ=90, KARTFRAME_REFRESH_HZ=90; GPU 674 MHz average, up to 90 C; SOAK90):
menus 90.5 sets/s, 0.02 % late; race 1p 90.5, 0.01 %, GPU 4.16 / 7.13; race 2p 90.6, 0 %; race 4p 90.6,
0 %. Mock: 0 errors, 107876 releases all finished, 30174 stamped eye images all for their poses' locate.

Menus are unchanged (the screen and environment images are still copied: 1.86 ms of a 2.47 ms set).

## The two stamp errors of the orchestrator's soak

Trace: `ERROR stamp: eye 0/1 image rendered for a display time 0.24 ms before the one its submitted poses
were located for`, one set. Not a pairing error: the game's DisplayTimeNanos turned the XrTime into
steady-clock ns as a delta between two clock reads (steady_clock, then CLOCK_MONOTONIC after the
conversion call); a preemption between them shifted that set's stamp (and its interpolation time) by
0.24 ms. On Linux the converted CLOCK_MONOTONIC time is steady_clock's own: it is returned as is now.
The mock filed an offset of less than a refresh (n = 0) under ">=+2"; it is "between" now.

## Switches

KARTFRAME_SINK=srgb (default) | copy | blit; KARTFRAME_DIRECT_EYES=0/1 (forces the direct trial's
outcome, copy/blit sinks only); KARTFRAME_MONO_FULL=1; KARTFRAME_NATIVE_CLIP=0; KARTFRAME_FPSLOG=2
(segment profile); MOCK_XR_MUTABLE=0 (mock: images without MUTABLE_FORMAT even when asked).
