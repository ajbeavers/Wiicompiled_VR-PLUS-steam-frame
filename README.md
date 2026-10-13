# KartFrame

Mario Kart Wii in VR, running natively on the Valve Steam Frame.

KartFrame is a fork of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), the Mario Kart Wii VR port. The game in it is heurazy's static translation of the Wii game to C++, drawn by the aurora renderer on Dawn (WebGPU) and shown through OpenXR. KartFrame builds it as a Linux Arm64 program that runs in the Frame's own SteamVR. You do not need Lepton, an APK, adb or a PC.

You need your own Mario Kart Wii disc. No game data is in this repository or its releases.

What it does:

- Plays Mario Kart Wii in VR on the Steam Frame, in the Frame's SteamVR.
- Renders both eyes on every refresh of the 120 Hz display, from your head position at that refresh, at 1728 pixels per eye.
- Uses the Steam Frame controllers through SteamVR's own controller profile.
- Builds the game on your Frame from your own disc, once, in about 6 minutes.

[to confirm in the headset]

## What this adds to Wiicompiled VR PLUS

Upstream runs on Windows and on the Meta Quest. On the Frame that would mean Lepton and an Android layer between the game and SteamVR. KartFrame changes the platform side and leaves the game as it is:

- A native Linux Arm64 build.
- Dawn's Vulkan backend bound to SteamVR's own Vulkan device, through upstream's own Dawn patch.
- Eye images handed to SteamVR only after the GPU has finished them.
- Both eyes rendered on every refresh from that refresh's head pose, with the game's own VR frame interpolation filling the display rate from the game's 60 frames a second.
- Pipelined frame pacing, with the older single stage mode kept as a switch.
- The Frame controller layout in code, with no SteamVR binding file.
- Simplified models of the Frame's controllers, converted at install from the Frame's own SteamVR files.
- The game's 2D screen shown as a flat quad layer in front of you. [to confirm in the headset]
- A first run that extracts your disc, translates the game and compiles it on the Frame with a bundled compiler, using a kit of prebuilt runtime parts.
- A launch wrapper that sets SteamVR's per-app settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) at every launch and writes a session log folder.

## Status

Version 1.0.0 is the first release.

[to confirm in the headset]

The list of what is built but not yet confirmed in the headset is in [frame/UNTESTED.md](frame/UNTESTED.md).

## Requirements

- A Steam Frame with SteamVR, and about 1 GB free for the toolchain and kit, plus the extracted disc.
- Your own clean PAL Mario Kart Wii disc image (RMCP01).
- A terminal on the Frame for the one time setup: SSH, or Konsole from desktop mode.
- Steam running on the Frame while you install, so the installer can add the library entry.

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/KartFrame.git
   cd KartFrame
   ```

2. Download `kartframe-1.0.0-linux-arm64.tar.gz` and its `.sha256` file from the [Releases](https://github.com/simplysanatori/KartFrame/releases) page. Check the download and install it:

   ```sh
   sha256sum -c kartframe-1.0.0-linux-arm64.tar.gz.sha256
   frame/install.sh ~/Downloads/kartframe-1.0.0-linux-arm64.tar.gz
   ```

   The installer unpacks the program into `~/Games/KartFrame/app`. It adds **KartFrame** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It converts the Frame's controller models from SteamVR's own files. The script talks to Steam through its local debugging port, which SteamOS has on by default.

3. Build the game from your disc, once:

   ```sh
   ~/Games/KartFrame/app/setup.sh /path/to/MarioKartWii.iso
   ```

   Setup checks that the disc is RMCP01, extracts it to `~/.local/share/KartFrame/game`, translates the game, compiles it with the bundled compiler and installs the finished program. It took about 6 minutes on my Frame. It can be run again after an interruption and picks up where it stopped.

4. Put the headset on and launch **KartFrame** from your Steam library.

To update, download the new tarball and run `frame/install.sh` again, then `setup.sh` again if it tells you the kit changed. Your game files, saves and settings stay where they are.

The installer changes nothing outside the folders listed under [Files](#files), the Steam shortcut and the controller models. To remove KartFrame, run `~/Games/KartFrame/app/tools/uninstall.sh`. It keeps your data folder unless you pass `--purge`.

## First run

The launch wrapper checks that the installed program was built for the installed kit. If it was not, it stops and writes the reason to the session log; run `setup.sh` again.

On the very first start the game creates its own settings file and a fresh Wii save area. The game makes the Wii licence in-game on first boot.

If the game cannot find the disc files, the setup did not finish. Run `setup.sh status` to see which step is missing.

[to confirm in the headset]

## Controls

The game sees a Wii wheel and pad. The Frame controllers are mapped like this.

| Steam Frame | In the game |
|---|---|
| Right trigger, **A** | Accelerate |
| Left trigger | Reverse |
| **B** | Brake, and back in menus |
| **X**, left bumper | Use item |
| **Y** | Look back |
| Both grips, right bumper | Drift |
| Left D-pad | D-pad |
| Right **Menu** | Start |
| Left **View** | Back. Held for half a second, it opens the VR settings panel. Pressed while the panel is open, it closes it |

Hand steering: in the first-person cockpit the grips hold the wheel, and the right bumper drifts there. Elsewhere the grips drift.

[to confirm in the headset]

## Settings

Hold left View for half a second. The VR settings panel opens. Everything on it is also in `~/.local/share/KartFrame/UserData/Config.toml`, which is seeded from `Config.frame.toml` on the first run. The keys that matter:

| Key | Default | What it does |
|---|---|---|
| `[vr] render_scale` | 1.0 | Eye size relative to the size SteamVR's resolution setting gives the game (1728 per eye at SteamVR's 100 %) |
| `[vr] frame_interpolation_fps` | 1 | 1 renders at the headset's refresh rate, interpolated from the game's 60 frames a second |
| `[vr] frame_pacing` | `"auto"` | `auto`, `inline` or `pipelined`. Auto picks pipelined |
| `[vr] refresh_rate` | 120 | The refresh rate the game asks for |
| `[vr] menu_shader_quality` | 0 | 0 is a plain dark background behind the menus |
| `[vr] hand_steering` | true | Grips hold the wheel in the first-person cockpit |
| `[vr] hud_virtual_screen` | true | Menus and the race HUD on a virtual screen |
| `[vr] hud_distance_meters` | 2.0 | How far away that screen is |
| `[vr] hud_width_meters` | 2.4 | How wide it is |
| `[video] resolution_multiplier` | 1.0 | Resolution of the flat desktop mirror |

I do not ship a default below 1728 per eye. Lower values of `render_scale` work, but the picture gets soft.

Switches for testing go in `~/.local/share/KartFrame/session.env`, one `NAME=value` per line, read at the next launch only:

| Name | What it does |
|---|---|
| `KARTFRAME_PACING` | `auto`, `inline` or `pipelined`, overriding the config |
| `KARTFRAME_REFRESH_HZ` | 90 or 120, the rate asked of SteamVR |
| `KARTFRAME_SYSMEM` | 0 turns off the GPU's direct memory rendering mode (`TU_DEBUG=sysmem`); it is on by default |
| `KARTFRAME_FDM_LAYER` | 0 disables Valve's foveation layer in the Vulkan chain, to compare |
| `KARTFRAME_HOME` | The data folder, if it is not `~/.local/share/KartFrame` |

[to confirm in the headset]

## Performance

[to confirm in the headset]

What the build does, so you know what the numbers will mean:

- At 120 Hz there are about 8.3 ms per refresh, and about 7 ms of that is the GPU's for both eyes.
- Each refresh locates the head once and renders both eyes from that pose.
- Pipelined pacing overlaps one refresh of CPU work with the previous refresh's GPU work, at the price of one refresh more latency. Inline mode waits for the eyes in the same refresh.
- An eye pair is handed to SteamVR only after the GPU has finished it. If a pair is not finished in time, the previous pair is shown again and the miss is counted.
- Every second, the game logs one `vr pacing:` line with the refresh rate, the sets rendered, interpolated, late and missed, and GPU and encode times. `report.txt` in each session folder summarises them together with SteamVR's compositor timing.

## Files

| What | Where |
|---|---|
| Program, wrapper, kit, toolchain, translator | `~/Games/KartFrame/app` |
| Game files, built game, saves, settings | `~/.local/share/KartFrame` (`game/`, `build/`, `bin/`, `UserData/`) |
| Controller models | `~/.local/share/KartFrame/UserData/controller_models/frame` |
| Session logs | `~/.local/share/KartFrame/sessions/<date-time>/` (`launch.log`, `stdout.log`, `game.log`, `vrtiming.log`, `report.txt`); `sessions/latest` points at the newest |
| Downloaded tools | `~/.cache/kartframe` (nodtool, fetched by `setup.sh`) |
| One launch's test settings | `~/.local/share/KartFrame/session.env` |

The game does not write to `~/.local/share/WiiCompiled`.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again.
- **The game does not start and `launch.log` says it refuses.** Read the reason in `~/.local/share/KartFrame/sessions/latest/launch.log`. If it names a kit mismatch, run `setup.sh` again.
- **`setup.sh` says the disc is not Mario Kart Wii RMCP01.** Only the clean PAL disc is supported.
- **SteamVR's per-app settings were not applied.** `launch.log` lists each one and what SteamVR holds after the write. They are only set when the game is started from the Steam library.
- **Motion is uneven, or the picture ghosts when you turn your head.** [to confirm in the headset]
- **Menus flicker.** [to confirm in the headset]

## Reporting a problem

Open an [issue](https://github.com/simplysanatori/KartFrame/issues) with what you did, what you expected and what happened, and the output of:

```sh
~/Games/KartFrame/app/tools/collect-logs.sh
```

It gathers the newest session's logs into one archive and masks IP and MAC addresses, host names, network names, serial numbers, Steam ids, email addresses and user names in paths. The masking is a best effort. Read the files before you attach them.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md). The patched Dawn is built once on the Frame, then the game and the release kit. `frame/tools/privacy-check.sh` checks the tree, the history and a release tarball for personal data before publishing, and `frame/boundary-check.sh` checks that nothing from the disc is in a release.

## Support

KartFrame exists because of heurazy's [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS). If you want to support someone, support them first: star and follow it, report what you find there, and send improvements to the game upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). It pays for the Claude API credits these ports are built with, spent as carefully as I can.

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori).
- [Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS) by heurazy, which this is built on, and heurazy's [Mario Kart Wii VR port](https://github.com/heurazy/mario-kart-wii-VR-port) before it.
- [Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR) by iChris4, the OpenXR port of WiiCompiled that Wiicompiled VR PLUS started from, and the WiiCompiled translator that turns the Wii game into C++.
- [aurora](https://github.com/encounter/aurora), the renderer, and [Dawn](https://dawn.googlesource.com/dawn), the WebGPU implementation under it.
- [SDL3](https://github.com/libsdl-org/SDL) and the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK).
- [nod and nodtool](https://github.com/encounter/nod) by encounter, which extracts the disc.
- [LLVM](https://llvm.org/), the compiler bundled for the first run, and the [.NET runtime](https://github.com/dotnet/runtime), which the translator carries.
- Valve, for the Frame controller models. They are converted on your Frame from your own SteamVR files and never redistributed.
- bigmak94's [GalaxyQuest](https://github.com/bigmak94/GalaxyQuest) and the GalaxyFrame work, for what I learned about pacing for SteamVR and for the test tools (`xrmock`, and `xrprobe`, GPLv3).

[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) has the full list with licences.

Mario Kart Wii, Nintendo and their logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve or the upstream author.

## Licence

GPLv3, as upstream: see [LICENSE](LICENSE). Third-party parts keep their own licences: see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/` folder. The test probe in `frame/tools/xrprobe` is GPLv3 and is not part of the game. No Nintendo game data is included.

Upstream's own README is kept as [README.upstream.md](README.upstream.md).

## How this was made

I wanted Mario Kart Wii on my Frame without Lepton, so I started from Wiicompiled VR PLUS and had it built natively for SteamVR: a Linux Arm64 build, Dawn's Vulkan backend on SteamVR's own device, pacing written for how SteamVR behaves, the Frame controller layout in code, and a first run that builds the game from your disc on the Frame. It started as a project for my own headset.

This is vibe coded, with manual review throughout. I wrote it with Claude Code, using Anthropic's Fable 5.1, Opus 5.5 and Sonnet 5.5 models running on the Frame itself. I chose what to build, reviewed the code and the scripts as they were written, tested in the headset and reported what I saw; Claude read the logs and changed the code. The upstream author has not reviewed this port. If that is not for you, no worries. Read the code before trusting it; it is a starting point for anyone who wants a native game on the Frame.

Developer notes and what was learned about SteamVR on the Frame are in the [technical notes](docs/TECHNICAL_NOTES.md).
