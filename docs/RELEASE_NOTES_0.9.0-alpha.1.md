# KartFrame 0.9.0-alpha.1 (pre-release)

Mario Kart Wii in VR, running natively on the Valve Steam Frame. KartFrame is a fork of heurazy's Wiicompiled VR PLUS with a native Linux Arm64 build, Vulkan on SteamVR's own device, and frame pacing written for SteamVR on the Frame. No Lepton, no APK, no PC.

This is a pre-release. It plays, at 120 Hz and 1728 pixels per eye, and has been through twenty headset sessions, but one problem is still being worked on (below), and a few smaller things are listed as known issues. Version numbers stay below 1.0.0 until that is done.

You need your own PAL Mario Kart Wii disc image. No game data is in this repository or its releases.

## What you get

- The game built on your Frame from your own disc, once, in about 6 minutes.
- A Steam library entry that starts it in SteamVR with the right per-app settings every time.
- Both eyes rendered on every refresh of the 120 Hz display from that refresh's head pose; races hold 119-120 sets a second with under 1 % late.
- The 2D menus on a flat, steady screen in front of you; the race HUD on the virtual screen.
- The Frame controllers, with the grips holding the wheel in the cockpit view.

## The known issue: the double image

In races, turning your head could show the world twice, one copy a little ahead of the other, worse in one eye. Twenty sessions of measurement found the mechanism: SteamVR's compositor on the Frame takes each eye's newest handed-over image at its own point inside the refresh, the right eye after the left, and a hand-over landing near those points made consecutive refreshes disagree. This build hands every frame over at a fixed point against the compositor's own clock (`KARTFRAME_END_PHASE=after`, the launch wrapper's default), which removes the doubling. What remains: the right eye runs one set ahead of the left, which reads as the right eye being a touch more sensitive to head movement, and the compositor now and then shows a frame twice. The README explains it in full; the cause behind the cause is still being chased.

## Other known issues

- The Time Trial ghost is drawn flat.
- Full-view 2D effects (Blooper ink, the black of a respawn) do not cover the whole view.
- The laser pointer's dot is not visible on the 2D menu screen; the ray is.
- Two and four player races are heavier and not yet measured in the headset.

## Download

- `kartframe-0.9.0-alpha.1-linux-arm64.tar.gz`
- `kartframe-0.9.0-alpha.1-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, run `frame/install.sh` with the tarball as its argument, then `~/Games/KartFrame/app/setup.sh` with your disc image. The [README](https://github.com/simplysanatori/KartFrame#install) has the steps. Updating is the same two commands; your game files, saves and settings stay where they are.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/KartFrame/app/tools/collect-logs.sh`. It masks addresses, names and ids before anything leaves your Frame.
