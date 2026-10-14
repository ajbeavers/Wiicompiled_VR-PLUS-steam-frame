# PANELS (lane J): the doubled image on head turns, from a panel recording

Session `$KARTFRAME_HOME/sessions/20261014-010727` (build `native` 7daa878: 120 Hz, pipelined, sysmem,
scene clock, an eye set every refresh), recorded with `KARTFRAME_RECORD=raw` (framecorder `--view raw`:
both panels as scanned out, 1920x1080, HEVC). The owner turned the head slowly left and right in a race:
"double image still when moving my head"; head still was fine.

## The recording

- 17019 frames over 171.5 s (the container's 141.8 s is frame count / 120). Each frame's time is the panel
  vblank it was grabbed at (pts snap to the 8.33 ms grid within 0.3 ms), so consecutive refreshes are
  known. The recorder missed refreshes while the GPU was loaded: menus 2 % of gaps, race 22 to 28 %
  (framecorder: 90 to 100 fps, 3 ms of GPU a frame, headset GPU 93 %). Only pairs of consecutive
  refreshes are compared. The recorder's GPU load is itself a suspect for the race's 13 % late sets
  (session 20261013-232117, same build family, no recording: 6.3 %).
- Video time + about 5.9 s = the game's `vr pacing` t (race start, the t=36 hitch; framecorder started
  01:07:48, the game's session at about 01:07:42).

Tools (`frame/perf/panels/`, no numpy or PIL: ffmpeg decodes, `audioop` does the per-pixel work in C):
`analyse.sh SESSION_DIR` runs `extract.sh` (per-eye 480x270 grey), `lagdiff.py` (RMS difference of a
central region against the 8 refreshes before, gray levels; codec noise of an identical image is 1.5 to
4, a new image 10 to 60), `motion.py` (global image shift per refresh from column/row projections) and
`cadence.py` (the summary below). About 4 minutes for this recording.

## a. Cadence per panel

Each refresh of each eye is "new", "held" (the previous refresh again) or "stale": pixel for pixel an
image the same eye showed 2 to 8 refreshes earlier while the previous refresh differed.

| part | eye | refreshes | new | held | stale (lag: count) |
| --- | --- | --- | --- | --- | --- |
| menus (0-25 s) | L | 2821 | 75.0 % | 25.0 % | 0 |
| | R | 2821 | 75.7 % | 24.3 % | 0 |
| race (25-171 s) | L | 10661 | 85.4 % | 4.1 % | **10.4 %** (2:36 3:141 4:880 5:53) |
| | R | 10661 | 90.1 % | 2.9 % | **7.0 %** (2:55 3:643 4:17 5:17) |

- Holds in the race: 1 refresh 10054 / 10220 (L/R), 2 refreshes 158 / 125, 3: 8 / 1. Held is only
  visible while nothing moves on the panel (the compositor re-projects a held frame), so the retained
  repeats of late sets (11 % of ticks in the game log) mostly show as "new" here.
- Both eyes: out of step in **19.4 %** of race refreshes (one eye stale while the other is new: L stale
  1110, R stale 728); stale in both eyes at once 2 of 10661 (0.02 %). Menus: out of step 4.1 %, all of it
  held/new on static menu content, never stale.
- The stale rate does not depend on motion: L 10.6 / 10.2 / 10.3 % and R 7.1 / 6.8 / 7.2 % of refreshes
  with the image still / slow (0.5-2 px a refresh) / moving (2 px and more, up to 25 px). A stale image
  matches the earlier one **at zero shift** while the view turns: it appears where it appeared 3 to 5
  refreshes before, not where the world now is. The compositor did not re-project it from its own
  pose: it was shown with a newer frame's pose.

## b. Within one frame

No doubled edges inside one panel image (full-resolution crops of lamp posts, fences, the screen edge
during turns: single sharp edges; `evidence/withinframe_L_48.45s_fullres.png`). Nothing is composited
twice: the double is temporal.

## c. Across frames

During a turn, an eye's sequence of positions runs smoothly and then, for one or two refreshes, jumps
back to where the image was 3 to 5 refreshes (25 to 42 ms) before, and forward again. Example (L,
48.4 s, image moving 2-3 px a refresh): refreshes 5814 and 5815 are refreshes 5810 and 5811 again
(RMS 3.1 and 2.7 against 16 to 30 for neighbours; `evidence/stale_event_48.4s_L5814-5815_shows_5810-5811.png`).
`motion.py`'s per-refresh shift alternates sign far more in the race than in menus (fraction of
refreshes whose acceleration reverses: menus 0 to 0.08, race L up to 0.25, R up to 0.52 a second).
Seen with a moving eye, an image that jumps back by 3 to 5 refreshes of head motion at about 10 % of
refreshes, in one eye at a time, is a second image of everything: the doubling.

## d. The virtual screen and the world

They move together: a stale image is the whole eye, screen included. The race timer on the screen
shows it frame by frame (L, 60.00-60.30 s): 1.178, 1.178, 1.197, 1.206, **1.178**, 1.206, 1.232,
1.232, 1.244, **1.232**, **1.232**, 1.244, 1.273, 1.273, 1.296, **1.273**, **1.273**, 1.296 ...; the right eye
at the same refreshes shows other game frames (1.167, 1.147, 1.178, 1.197, 1.197, 1.178, 1.206, 1.197,
1.206, 1.260, 1.244, ...), at times two game frames from the left.

## e. Against the logs

- Per second, the stale rate stays at 9 to 11 % (L) through the race, uncorrelated with the game's late
  sets per second (r = -0.2 at any alignment from 2 to 9 s): late sets (repeats) are not the cause.
- What differs between menus (0 stale) and race (10 %): when frames are ended. SteamVR's "poses to frame
  ready" (vrtiming.log) is 0.16 to 0.22 ms in menus and 3 to 6 ms in the race (max 8 to 10): in menus the
  set finished during the previous refresh and was ended right at the wake-up; in the race the pipelined
  loop ends it whenever the GPU finishes, anywhere up to 7.3 ms into the refresh.
- SteamVR's own counters stay clean (reused 0.5 %, mispresented 1.2 a second): SteamVR shows a new frame
  nearly every refresh; it is the content of an eye that is old.

## Mechanism

Nothing in a set can make one eye pixel-identical to an image from 4 refreshes before: both eyes come
from one set, rendered and copied into their swapchain images in one command buffer from one locate.
The old images were images SteamVR still had in those swapchains. With two swapchain pairs alternating,
the pair a frame showed is acquired and written again one refresh later, while SteamVR, which predicts
each frame's display 3 refreshes (25 to 27 ms) after its wake-up, may not yet have composed that frame.
SteamVR evidently resolves each eye's image when it composes the eye (it also "shows a swapchain image
the moment it is released"), not when the frame was ended: depending on where in the refresh the next
acquire, render and release fall, an eye gets the image the frame was ended with, the swapchain's
newer one (2 refreshes ahead) or, while the newly acquired image is still being written, the one from
two uses before (4 refreshes behind, the stale images). Each eye is composed at its own moment, so the
eyes split. Ending sets at a fixed point right after the wake-up (menus here, GalaxyFrame always) keeps
whichever image SteamVR takes consistent, hence clean; ending them anywhere in the refresh (this race)
mixes them.

Candidates ruled out: PrimeFrame's 1-3 cadence (SteamVR's counters are clean, stale is not a cadence);
late sets shown a refresh after their poses' time and the retained repeats (those are 1-refresh holds,
3 to 4 % here, and the stale rate does not follow the late rate); interpolation time jitter (it cannot
make an image pixel-identical to an old one); two images composited in one frame (none seen).

## The mock reproduces it

`xrmock` (with `MOCK_XR_STAMP`) now logs every acquire, release (with the released image's stamp) and
xrEndFrame and at the end of the session replays a vsync grid against three models of when SteamVR
resolves an eye's image: `spec` (latched at xrEndFrame), `lazyrel` (the swapchain's newest released
image when the eye is composed) and `lazyacq` (its newest acquired image, which until its release still
holds its previous content); frames by display time, eyes composed 3 / 1 ms before the vsync
(`MOCK_XR_COMPOSITE_MS`). Lines "xrmock: compositor ..." and "xrmock: xrEndFrame after its xrWaitFrame
returned". The headset's end times need a loaded GPU: `KF_MOCKRUN_RENDER_SCALE=1.25` (measurement only)
puts the mock race at the headset's 4 to 5 % late and xrEndFrame 4 ms after the wake-up (median).

Scripted Grand Prix (`frame/perf/hudrace.sh NAME 90 MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 ...`), 7148 vsyncs:

| run | pairs | end after wake-up (<0.5 / 0.5-4 / >4 ms) | lazyacq: eye with its own poses L / R | shown again from 2-8 refreshes before L / R | eyes from different sets | lazyrel: again, split |
| --- | --- | --- | --- | --- | --- | --- |
| A2s (scale 1.25) | 2 | 4685 / 3039 / 2941 | 2.3 / 2.3 % (off -4: 1919 / 818, +2: 4216 / 5462) | **20.3 / 11.6 %** | **17.6 %** | 9.0 %, 17.6 % |
| B6s (scale 1.25) | 6 | 4777 / 3129 / 2469 | 100.0 / 100.0 % | 0 / 0 | 0.0 % | 0, 0.0 % |
| A2 (scale 1.0) | 2 | 9800 / 794 / 126 | 1.6 % (+2: 6110) | 0.2 / 0.2 % | 0.2 % | 0.2 %, 0.1 % |
| B6 (scale 1.0) | 6 | 9685 / 763 / 111 | 100.0 / 100.0 % | 0 / 0 | 0.0 % | 0, 0.0 % |

With two pairs and the headset's end times, `lazyacq` gives what the recording showed: one eye at a time
an image 4 (and 5) refreshes old, the left more than the right, eyes from different sets 17.6 % (recording
19.4 %). At the mock's own unloaded timing (ends at the wake-up) it is consistent (+2 refreshes, almost
no stale ones), like the menus and GalaxyFrame. `spec` never shows it, as the stamp check never did.

## The fix

`KARTFRAME_EYE_PAIRS` (default 6; 2..8): the per-refresh sets rotate through that many eye swapchain
pairs and a pair is acquired again only `pairs - 1` refreshes after the last frame that showed it (5:
beyond SteamVR's 3.3-refresh lead plus the refresh a frame stays on show). Under any of the three models
every eye then shows the image its frame's poses were rendered for. `=2` is the old alternation. A
refresh with no pair cooled off long enough renders nothing (`pairwait=` in the pacing line; 0 in every
run, the rotation never waits while a set is shown every refresh). The extra pairs are made with the
first per-refresh set (the render-first path, interpolation off, keeps two). Cost: 8 more 1728x1728
swapchains (about 290 MB with SteamVR's images), no GPU or CPU time (mock race: B6 116.1 sets/s, 0.4 %
late, GPU 4.61 ms; A2 118.8, 0.2 %, 4.60).

Checks, all runs: 0 errors, 19996 to 21258 releases compared after the GPU passed them and 0 changed,
13458 to 14030 stamped eye images all rendered for their submitted poses' locate, every xrEndFrame with
its predicted display time; ctest 32/32.

Also new: `vr pacing` lines end in `end_ms=<p50>/<p99> pairwait=<n>` (xrEndFrame after the wake-up);
`KARTFRAME_FRAME_LOG=1` writes one `XRF c= w= e= f= p= i= d= l=` line per frame (cycle, wake-up time,
end after it, fresh or retained, pair, released image index per eye, display step, display minus locate
in refreshes), to line a recording up with the frames; session-report.py shows the pairs and end times.

Not known: whether SteamVR is lazy in exactly this way (the mock is a model fitted to one recording);
whether 5 refreshes is enough margin on the headset (8 pairs is the next step); whether the recorder's
GPU load (3 ms a frame) is what pushed race end times to 3-6 ms in this session.

## Next headset session (three launches, under a minute each, each recorded)

Each launch: from the menus straight into a race (Time Trial or Grand Prix, any course). In the race,
look at a track-side edge (a post, the fence) and turn the head slowly left and right for about ten
seconds, then hold still for five, then once more. Say for each launch: doubled or not, which eye.

1. `session.env`: `KARTFRAME_EYE_PAIRS=2` `KARTFRAME_FRAME_LOG=1` `KARTFRAME_RECORD=raw`
   `KARTFRAME_SESSION_ENV_ONCE=1`. The old build's behaviour: doubled on turns, as before.
   Logs: game.log "OpenXR eye swapchain pairs: 2 (alternating ...)", XRF lines, `end_ms` p50 3 to 6 ms in
   the race. `frame/perf/panels/analyse.sh <session>`: race stale about 10 % (L) and 7 % (R), eyes out of
   step about 19 %, menus 0.
2. `session.env`: `KARTFRAME_EYE_PAIRS=6` `KARTFRAME_FRAME_LOG=1` `KARTFRAME_RECORD=raw`
   `KARTFRAME_SESSION_ENV_ONCE=1` (6 is the default; set to make the variable explicit). Expected: no
   doubling on turns, both eyes alike. Logs: "OpenXR eye swapchain pairs: 6 (a pair is written again 5
   refreshes ...)", `pairwait=0` every second, late and `end_ms` like launch 1. analyse.sh: stale under
   0.5 % in both eyes, eyes out of step only by held frames (under 3 %).
3. `session.env`: `KARTFRAME_EYE_PAIRS=8` `KARTFRAME_FRAME_LOG=1` `KARTFRAME_RECORD=raw`
   `KARTFRAME_SESSION_ENV_ONCE=1`. Only if launch 2 still shows stale images (some, fewer than launch 1):
   more margin. If launch 2 is clean, run launch 2's settings without `KARTFRAME_RECORD` instead, to see
   whether the recorder's GPU load is what makes the race 13 % late.

How it decides: launch 2 clean and its stale rate near 0 against launch 1's 10 % -> six pairs stay the
default, the mechanism is confirmed. Launch 2 stale at launch 1's rate -> SteamVR does not resolve images
this way: line the XRF lines up with the recording (the stale refreshes against `e=`, `p=`, `i=`) to see
which image it took. Launch 2 fewer but some -> launch 3 (8 pairs) tells whether the margin is short.
