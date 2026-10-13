# Lane E measurements (headless mock, real Turnip GPU)

All runs: `frame/perf/run.sh NAME SECONDS VAR=value ...` from the worktree (mock runtime, private
Xvfb, scratch home `~/.cache/kf-laneE/home` with warm caches unless noted), then
`frame/perf/pacing-parts.py <run dir>` (per part, exact percentiles from KARTFRAME_PACING_SAMPLES) and
`frame/perf/hitches.py <run dir>`. Build: lane/E after bba3286 (SDL_SyncWindow fix, quad menus,
never half rate, HUD clip). Every row has `MOCK_XR_SKIP=1 TU_DEBUG=sysmem` (the wrapper's default)
and `"KF_MOCKRUN_VR=hud_virtual_screen = true"` (the owner's config; without it the race's 2D layer is
not reprojected); pacing is the default (auto = pipelined). Eyes 1728 per eye unless noted.

The attract-mode demo runs 1, 2 and 4 player races in turn (MKW runs 3 and 4 players at 30 fps).
"race" below is the 1-player race, the owner's case; the 10-minute soak also had a 2p and a 4p race,
listed apart. GPU and encode are ms per eye set (both eyes); sets/s per second of that part; late is
% of sets due; repeats % of compositor ticks.

## Table

| row | command (after `frame/perf/run.sh`) | part | s | sets/s | late % | repeats % | missed | GPU p50 / p99 | encode p50 / p99 | game fps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| (a) shipped default, 10 min | `A-1728-120-soak 600 MOCK_XR_SKIP=1 TU_DEBUG=sysmem "KF_MOCKRUN_VR=hud_virtual_screen = true" MOCK_XR_BUSY_MS=1` | menus | 429 | 120.3 | 0.04 | 0.11 | 36 | 1.58 / 3.08 | 0.18 / 0.44 | 59.8 |
| | | race (2 races) | 85 | 119.5 | 0.72 | 0.78 | 9 | 5.19 / 6.37 | 2.27 / 3.72 | 58.3 |
| | | race 2p | 41 | 112.9 | 6.37 | 6.37 | 0 | 6.18 / 7.61 | 1.90 / 3.32 | 46.0 |
| | | race 4p | 42 | 120.6 | 0.02 | 0.02 | 0 | 4.32 / 5.23 | 1.78 / 2.83 | 30.5 |
| (b) 1728 tiled | not rerun (see below) | | | | | | | | | |
| (c) FDM layer off | not applicable headless (see below) | | | | | | | | | |
| (d) 2160 at 120 | `D-2160-120 200 ... KF_MOCKRUN_RENDER_SCALE=1.25` | menus | 157 | 120.1 | 0.07 | 0.28 | 11 | 2.10 / 3.25 | 0.21 / 0.67 | 60.6 |
| | | race | 42 | 93.9 | 22.15 | 22.15 | 0 | 7.68 / 9.43 | 2.55 / 4.13 | 51.1 |
| (e) 2160 at 90 | `E-2160-90 200 ... KF_MOCKRUN_RENDER_SCALE=1.25 MOCK_XR_HZ=90 KARTFRAME_REFRESH_HZ=90` | menus | 156 | 90.4 | 0.05 | 0.24 | 1 | 2.37 / 3.73 | 0.24 / 0.73 | 60.6 |
| | | race | 42 | 90.5 | 0.05 | 0.05 | 0 | 7.41 / 8.67 | 2.36 / 3.67 | 58.8 |
| (f) 1728 at 90 | `F-1728-90 200 ... MOCK_XR_HZ=90 KARTFRAME_REFRESH_HZ=90` | menus | 156 | 90.3 | 0.06 | 0.24 | 9 | 2.10 / 4.09 | 0.24 / 0.77 | 60.6 |
| | | race | 42 | 90.5 | 0.05 | 0.05 | 0 | 6.88 / 8.09 | 2.57 / 4.03 | 54.3 |

(a) mock checks: 91202 releases compared after the GPU passed them, 0 changed after release, 0 mock
errors; game ticks 71918 / rendered 71465 / late 411 / repeats 453 against the mock's 71956 ended,
401 repeated + 43 without a layer = 444 (the difference is the seconds before the first and after
the last pacing line). Menus are a quad layer now: 51813 of the 71956 frames.

Reading the table:
- The shipped default holds the 1-player race: 0.72 % late over 85 s of race, GPU p99 6.37 ms (ship
  rule p99 <= 6.3 ms and late <= 2 %: late passes, p99 is at the line). The 2-player demo race is
  the only part above 2 % late.
- 2160 per eye at 120 Hz does not hold (22 % late, GPU 7.7 ms a set against 8.3 ms a refresh). At
  90 Hz it holds with room (0.05 % late, GPU p99 8.7 of 11.1 ms). It is the candidate if the owner
  wants the panel's native resolution: 2160 at 90.
- GPU ms per set at 90 Hz is higher than at 120 Hz for the same 1728 eyes (6.9 against 5.2): the
  GPU's clock follows its load (devfreq), so per-set times at different rates are not comparable;
  sets/s and late are.
- Run-to-run noise is large. The same 1728 1-player race measured 4.6 to 6.1 ms GPU p50 and 0 to 4 %
  late in back-to-back 135 s runs of one build (t8 to t11 in the scratch runs). A single 200 s row is
  a direction, not a verdict; the 10-minute soak is the number to use.
- (b) and (c) were measured by the previous lane E runs on the pre-fix build (1728 tiled: race 90.5
  sets/s, 1.5 % late, GPU 5.92 / 8.44; FDM switch: see below) and were not repeated: menus no longer
  render eyes, and the race's game rate changed with the SDL fix.
- (c) Valve's FDM layer is not in the Vulkan chain headless: the startup line reads
  `Vulkan layer chain (active, 1): VK_LAYER_FROG_gamescope_wsi_aarch64`. Its library is loaded, but
  as the OpenXR API layer `XrApiLayer_VALVE_fdm_injection` in front of the mock. The A/B is only
  meaningful in the headset (H1's chain: the loader skipped fdm_injection and rpo, "no
  vkGetInstanceProcAddr").

## Game frame rate (the "half speed" of H1)

aurora's window::pump_events called SDL_SyncWindow every game frame; on X11 with a window whose
geometry SDL does not expect (Xvfb without a window manager, gamescope) it ends in SDL_Delay(10).
Same build, 1728, demo race: `KARTFRAME_SYNC_WINDOW=1` (old) 38.1 game fps, 72.9 sets/s, 3.7 %
late; fixed 59.8 game fps, 120.6 sets/s, 0.0 % late. H1's race ran at 38.5 game fps. Commit 57526ee.
`KARTFRAME_FPSLOG=1` logs the game rate every 5 s (run.sh sets it).

## Validation layer (VUID 08910)

`vvl2 140 MOCK_XR_SKIP=1 TU_DEBUG=sysmem MOCK_XR_BUSY_MS=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
"KF_MOCKRUN_VR=hud_virtual_screen = true"` (current build, HUD clip shaders included), menus and the
demo race (entered at frame 5869, left at 6792): 0 validation messages, 0 mock errors. Chain:
`VK_LAYER_FROG_gamescope_wsi_aarch64 VK_LAYER_KHRONOS_validation`. The layer prints to stdout, which
mockrun writes into the game log; lane A's run of the same setup showed VUID 08910 and 01387 there,
so the empty result is a real one. Fixed by 375e0e3 (TexCopyConv blit pipeline for RGBA8 targets)
and 1ab059b (VK_EXT_surface_maintenance1 on the instance). Nothing is left from our code.

## Hitches (run (a))

Game frames longer than 20 ms (aurora `hitch:` lines, end_frame to end_frame), with the cause the
frame's own counters point at:

| part | frames > 20 ms | 20-33 | 33-50 | 50-100 | 100-250 | pipelines | textures | disc | worker | no cpu | game cpu | blocked |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| menus (429 s) | 23 | 15 | 6 | 1 | 1 | 0 | 0 | 21 | 0 | 0 | 0 | 2 |
| race 1p (68 s after the first 10) | 317 | 315 | 1 | 1 | 0 | 0 | 0 | 3 | 16 | 1 | 161 | 136 |
| race 1p start (2 x 10 s) | 141 | 127 | 11 | 1 | 2 | 0 | 0 | 10 | 7 | 6 | 31 | 87 |
| race 2p | 796 | 781 | 15 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 771 | 25 |
| race 4p (30 fps by design) | 943 | 433 | 509 | 1 | 0 | 0 | 0 | 2 | 0 | 0 | 941 | 0 |

- Pipelines: none. With a warm cache no frame waited for a pipeline.
- 1p race: 8 % of game frames miss one 60 Hz slot (20 to 33 ms), half of them with the game thread
  computing 13 ms or more of the frame itself ("game cpu": translated game logic plus GX recording),
  the rest blocked in the game's own waits. VR interpolation hides these from the eyes (sets/s stays
  at 119.5); they show as a game-frame repeat in motion.
- Race start: one 144 ms frame per race (course load: 4 MB read, 7.5 ms texture conversion) and a
  52 ms one, both inside the fade-in.
- Per-tick (compositor) stalls: 9 missed refreshes in the 1p race seconds of the soak, 36 in 429 s
  of menus (load screens).

Pipeline cache: the bundled seed `initial_pipeline_cache.db` (runtime/assets/pipeline, copied beside
the binary) is stale: its rows are GX config version 19 (2768 bytes) and clear version 3, the code is
at 22 and 4, so every row is skipped and a fresh install compiles everything at first use: 536 new
pipelines in hitch frames at the first race start of a cold home. See "Cold cache" below for what a
regenerated seed does.

## Half-rate rule

Commit fffb57c had changed the return rule (return early only when the GPU p90 at half rate is under
60 % of a refresh, otherwise after a retry wait doubling 10 to 160 s), because at 2160 and 120 Hz the
old rule cycled 10 s at half rate and 3 s at 40 % late. H1 then showed what any return rule does in
the headset: 16 drops and 15 returns in 4 minutes, 115 of the 143 race seconds at half rate, judder
and head-turn ghosting. Since b240caf the loop never drops by default (a late set repeats the
retained layer with its own poses and SteamVR reprojects it); KARTFRAME_HALF_RATE=auto drops after
more than 5 % late over 30 s and never comes back in the session; =1 always. Run (a): never
triggered (0 switches). Run (d), default: no switch, 22 % late shown as repeats.

## dlopen audit

`LD_DEBUG=libs` run (`ld-30s 30 ... LD_DEBUG=libs LD_DEBUG_OUTPUT=...`), game process only. The binary
needs libm, libstdc++, libgcc_s, libc, ld-linux (glibc 2.39, gcc-libs 15.1.1). Everything else is
dlopened: the OpenXR runtime (here the mock), the OpenXR API layer, the Vulkan loader and ICD, the
Vulkan layer, SDL's X11/dbus/udev back ends.

| library | package | installed |
| --- | --- | --- |
| ~/.cache/kf-xrmock/.../libxrmock.so (mock runtime; pulls libGLESv2, libEGL) | none (ours, test only) | built 2026-10-13 |
| ~/.local/share/Steam/steamrtarm64/libvulkan.so.1 | none: Steam runtime's Vulkan loader | file 2024-12-05 |
| ld-linux-aarch64.so.1, libc, libm | glibc 2.39-2 | 2026-09-15 |
| libstdc++, libgcc_s | gcc-libs 15.1.1 | 2026-09-15 |
| libcap | libcap 2.69-4 | 2026-09-15 |
| libdbus-1 | dbus 1.14.10-2 | 2026-09-15 |
| libexpat | expat 2.6.2-1 | 2026-09-15 |
| libffi | libffi 3.4.6-1 | 2026-09-15 |
| libsystemd, libudev | systemd-libs 257.7-2.2 | 2026-09-15 |
| libusb-1.0 | libusb 1.0.27-1 | 2026-09-15 |
| libz | zlib 1.3.1-1 | 2026-09-15 |
| libzstd | zstd 1.5.5-1 | 2026-09-15 |
| libdisplay-info | libdisplay-info 0.1.1-3 | 2026-10-07 |
| libdrm | libdrm 2.4.134-1 | 2026-10-07 |
| libEGL, libGLESv2, libGLdispatch | libglvnd 1.7.0-1 | 2026-10-07 |
| libSPIRV-Tools | spirv-tools 1.4.309.0-4 | 2026-10-07 |
| libVkLayer_FROG_gamescope_wsi_aarch64 | gamescope 3.16.28-2 | 2026-10-07 |
| libVkLayer_VALVE_fdm_injection (as the OpenXR API layer) | deckard-vulkan-layers-linux-aarch64 20260923.1-1 | 2026-10-07 |
| libvulkan_freedreno (Turnip) | deckard-mesa-linux-aarch64 26.3.0_devel+gitabc426bb-1 | 2026-10-07 |
| libwayland-client | wayland 1.26.0-1 | 2026-10-07 |
| libX11, libX11-xcb, libXau, libXcursor, libXdmcp, libXext, libXfixes, libXi, libXrandr, libXrender, libXtst, libxshmfence | libx11 1.8.9, libxau, libxcursor, libxdmcp, libxext, libxfixes, libxi, libxrandr, libxrender, libxtst, libxshmfence | 2026-10-07 |
| libxcb, libxcb-dri3/present/randr/shm/sync/xfixes | libxcb 1.17.0-1 | 2026-10-07 |

The 2026-10-07 dates are not the owner's installs: `/etc/os-release` has BUILD_ID=20261007.6125817,
and 850 of the 966 packages carry install times between 15:58 and 16:01 on 2026-10-07, packaged by
SteamOS's CI, while pacman.log has no transaction after 2026-10-02 (the only manual one there:
`pacman -S --needed base-devel git`). They are the stock image of the current SteamOS build; the
other 116 date from 2026-09-15. Nothing the game loads is an owner install.

Worth knowing: the shell (and Steam's launch environment) puts `~/.local/share/Steam/steamrtarm64`
on LD_LIBRARY_PATH, so the game uses the Steam runtime's Vulkan loader (2024-12-05), not the system
vulkan-icd-loader 1.4.309; every other library is tried there first and then found in /usr/lib.

## Cold cache and the pipeline seed

Same settings as row (a), 200 s, empty home (`KF_PERF_COLD=1`, own `KF_PERF_HOME`):

| run | seed | boot | race sets/s | late % | GPU p50 / p99 | game fps | race frames > 20 ms (pipelines) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C1-cold-stale-seed | bundled (v19 rows, all skipped) | no prewarm | 118.3 | 1.84 | 5.88 / 6.88 | 57.2 | 175 (0) |
| C2-cold-new-seed | regenerated (2346 v22 + 10 clear rows) | prewarm 2346 pipelines in 3.8 s, 4220 Dawn blob stores | 105.9 | 12.06 | 6.76 / 8.02 | 51.3 | 552 (0) |

Neither run has a game frame that waited for a pipeline: first-use compiles run on the compiler
workers and the draw is skipped until its pipeline is ready (a missing element for a frame or two,
not a stall). The C2 race is worse, but within the run-to-run spread measured above; the seed does
not show a pacing benefit in the mock. It does remove the first-use compiles (the old cold run
compiled 536 pipelines at the first race start) at a cost of 3.8 s of background compiling at the
first boot. How it was produced (current GX config version 22, clear version 4):

    cp build-linux/initial_pipeline_cache.db seed.db
    sqlite3 seed.db "attach 'file:$HOME/.cache/kf-laneE/home/data/WiiCompiled/Cache/pipeline_cache.db?immutable=1' as u;
      delete from pipeline_cache;
      insert into pipeline_cache select * from u.pipeline_cache
        where (type=0 and config_version=4) or (type=1 and config_version=22);
      detach u; vacuum;"

from a home that ran menus and the demo races (1, 2 and 4 players, several courses). Shipping it means
replacing runtime/assets/pipeline/initial_pipeline_cache.db (not a lane E file); it goes stale again
whenever GXPipelineConfigVersion changes.

## Other checks

- Refresh request: `t16-rates90 40 MOCK_XR_SKIP=1 TU_DEBUG=sysmem MOCK_XR_RATES=90` (the mock offers
  only 90, as SteamVR did in H1): `display refresh rates: 90; 120 Hz not offered, requested none
  (SteamVR's per-app setting decides)`, no request in the mock trace.
- KARTFRAME_HALF_RATE=auto: `D2-2160-120-auto 200 ... KF_MOCKRUN_RENDER_SCALE=1.25 KARTFRAME_HALF_RATE=auto`
  dropped once, 23 s into the race ("181 of the last 3600 eye sets late; rendering every second refresh
  for the rest of the session"), and stayed there.
- Intro (first-run welcome and controller guide shown, `KF_MOCKRUN_INTRO=1`, 90 s, projected): GPU
  2.10 / 2.87 ms a set with TU_DEBUG=sysmem, 2.06 / 2.70 tiled. sysmem does not make the guide
  expensive in the mock.
