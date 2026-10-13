# KartFrame development notes (lane 0)

## Build

    frame/build-dawn.sh                      # once: patched static Dawn into ~/.cache/kartframe/dawn/<rev>-<patch hash>/
    ~/Games/KartFrame/kfbuild.sh <worktree> WiiCompiled      # the game, binary build-linux/kartframe
    KF_CONFIGURE_ONLY=1 ~/Games/KartFrame/kfbuild.sh <worktree>   # configure only (no lock)

`kfbuild.sh` takes the shared build lock, uses ccache (~/.cache/ccache-kf), links the worktree's
`generated` to the development translation (`~/Games/KartFrame/data/generated-dev`, never committed),
and passes the newest Dawn package (or `KARTFRAME_DAWN_DIR`) as `-DMKW_DAWN_PACKAGE_DIR`. The project
only accepts `CMAKE_BUILD_TYPE=Release`. The translated shards are the heavy part;
`MKW_TRANSLATED_COMPILE_JOBS` (default 3 in kfbuild) caps how many compile at once.

The CMake wiring for Linux: OpenXR on by default, pinned Vulkan headers and the pinned static OpenXR
loader (vendored jsoncpp) are fetched, the patched Dawn package is linked statically (its
`AuroraDawnVulkan*` ABI resolves at link time), SDL3 is built from source (audio: ALSA, JACK, PipeWire,
PulseAudio loaded at run time, plus dummy; video: Wayland, X11, KMS/DRM, offscreen, dummy). The shard
manifest and the blob assembly of a translation made elsewhere are relocated at configure time.

## Where the runtime looks for things

Executable `kartframe` (target `WiiCompiled`). Its resources (`dsp_coef.bin`, `initial_pipeline_cache.db`,
`wii_bootstrap/`, `resources/`) are copied next to it in the build directory and are found next to the
executable.

User state, from `runtime_config.h`:

- Normal install: `$XDG_DATA_HOME/WiiCompiled` (default `~/.local/share/WiiCompiled`), holding
  `Config.toml`, `NAND/`, `Cache/`, `Logs/`.
- Portable: a file `portable.txt` in the executable's directory or up to a few parents makes
  `<that dir>/UserData` the root instead.
- `Config.toml` is created with defaults on first start. The game files are not found automatically:
  `[paths] dvd_root` must name the extracted DATA directory (the one holding `sys/fst.bin` and
  `files/`); a relative value is relative to the Config.toml. `[paths] nand_root` is optional; when it
  is absent the managed NAND is `<state dir>/NAND` (the licence is made in-game on first boot).
- `[vr] enabled = true, required = false` (defaults) try OpenXR and fall back to the desktop renderer.
  The OpenXR loader reads `~/.config/openxr/1/active_runtime.json` (SteamVR) unless `XR_RUNTIME_JSON`
  is set. `[vr] force_steamvr` does nothing on Linux.
- The program takes no command line arguments.

## Tools and runs

`frame/tools/mockrun.sh [--flat] [--seconds N]` runs the game against the mock runtime (xrmock) or flat,
headless: a private Xvfb for the window (Dawn needs a window surface to pick the adapter),
`SDL_AUDIO_DRIVER=dummy`, and `KARTFRAME_HEADLESS=1`, which makes aurora skip configuring and
presenting the desktop swapchain (it renders into an offscreen texture and logs a frame count). The
scratch user state is `~/.cache/kf-mockrun/data/WiiCompiled`. It aborts when the mock runtime file is
missing or empty and when a game is already running. Never run the game with the real runtime from an
agent. Known facts: Valve's implicit OpenXR layer `XrApiLayer_VALVE_fdm_injection` is loaded in front of
the mock; the mock offers no `XR_KHR_convert_timespec_time` yet, so the game logs "no clock
conversion"; abseil, zlib, libpng and freetype are built static from pinned sources (AURORA_VENDOR_THIRD_PARTY); the binary needs only glibc, libstdc++ and libgcc_s.

## Pacing knobs (lane A)
Config `[vr] frame_pacing = auto|inline|pipelined` (auto = pipelined, measured) and `refresh_rate`
(120; requested in-app only when SteamVR offers exactly that rate, else the wrapper's per-app key
decides). Menus are a quad layer fixed in the room (menu_shader_quality 0), the first-run guide is
projected. Env: KARTFRAME_PACING overrides the mode; KARTFRAME_REFRESH_HZ the rate;
KARTFRAME_PACING_MARGIN_US (1000) is kept free before the deadline for release + xrEndFrame;
every refresh always by default (a late set repeats the retained layer, which SteamVR reprojects;
half rate is 60 Hz content shown twice, the judder H1 rejected); KARTFRAME_HALF_RATE=1 always every
second refresh (measurements), =auto drops after more than 5 % late over 30 s and stays there for
the session; KARTFRAME_PACING_DEBUG=1 per-set timelines;
KARTFRAME_NO_GPU_WAIT=1 negative test only. The wrapper sets TU_DEBUG=sysmem unless
KARTFRAME_SYSMEM=0 (the race holds 120 Hz at 1728 only with it). Log line per second:
`vr pacing: t= hz= mode= ticks= rendered= interpolated= late= missed= repeats= gpu_ms=p50/p99 encode_ms=p50/p99`
(format in runtime/include/vr/openxr_diagnostics.h); session-report.py reads it with vrtiming.log.
Mock: MOCK_XR_BUSY_MS (exact released-unfinished check), MOCK_XR_FOCUS_BLIP=<ms>[@<s>],
MOCK_XR_PROFILE, MOCK_XR_EYE, MOCK_XR_TIME_OFFSET_S, MOCK_XR_DUMP=<dir> (released images as PPM at
MOCK_XR_DUMP_AT seconds); mockrun seeds welcome/tutorial done so the demo race starts by itself
(KF_MOCKRUN_INTRO=1 shows them). KARTFRAME_FPSLOG=1 logs the game frame rate every 5 s;
KARTFRAME_SYNC_WINDOW=1 restores aurora's per-frame SDL_SyncWindow (10 ms a game frame on X11);
KARTFRAME_HUD_CLIP=0 stops clipping the race's 2D layer to the frame and the game's scissor.

## Lane G switches
KARTFRAME_SCENE_CLOCK=0 restores the old interpolation rule (scene sampled at the display time;
with pipelined pacing that pinned the weight to the newest frame). KARTFRAME_END_TIME=cycle ends
layers at the cycle's predicted time instead of their poses' locate time. KARTFRAME_MENU_ENV=over
puts the menu environment layer (controllers, lasers) over the screen quad; =0 turns it off.
KARTFRAME_FRAME_STAMP=1 (tests) stamps each eye image with its locate time. Mock: MOCK_XR_STAMP,
MOCK_XR_PREDICT_JITTER, MOCK_XR_FOV (default Frame-like asymmetric), MOCK_XR_HANDS, MOCK_XR_PRESS
(scripted presses; frame/perf/hudrace.sh drives a real Grand Prix race). Log lines: "vr display
times" and "VR interpolation weights" every 10 s, eye fields of view once. Details in frame/perf/H2.md.
