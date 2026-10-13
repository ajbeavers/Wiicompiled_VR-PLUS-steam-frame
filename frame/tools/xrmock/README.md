# xrmock

A mock OpenXR runtime for testing the frame loop of `xr_app.cpp` without a
headset, and a script that compares what two builds do against it.

- `build.sh [out dir]` builds `libxrmock.so` and `runtime.json` (select it with
  `XR_RUNTIME_JSON=<out>/runtime.json`). Needs clang and the OpenXR headers of
  `third_party/openxr`.
- `trace-compare.sh <build A> <build B> [paired|unpaired|warp ...]` runs the
  real `galaxyframe` launcher of each build against the mock in a scratch
  `GALAXYFRAME_HOME` and diffs the traces (swapchain acquire / release, frame
  begin / end with their layers). Use `IGNORE_EXT=XR_KHR_vulkan_enable2` when
  comparing a build from before the extension was enabled.
- `xrmock.c` is the runtime (frame clock, call-order checks, trace, settings
  by `MOCK_XR_*` environment variables, listed in its header comment).
- `xrmock_gfx.h` is the seam to the graphics API; `xrmock_gles.c` is the GLES
  side (swapchain images are textures of the app's EGL context).

## Vulkan

`xrmock_vk.c` is the Vulkan side (`XR_KHR_vulkan_enable2`): the app's instance
and device are made through the mock with the app's own loader, swapchain
images are VkImages on the app's device, handed out in
`COLOR_ATTACHMENT_OPTIMAL`. Run the launcher with `PETARI_GFX=vk`.

Checks, as `ERROR` lines in the trace and a summary on stderr at the end of
the session (`xrmock: summary ...`):

- acquire, wait and release in order per swapchain (both APIs);
- Vulkan: no swapchain with mip levels (SteamVR on the Frame allocates
  memory for level 0 only; writing the others corrupted other images);
- Vulkan, with `MOCK_XR_BUSY_MS=<ms>`: at each release the mock waits for the
  app's queue to go idle; longer than that means work was still queued. Off
  by default: the app records and submits the next set before it releases
  the last (its own check, per image against the timeline, logs
  "released after a wait in the release itself"). `PETARI_NO_GPU_WAIT=1` in
  the app is the negative test of the app's check;
- both eyes of a projection layer come from one `xrLocateViews` (the head
  turns slowly, so views located for different times differ), and how many
  refreshes before the display time submitted that locate was made for.

A Vulkan session makes one set of eye swapchains (three with GLES) and
transfer usage on the UI layers, so its setup trace differs from a GLES one;
`trace-compare.sh` stays a GLES-against-GLES check.
