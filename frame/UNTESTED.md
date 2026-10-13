# Built but not confirmed in the headset

One line per claim. The documents carry `[to confirm in the headset]` at each of these. When the owner has tested one, delete its line here and complete the text in README.md, docs/RELEASE_NOTES_v1.0.0.md, docs/CHANGELOG.md and docs/TECHNICAL_NOTES.md.

The first headset session showed judder, half rate flapping and flickering menus. Fixes are in progress, so nothing below has a verdict yet.

## Status and performance

- The overall verdict: the game starts from the Steam library and plays a race through SteamVR.
- 120 Hz with new eyes on every refresh at 1728 per eye.
- Smooth head motion: no 60 Hz judder, no ghosting on turns.
- The pipelined default, and that it beats inline in the headset.
- Whether the every second refresh fallback is off or on by default, and how it behaves.
- Any performance number: GPU time per eye set, late, missed and repeated sets, hitches at race start, memory.
- TU_DEBUG=sysmem on by default: the speed gain and that it causes no visible problem with translucent overlays.
- Valve's foveation layer on or off (KARTFRAME_FDM_LAYER) making no visible difference.
- Menus and the intro screens at the display rate.
- The refresh rate: 120 Hz reached, no 90 Hz request from the game first.

## Screen and menus

- The 2D screen as a flat quad layer in front of the player: placement, size, no flicker.
- hud_virtual_screen, hud_distance_meters and hud_width_meters at their defaults.
- The race HUD.
- The item box (roulette) drawing in the right place.
- The controller guide and the tutorial screens.

## Controls

- The whole layout: right trigger and A accelerate; left trigger reverse; B brake and back; X and left bumper item; Y look back; both grips and right bumper drift; left D-pad; right Menu is Start.
- Left View: a tap goes back, held half a second opens the VR settings panel, pressed while open it closes it.
- Hand steering: the grips hold the wheel in the first-person cockpit, the right bumper drifts there.
- The simplified Frame controller models in the hands and in the tutorial.
- Controls staying live through a short loss of focus.
- The VR settings panel: aiming, and editing Config.toml.

## Install and first run

- install.sh adding the Steam shortcut through Steam's debugging port, and updating it.
- The per-app SteamVR settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) taking effect from a library launch, and the read back in launch.log.
- The sessions folder, report.txt and vrtiming.log from a real session.
- setup.sh from a disc image on a clean Frame, after the release tarball is installed from scratch.
- The kit check refusing to start after a kit change.
- First boot: the Wii licence made in-game, and a save kept between launches.
- collect-logs.sh and uninstall.sh on the real install.
