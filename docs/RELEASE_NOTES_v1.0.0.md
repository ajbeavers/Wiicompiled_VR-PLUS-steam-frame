# KartFrame 1.0.0

Mario Kart Wii in VR, running natively on the Valve Steam Frame. KartFrame is a fork of heurazy's Wiicompiled VR PLUS with a native Linux Arm64 build, Vulkan on SteamVR's own device and pacing written for SteamVR. You need your own PAL disc (RMCP01); no game data is included.

### The game on the Frame

- A native Linux Arm64 build that runs in the Frame's SteamVR. No Lepton, APK, adb or PC.
- The game is heurazy's static translation of Mario Kart Wii to C++, drawn by aurora on Dawn (WebGPU) and shown through OpenXR.
- Game files, saves and settings in plain folders under `~/.local/share/KartFrame`.

### Rendering

- Dawn's Vulkan backend bound to SteamVR's own device, through upstream's Dawn patch.
- Eye images handed to SteamVR only after the GPU has finished them.
- Both eyes rendered on every refresh of the 120 Hz display from that refresh's head pose, at 1728 pixels per eye, with the game's own VR frame interpolation filling the display rate.
- Pipelined frame pacing, with the single stage mode as a switch.
- The game's 2D screen shown as a flat quad layer in front of you. [to confirm in the headset]

### Controls

- The Frame controller layout in code, through the Frame's own interaction profile. No SteamVR binding file. [to confirm in the headset]
- Simplified models of the Frame controllers, converted at install from the Frame's own SteamVR files and never redistributed.
- Grips hold the wheel in the first-person cockpit.

### Install

- A first run that extracts your disc, translates the game and compiles it on the Frame with a bundled compiler, from a kit of prebuilt runtime parts. About 6 minutes on my Frame.
- An installer that unpacks the program, adds the Steam library entry and converts the controller models.
- A launch wrapper that sets SteamVR's per-app settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) at every launch, and writes a session folder with the logs and a report.
- Tools to collect logs for a bug report and to remove the program again.

## Download

- `kartframe-1.0.0-linux-arm64.tar.gz`
- `kartframe-1.0.0-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, then run `frame/install.sh` with the tarball as its argument, then `~/Games/KartFrame/app/setup.sh` with your disc image. The [README](https://github.com/simplysanatori/KartFrame/blob/native/README.md) has the full steps, the controls and the settings.

## Known

[to confirm in the headset]

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/KartFrame/app/tools/collect-logs.sh`.
