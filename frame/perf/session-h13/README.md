# Headset session 13 (ready, not run): KartFrame's own race images through GalaxyFrame's loop (xrprobe replay)

Lane S (`frame/perf/LANE_S.md`) found the race eye images KartFrame renders smooth in both eyes, standing
and driving, with the head turning (H7's zigzag rule 0 % per eye; an injected period-2 jump is flagged in
99-100 % of moving sets). What is left is what SteamVR does with them. This session shows SteamVR the same
kind of images from a minimal loop that is known to be clean on this headset (GalaxyFrame's), and changes one
hand-over detail at a time.

## Recording (headless, before the session)

```
KF_DUMP_DIR=~/.cache/kf-laneT/dump-h13rec KF_PERF_OUT=~/.cache/kf-laneT/runs KF_PERF_HOME=~/.cache/kf-laneT/home-race \
SECS=61 DUMP_AT=52.5,56.5 frame/perf/mockdump/dumprace.sh h13rec MOCK_XR_SWAY=0 MOCK_XR_DUMP_STEP=2 \
  "MOCK_XR_PRESS=/user/hand/right/input/a/click@12/1.5/0.15/46;/user/hand/right/input/a/click@54/1000/1000"
```

Head still (`MOCK_XR_SWAY=0`: every image rendered for yaw 0, eyes at x = -+32 mm, the Frame's own per-eye
fields of view, the mock's defaults), render scale 0.75 (the race at 60 fps, scene stamps 0.25/0.75 as on the
headset), 648 px per eye, two windows of 240 consecutive sets (lane T's recording: 960 images, 1.0 GB, mock 0
errors; not in the repo, the images come from the disc). `SECS=58` as first planned cut the second window to 175
sets; 61 gives all 240. Checked on the images: 52.5 s is the race after GO (timer 0:10) with the kart still
until set 172 (the A hold at 54 s), then driving off; 56.5 s drives through the start gate. Case 1 therefore
plays the first 168 sets only (`--replay-sets 168`, 1.4 s with no scene motion beyond the HUD).

## xrprobe `--replay DIR` (built, lane T)

`frame/tools/xrprobe/xrprobe.c` (`runReplay`): loads index.txt, takes one window's sets (both eyes, ordered by
display time), uploads every image once into an R8G8B8A8_SRGB array (2 layers a set, 0.8 GB for 240 sets).
Frame n shows set k = (hold x floor(slot / hold) + offset) mod N, slot = round((predicted display time - first
one) / period): a missed refresh skips a set. Each eye's image is blitted (linear) into its 1728 swapchain image,
a label strip (case label, the set number with a stepping hue and ticker) is drawn over it 1.5 m ahead and 0.75
m up in the image's own frame, and the projection layer carries the recorded pose (identity, x = -+0.032 m,
LOCAL) and fov, not the locate's: SteamVR places the race world-fixed in front of the startup direction and
reprojects it as the head turns.

- Default (GalaxyFrame's order, one swapchain per eye): xrWaitFrame, xrBeginFrame, xrLocateViews (LOCAL, the
  frame's own time), acquire both, blit, fence wait, release both, xrEndFrame.
- `--replay-pairs 3` (KartFrame's sets ahead): xrWaitFrame, xrBeginFrame, release the newest finished set due by
  this display time (else repeat the retained layer), xrEndFrame; then xrLocateViews for two refreshes ahead,
  acquire the next pair (first in, first out, never the one on display or in flight), blit, submit, no wait.
- `--replay-quad`: KartFrame's race quad (16x16, cleared transparent once, 1 cm, 2 m ahead, alpha blended)
  after every projection layer. `--replay-offset 1`, `--replay-hold 2`, `--replay-window`, `--replay-sets`,
  `--replay-label` as in `xrprobe --help`.
- Log (`--log`): one `R` line a frame (n, display time, slot, set shown, set wanted for this display time,
  its recorded display time, new or repeated, pair, set started) and SUMMARY lines: frames with a layer, new
  against repeated, the set matching its display time, set steps between frames, display time steps, fence wait.

## Session (six launches, one case each; framecorder `--view raw` recording)

| case | label | content | loop | expected if SteamVR is clean |
| --- | --- | --- | --- | --- |
| 1 | 1 StAnd | standing (168 sets) | GalaxyFrame order, one swapchain per eye | clean (control) |
| 2 | 2 drivE | driving | same | clean |
| 3 | 3 PHASE | driving, `--replay-offset 1` | same | clean |
| 4 | 4 PAIrS | driving | `--replay-pairs 3` | clean |
| 5 | 5 qUAd | driving | `--replay-pairs 3 --replay-quad` | clean |
| 6 | 6 HOLd | driving | `--replay-hold 2` | judder (each image twice), no doubling |

Install and queue (the orchestrator; nothing else changes, the game stays installed as it is):

```
cmake -S frame/tools/xrprobe -B ~/.cache/kf-xrprobe -G Ninja -DCMAKE_C_COMPILER=clang
flock ~/Games/KartFrame/.build.lock cmake --build ~/.cache/kf-xrprobe
frame/perf/session-h13/queue.sh ~/.cache/kf-xrprobe/xrprobe ~/.cache/kf-laneT/dump-h13rec
```

queue.sh copies the binary and `frame/tools/xrprobe/replay-case.sh` into `$KARTFRAME_HOME/h13/` and writes
`h13-01-standing.env` .. `h13-06-hold.env` into `$KARTFRAME_HOME/session.queue/`. Each makes the KartFrame
wrapper run the case instead of the game (`KARTFRAME_EXE`), with the panel recording (`KARTFRAME_RECORD=raw`),
SteamVR's per-app settings (120 Hz, 1728), its own session folder (`xrprobe.log`, `stdout.log`,
`vrtiming.log`, `panels.mkv`) and `KARTFRAME_SESSION_ENV_ONCE=1`. A case runs 45 s of visible time, then ends
by itself (back to the Steam library).

Owner, each launch from the KartFrame shortcut (the label above the image names the case):

1. Wait until the race image is up (the recording starts 8 s after the launch).
2. Look at a track-side edge (fence posts, the banner, the gate) and turn the head slowly left and right for
   ten seconds, then hold still for five.
3. The same with the left eye closed, then with the right eye closed.
4. Say: doubled or not, which eye. Case 6 judders by design; the question there is doubling only.

The driving window loops every 2 s (a jump back at the loop); the standing one every 1.4 s (no visible jump).

## Reading it

- Per launch: `$KARTFRAME_HOME/sessions/<time>/xrprobe.log` (`R` lines, SUMMARY: frames with a layer, repeats,
  the set against its display time; with pairs also sets released unshown and late), `vrtiming.log`.
- Panels: `frame/perf/panels/analyse.sh <session>`, then H7's zigzag rule per eye and case (session-h12's
  zig2.py), menus-like floor 4-6 %. The number panel beside the label (set k, hue and ticker) gives the set
  shown on each panel refresh.
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

## Mock verification (lane T)

```
XRPROBE=<binary> PROBE_MOCK=1 PROBE_SET=replay PROBE_SECONDS=20 PROBE_OUT=<dir> KF_REPLAY_DIR=<recording> \
  frame/tools/xrprobe/run-probes.sh          # + MOCK_XR_DUMP_AT=4 MOCK_XR_DUMP_FRAMES=6 MOCK_XR_DUMP_STEP=8: dumps
python3 -I frame/perf/session-h13/replaycheck.py <dir>
```

The mock runtime (never SteamVR), 120 Hz, 1728 eyes, `MOCK_XR_TRACE_DETAIL=1`, `MOCK_XR_BUSY_MS=1`. 20 s a case
(2401 frames); images from a second 8 s run with dumps of 6 releases per swapchain at 4 s.

| case | mock summary | frame order in the trace (share of frames) | sets (probe) | images: dump vs shown set (k+-1) |
| --- | --- | --- | --- | --- |
| 1 | 0 errors, 4802 releases finished | wait, begin, locate +0, acquire L R, release L R, end [proj] 100 % | 2401 of 2401 = display-time set, steps 1: 2400 (loop of 168) | 0.19 (0.22: no motion) |
| 2 | 0 errors | same, 100 % | 2401 of 2401, steps 1: 2400 | 0.22 (7.54) |
| 3 | 0 errors | same, 100 % | 2401 of 2401 (offset 1), steps 1: 2400 | 0.22 (6.93) |
| 4 | 0 errors, 4750 releases finished | wait, begin, release L R, end [proj], locate +2, acquire L R: 98.4 % | 2359 of 2399; 31 repeats, 7 unshown, 24 late | 0.23 (7.60), 36 images |
| 5 | 0 errors | same with end [proj quad]: 98.6 % | 2367 of 2399; 26 repeats, 6 unshown, 20 late | 0.23 (7.60), 36 images |
| 6 | 0 errors | as case 1, 100 % | 2401 of 2401, steps 0: 1200, 2: 1200 (each set twice) | 0.22 (8.19) |

- Display times: every xrEndFrame carried its frame's predicted display time (2401 of 2401 per case, "end
  display time = predicted"); the layer's display time steps one refresh in 2400 (cases 4, 5: 2398 of 2398).
- Poses: every projection layer "poses from no known locate" (the recorded ones, not a locate's), fov exactly
  the recorded L -1.0311, 0.8795, 0.8634, -1.0472 / R -0.8777, 1.0325, 0.8706, -1.0393 rad; locates for the
  frame's own time (cases 1-3, 6) or two refreshes ahead (4, 5); the quad on its own 16x16 swapchain, LOCAL,
  size 0.010, alpha-blend flag.
- Images: every dumped released eye image matches the recorded image of the set the probe logged for that
  frame and eye, resampled as the linear blit does (mean absolute difference 0.19-0.23 of 255 outside the label
  strip), nearer it than to sets k-1 and k+1 in 12 of 12 (cases 1-3, 6) and 36 of 36 (4, 5).
- Cases 4, 5: the repeats, late and unshown sets all fall within four frames after an xrWaitFrame that returned
  less than 2 ms after the last (the probe scheduled late on a busy machine, the mock then catching up): 40 of 40
  and 32 of 32 frames. KartFrame's loop does the same there (no wait inside the cycle); on the headset these
  show in the SUMMARY's late / unshown counts.
- The wrapper path: queue.sh into a scratch KARTFRAME_HOME, then `frame/kartframe.sh` with the mock's
  XR_RUNTIME_JSON: the queue file became session.env, the wrapper ran replay-case.sh, xrprobe.log landed in the
  session folder, the queue advanced to the next case.

## Headset results (2026-10-15, orchestrator's reading)

Owner, six launches in order: launch 1 "no doubling at all when moving my head"; launches 2 to 5 "no
doubling"; launch 6 "no doubling". Remark: a quality difference, "not full 3D all the way around" (expected:
the replay shows fixed recorded images, half resolution (dump step 2), moved with the head, so nothing new
appears at the edges on a head turn).

Reading: KartFrame's race eye images, through GalaxyFrame's loop AND through KartFrame's own order (three
pairs, locate two refreshes ahead, the race quad), with the 60 Hz content phase either way, and held two
refreshes, are all clean on this SteamVR. So the images (lane S), the poses and prediction distance (H12),
the layer structure, pair count, quad and content phase (this session) are all cleared. What is left is what
differs between the probe and the live game at submission: the game's GPU work on the swapchain image and
when SteamVR sees it (the probe's images are uploaded once; the game renders into the image every set and
releases after its fence), Dawn's queue and the point at which its commands are really submitted relative to
the fence the release waits on, the Vulkan layer chain of the game process, and GPU contention from the
game's own ~5 ms per set. Strongest old clue in this light: H7 found the jump size follows the pair count
(2 pairs: clean +-2-refresh jumps; 6 pairs: 1 to 8 refreshes), i.e. the swapchain image reuse distance.
