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
conversion"; the binary links the system libabsl, libpng, freetype (present on SteamOS).
