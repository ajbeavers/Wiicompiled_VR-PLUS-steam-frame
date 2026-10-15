# Lane U: does the release wait for the GPU work that wrote the image? Yes (verdict B)

Question (after session 13: KartFrame's own race images through a minimal loop were clean on the headset in
all six cases): can a set be released while its images are still unwritten, so that SteamVR shows the
image's content from one reuse ago (H7: the jump follows the pair count)? And what else differs per
process between the probe and the game at submission?

## 1. The order, read from the code

- Aurora's frame worker encodes both eyes and the sink draw into the swapchain images in one command buffer,
  then, holding `g_queueSubmitMutex`: `g_queue.Submit(1, &buffer)` and right after it the sink's `submitted`
  callback (aurora.cpp, `submitEncodedSlot` / the replay path).
- Dawn (pinned 13abc3bc, `src/dawn/native/vulkan/QueueVk.cpp`): `Queue::SubmitImpl` records the command
  buffers into the pending recording context and calls `SubmitPendingCommandsImpl()` at once, which calls
  `vkQueueSubmit`. Nothing is deferred: when `Submit` returns, the render work is on the VkQueue.
- `Bridge::Submitted` -> `AuroraDawnVulkanRelease` (patch `aurora_vulkan_interop.inc`): under Dawn's device
  guard, the COLOR_ATTACHMENT_OPTIMAL barriers into the pending context and `SubmitPendingCommands()`
  (a second vkQueueSubmit). Then `GpuCompletion::Signal`: under the same guard, an empty vkQueueSubmit that
  signals the set's timeline value. A semaphore signal's first synchronization scope is every command
  earlier in submission order on that queue, so the value cannot signal before the set's render work and
  its barriers finished. The pacing thread releases only after the watcher saw that value
  (`PollAheadSet` / `FinishFrame`).
- Queue and family: the XR binding gets Dawn's family and queue index 0 (`handles.queueFamily/queueIndex`);
  the watcher's `vkGetDeviceQueue` uses the same pair; Turnip has one queue family. SteamVR's client submits
  on that queue inside our calls, under the guard.
- Layouts: images stay COLOR_ATTACHMENT_OPTIMAL (Dawn's tracked state, the release barrier, lane Q's
  first-write barrier); no ownership transfer, as GalaxyFrame (H9 table).

So a "fence on the real submission" adds no ordering the timeline value lacks; no `fence` mode was built.

## 2. The mock's checks, and a strict one

- Old exact check (`MOCK_XR_BUSY_MS`): host reads 512 x 64 bytes of the image's memory at the release (no
  wait on anything of the app's), then again once an empty submission made at the release completed. Blind
  spot: work submitted AFTER the release would land after both samples (the stamps would catch a stale
  image, the bytes would not).
- New strict check: the same bytes at the release against the bytes at the image's next
  `xrAcquireSwapchainImage` (CPU only, no queue). Any write between release and reacquire, however ordered,
  is an error ("changed between its release and its next acquire").
- New release-queue line per swapchain: a waiter thread times the empty submission made inside
  `xrReleaseSwapchainImage` (as SteamVR's client submits there) until it completes, i.e. how long a
  submission at the release waits behind our queued GPU work; split by frame parity.
- Positive control (`KARTFRAME_NO_GPU_WAIT=1 KARTFRAME_PACING=inline`: release right after submission):
  strict 13891 of 14282 changed, exact 13909 of 14300; release queue p50 1.37 ms, p90 3.51 (even frames
  p50 3.40, odd 1.14: the per-set GPU cost alternates with the 60 Hz content). With the default pipelined
  pacing, `NO_GPU_WAIT` alone is caught once in 14271 (sets are released two refreshes after submission, the
  GPU long done).

## 3. Measurements (scripted race, A/B interleaved)

`hudrace.sh` 100 s, `MOCK_XR_STAMP=1 MOCK_XR_BUSY_MS=1 KF_MOCKRUN_END_GATE=2`, lane T's presses (A held from
56 s), head sway from 51 s, `KARTFRAME_FRAME_LOG=1`; runs 1-2 at render scale 0.75 (the race at 60 fps),
run 3 at 1.0. A = default, B = `KARTFRAME_RELEASE=idle`. Race window (67 s):

| run | sets/s | late | GPU ms p50 / p99 | end > 1 ms after wake-up | strict changed | exact changed | mock errors | release queue p50 / p99 ms (eye swapchains) | drain before release / end, mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | 120.5 | 0.0 % | 3.16 / 4.32 | 0.4 % | 0 of 23857 | 0 of 23880 | 0 | 0.31-0.45 / 0.71-0.73 | - |
| B1 | 120.4 | 0.0 % | 3.23 / 4.46 | 13.2 % | 0 of 23867 | 0 of 23890 | 0 | 0.16-0.24 / 0.36-0.41 | 0.33-0.42 / 0.12-0.14 |
| A2 | 120.5 | 0.0 % | 3.34 / 4.17 | 0.3 % | 0 of 23871 | 0 of 23894 | 0 | 0.27-0.43 / 0.64-1.39 | - |
| B2 | 120.4 | 0.0 % | 3.14 / 4.01 | 12.2 % | 0 of 23877 | 0 of 23900 | 0 | 0.14-0.17 / 0.37-0.41 | 0.31-0.44 / 0.10-0.14 |
| A3 (1.0) | 120.4 | 0.0 % | 4.55 / 5.75 | 0.5 % | 0 of 23867 | 0 of 23890 | 0 | 0.25-0.32 / 0.68-1.72 | - |
| B3 (1.0) | 120.5 | 0.0 % | 4.47 / 5.63 | 10.7 % | 0 of 23869 | 0 of 23892 | 0 | 0.19-0.26 / 0.35-0.38 | 0.28-0.35 / 0.12-0.15 |

- Default: a submission made at the release completes in 0.3-0.45 ms p50 (about the floor of a submit,
  interrupt and wake-up on this machine; with idle it is 0.15-0.25), > 2 ms in at most 0.3 % of releases,
  even and odd frames alike. The set released was finished and the next one is mostly done by the wake-up:
  at the release our queue is about as empty as the probe's.
- Stamps (A1): 15992 of 15998 eye images rendered for the locate of their submitted poses, 0 for another.
- `idle` costs the drain's round trip (0.3-0.4 ms before the releases, 0.1 before xrEndFrame): xrEndFrame
  later than 1 ms after the wake-up in 11-13 % of frames (fails the 2 % end gate), no late sets, no
  missed refreshes, GPU per set unchanged. Fine for a headset A/B, not a default.
- Machine idle check before every run; SteamVR (vrserver/vrcompositor) running as always.

## Verdict

B: the release is ordered correctly. The release cannot precede the write: by Dawn's code (the render work
is on the VkQueue before the timeline signal is queued), by the exact check, and by the new strict check
(0 of about 143000 released images in six race runs changed between release and next acquire, while the
positive control flags 97 %). And at SteamVR's release-time submission our queue is nearly empty, as in the
probe. H7's "jump follows the pair count" is therefore not our own image reuse.

Per process, what the game has that the probe (and every mock run) lacks, ranked:

1. **The desktop window's presentation.** On the headset the game creates a Gamescope WSI swapchain
   (854x480, Mailbox; "Creating swapchain for xid" in every game log) and presents into it once per game
   frame: 60 Hz, i.e. every second refresh, on the same VkQueue as the eyes and SteamVR's client (Dawn's
   present path waits on the WSI acquire semaphore in a queue submission and vkQueuePresentKHR goes through
   the gamescope layer). Mock runs set `KARTFRAME_HEADLESS=1` (no swapchain; 63 offscreen presents a second),
   xrprobe has no desktop swapchain. Period 2 at 120 Hz with a start-up phase against the eye releases fits
   "every second refresh, which eye per launch". Not reproducible headless (no gamescope here).
2. **Queue backlog at SteamVR's release or end submission that only the headset has** (for example
   created by 1, or the headset's own GPU timing). Measured absent in the mock; `KARTFRAME_RELEASE=idle`
   removes it and its 10 s log line measures it on the headset.
3. **GPU contention with SteamVR's compositor**: the game keeps the GPU busy 3-5 ms a set, with a heavy and
   a light set alternating at 60 Hz (positive control: 3.4 against 1.1 ms of work left at the release);
   the probe's blit is a fraction of a millisecond. Next test if 1 and 2 are not it: a render-scale-0.5
   measurement launch, or xrprobe replay with an added GPU load alternating at 60 Hz.
4. Excluded here: release before the GPU finished, a Dawn submission after the signal, timeline semaphore
   ordering, queue family mismatch, layout transitions. TU_DEBUG=sysmem and the Vulkan layer chain are the
   same for both (the wrapper exports TU_DEBUG before running KARTFRAME_EXE; both logs show the
   fdm_injection and Gamescope WSI layers); sysmem changes how Turnip renders a pass, not the memory of
   SteamVR's images.

Session 14 (`frame/perf/session-h14/README.md`) tests 1 and 2 in two launches.

## Commits (lane/U)

- `xrmock`: strict release check and release-queue latency (only with MOCK_XR_BUSY_MS).
- `KARTFRAME_RELEASE=idle` (default unchanged) with DEV.md lines.
