# Headset session 13 (plan, not run): KartFrame's own race images through GalaxyFrame's loop (xrprobe replay)

Lane S (`frame/perf/LANE_S.md`) found the race eye images KartFrame renders smooth in both eyes, standing
and driving, with the head turning (H7's zigzag rule 0 % per eye; an injected period-2 jump is flagged in
99-100 % of moving sets). What is left is what SteamVR does with them. This session shows SteamVR the same
kind of images from a minimal loop that is known to be clean on this headset (GalaxyFrame's), and changes one
hand-over detail at a time.

## Recording (headless, before the session)

```
SECS=58 DUMP_AT=52.5,56.5 frame/perf/mockdump/dumprace.sh h13rec MOCK_XR_SWAY=0 MOCK_XR_DUMP_STEP=2 \
  "MOCK_XR_PRESS=/user/hand/right/input/a/click@12/1.5/0.15/46;/user/hand/right/input/a/click@54/1000/1000"
```

Head still (`MOCK_XR_SWAY=0`: every image rendered for yaw 0, eyes at x = -+32 mm, the Frame's own per-eye
fields of view, the mock's defaults), render scale 0.75 (the mock race holds 60 fps, scene stamps 0.25/0.75
as on the headset), 648 px per eye (every 2nd pixel), two windows of 240 consecutive sets: 52.5 s kart
standing, 56.5 s driving off. About 1 GB of PPMs plus index.txt (eye and display time per image). Check the
two windows' first images before the session. (The A hold starts at 54 s; look at the images to confirm.)

## xrprobe `--replay DIR` (to write; `frame/tools/xrprobe/xrprobe.c`, about 200 lines)

GalaxyFrame's order, as xrprobe already does: xrWaitFrame, xrBeginFrame, xrLocateViews (LOCAL space), render,
fence wait, release, xrEndFrame. New:

- Load index.txt, order each window's sets by display time, upload every eye image once (R8G8B8A8_SRGB 648x648,
  about 0.8 GB of images; or a 1 s loop if memory is short).
- Per frame: set k = round((predictedDisplayTime - t0) / period) mod N (a missed refresh skips a set, as the
  game does); vkCmdBlitImage (linear) each eye's image into the acquired 1728x1728 swapchain image; projection
  layer with the recorded pose (identity orientation, position (-+0.032, 0, 0)) and the recorded fov per eye,
  not the locate's. SteamVR then places the recorded race world-fixed in front of the startup direction and
  reprojects it as the head turns, as it does with the game's layers.
- A label strip with the case name and k (as `--sequence` draws), stdout "CASE n start".
- Options for the cases below: `--replay-window driving|standing`, `--replay-offset 1` (start one set later:
  the opposite 60 Hz content phase), `--replay-pairs 3` (KartFrame's rotation over three swapchain pairs,
  acquired two refreshes ahead), `--replay-quad` (KartFrame's 1 cm race quad 2 m ahead after the projection
  layer, KARTFRAME_RACE_QUAD), `--replay-hold 2` (each set for two refreshes: 60 Hz content).

Build per the xrprobe CMakeLists (needs the OpenXR headers); under the KartFrame build lock.

## Session (one launch of xrprobe, 30 s a case, framecorder `--view raw` recording)

| case | content | loop | expected if SteamVR is clean |
| --- | --- | --- | --- |
| 1 | standing | GalaxyFrame order, one swapchain per eye | clean (control) |
| 2 | driving | same | clean |
| 3 | driving, `--replay-offset 1` | same | clean |
| 4 | driving | `--replay-pairs 3` | clean |
| 5 | driving | `--replay-pairs 3 --replay-quad` | clean |
| 6 | driving | `--replay-hold 2` | judder (each image twice), no doubling |

Owner, each case: look at a track-side edge (fence posts, banner) and turn the head slowly left and right for
ten seconds, then hold still. Close one eye at a time. Say per case: doubled or not, which eye.

## Reading it

- Panels: `frame/perf/panels/analyse.sh <session>`, then H7's zigzag rule per eye and case (session-h12's
  zig2.py), menus-like floor 4-6 %.
- **Case 2 doubled like the game**: SteamVR does it to these images however they are handed over; the content
  (60 Hz scene motion shown at 120 Hz, interpolated) is what matters. Then case 3 tells whether the affected
  eye follows the content's phase (the per-launch eye: right in H7, left in H12 launch 3), case 6 whether
  plain 60 Hz content doubles too, and case 1 (no scene motion) should stay clean as in H12 launch 1's
  standing seconds.
- **Case 2 clean, 4 or 5 doubled**: the hand-over detail that case adds (the three-pair rotation, the race quad
  after the projection layer) is it: A/B it in the game next (`KARTFRAME_EYE_PAIRS`, `KARTFRAME_RACE_QUAD=0`).
- **All clean**: the images and the hand-over are fine; what remains is the live game's timing on the headset
  (when sets are released and ended relative to SteamVR's composition). Next: KartFrame launches that move
  toward this loop one switch at a time.
