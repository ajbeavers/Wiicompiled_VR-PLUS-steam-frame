# Mario Kart Wii VR on the Steam Frame

This is a fork of [heurazy/Wiicompiled_VR-PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS),
the Mario Kart Wii VR port, that runs standalone on Valve's **Steam Frame** through Lepton (the
Frame's Android compatibility tool). Upstream's Quest APK installs on the Frame but crashes the
moment the game starts; with the changes here it runs at 90 Hz with the Frame controllers laid out
like a normal gamepad, straight from the Steam library, no PC involved.

The branch to use is `steam-frame`. Everything the fork adds lives in [`steam-frame/`](steam-frame/)
plus five small commits to `runtime/` and `aurora-main/`. Upstream's own README is kept as
[README.upstream.md](README.upstream.md); read it for what the port itself does, this file is only
about the Frame.

**Status, October 2026:** working on my Steam Frame (SteamOS 0.4.5, Lepton 2.8.11, SteamVR), in
the original and first-person cameras, single player. See [What works and what doesn't](#what-works-and-what-doesnt)
before you spend an afternoon on it.

## The honest part: an AI wrote this

I did not write the code in this fork, and I did not write this README either. Both were produced
by Anthropic's Claude (the models called **Claude Opus 5.5** and **Claude Fable 5.1**), running
through Claude Code on the Steam Frame itself over SSH. What I did was decide what to try, launch
every build in the headset, play it, and report back what I saw: "it's a flickering mess", "the
menus are a slideshow", "the item box stays on screen", "that resolution change made it stutter,
roll it back". The AI read the logs, read the code, made the change, and we went round again.
Everything in the status and testing sections below comes from those rounds on real hardware, not
from reasoning about the code.

So: this is AI-generated code, by someone who could not have written it by hand, published because
it works and because I think it proves something worth proving, that a Wii game can run natively in
VR on a Steam Frame at 90 Hz without streaming. Treat it accordingly. Read the diffs before you trust
them. Nobody from the upstream project has reviewed any of it, and I asked the AI to say so plainly
rather than dress it up. The [How this was made](#how-this-was-made) section at the end has the
longer version, including the things it got wrong along the way.

If you want a project that is honest about the same thing in a much funnier way, read the README of
[baijiSteamVR](https://github.com/thejerf/baijiSteamVR); it is where I took the idea of just saying
it out loud.

## What is different from upstream

Upstream ships three things: a Windows build, a Quest APK, and the design where **your game is
compiled on your own device from your own disc**: the APK carries a "game kit" (precompiled runtime
objects, headers, a toolchain) and the headset translates and links `libmain.so` from your disc
against that kit. No Nintendo code is ever in the APK or in this repository, and that stays true
here.

This fork changes the kit, not the design. The Steam Frame APK is upstream's v1.3.1 Quest APK with
four kit files swapped for ones compiled from the patched sources, one patched `classes.dex`, and a
new kit fingerprint. When the headset builds your game it links it against the Frame objects, so you
get the Frame version from the same disc import and build that upstream documents.

What the patches do, in the order they were needed:

1. **It starts.** Upstream's app registers a clipboard listener when SDL comes up. Lepton's Android
   image has no clipboard service, the lookup returns null, and the game process dies with a
   `NullPointerException` before it draws anything. The ten bytes of that call are replaced with
   nops in `classes.dex` ([`steam-frame/dexpatch.py`](steam-frame/dexpatch.py)). That is the whole
   reason the stock APK "doesn't work" on the Frame.

2. **90 Hz.** Upstream deliberately disables VR frame interpolation on Android (its README says so:
   it "makes the standalone presentation unstable" on Quest). On the Frame that leaves the game
   presenting its 60 fps simulation into a 90 Hz headset, which looked like judder and ghosting
   whenever I turned my head. The Android runtime overrides that forced it off are removed, so
   Aurora's stereo replay renders the eyes at the headset's refresh rate with a fresh head pose each
   frame, from the game's 60 fps. SteamVR's OpenXR inside Lepton has no
   `XR_KHR_convert_timespec_time`, which the pacing needs, so there is a `CLOCK_MONOTONIC` fallback
   that anchors on the first frame. (`runtime/src/vr/openxr_integration.cpp`;
   `setprop debug.wiicompiled.vr_interp 0|1|72|90|120` overrides it per launch.) Races hold 90 fps
   on the counter, with frames submitted well inside the budget. Not perfectly smooth, see below.

3. **Frame controllers.** The game's actions are hand-specific OpenXR actions written for Quest Touch
   and SteamVR had auto-mapped them onto the Frame controllers so that A did three things and B, X
   and Y did nothing. The runtime now also creates hand-independent actions (`frame_item`,
   `frame_drift`, `frame_look_back`, `frame_start`, `frame_back`, a D-pad), a SteamVR binding file
   assigns the Frame's buttons to them, and when SteamVR reports them bound the game switches to a
   gamepad-style layout, the same in every camera (no cockpit "A becomes drift" swap). The layout
   latches for the session, because SteamVR briefly reports the actions inactive when a controller
   dozes and you do not want the controls changing mid-race. (`runtime/src/vr/openxr_input.cpp`,
   [`steam-frame/bindings_frame_controller.json`](steam-frame/bindings_frame_controller.json).)

4. **Menus at 90 instead of 40.** Races were fine but the menus, which upstream draws on a spatial
   panel with an animated "dielectric" raymarched background, ran at 37 to 45 fps with each frame
   taking 15 to 24 ms to submit. Turning the background quality to 0 upstream would have moved the
   menus to a quad layer, which under Lepton's SteamVR bridge is a slow submission of its own. On
   Android the menus are now always drawn into the eye images, and quality 0 means a plain dark
   background, which costs nothing. (`openxr_integration.cpp`; `menu_shader_quality = 0` in the
   config.)

5. **The item box.** In VR the item roulette slid up after use and stayed visible. Aurora's
   virtual-screen HUD draws ignore the game's scissor rectangle; the port now skips a 2D draw that
   lies entirely outside a scissor the game narrowed, which is what hides it on a TV.
   (`aurora-main/lib/gfx/common.cpp`.) During the slide itself a sliver can still show.

6. **Settings that suit the Frame**, seeded into `Config.toml` on the first launch by the wrapper
   ([`steam-frame/Config.frame.toml`](steam-frame/Config.frame.toml)): interpolation on auto, menu
   background quality 0, hand steering off (the grips are drift on this layout and kept grabbing the
   wheel in first person), render scale 1.0 and resolution multiplier 1.0. In SteamVR the app gets
   `preferredRefreshRate 90`, `framesToThrottle 0` and `additionalFramesToPredict 0`; those went in
   together with the interpolation and I have not tested them one at a time.

7. **A launch wrapper**, because the app's 2D launcher never shows up in the headset on the Frame
   (SteamVR does not pick up Lepton's flat window for this app). Once a game built against this
   APK's kit is installed (it compares the kit fingerprints), the wrapper waits for the launcher to
   come up and starts the game activity itself, so a launch from the Steam library goes straight to
   the title screen in about ten seconds.

Everything else, the cameras, the hand IK, the HUD, Retro Rewind support, is upstream's work and is
unchanged.

## What works and what doesn't

Confirmed on my Frame, in the headset:

- Launching from the Steam library into the game, title screen to race, at 90 Hz.
- Racing in the original camera; the first-person cockpit, briefly; the menus at full rate; the
  VR settings panel; the controller layout in the table below, including pause and back.
- The item box fix is in the build I play and I have not seen the box stuck since; it had the
  least testing of the five changes.

Known problems and things I have not tested:

- **The release APK has not yet been through a clean run.** What I play is the same four objects,
  linked into `libmain.so` on the Frame by the AI's host-side build. The APK on the Releases page
  was made by `build.sh` from those objects and passes the same archive, alignment and signature
  checks as upstream's APK, but the full install, disc import and on-headset build with it is the
  next thing I do, and this line changes when it has happened. If you get there first, an issue
  with the build log would be welcome.
- **Subtle slowdowns remain.** Races are a solid 90 on the counter, but there are brief hitches
  now and then that I notice and cannot pin to anything in the logs. Good enough that I play it;
  not perfect.
- **The 2D launcher is invisible** in the headset, so the one-time disc import and build are done
  from a terminal with two commands (below). Upstream's "Select disc image" button needs a document
  picker Lepton does not have anyway.
- **Resolution.** I tried 1.5x game resolution with 2160x2160 eyes for sharper menu text. It
  looked better and dropped frames all over the place (10 to 24 ms late, 30 to 50 ms stalls), so
  it went back to 1.0x and 1728x1728, which is what the installer sets. If you want sharper menus,
  make the menu screen bigger or closer in VR settings instead.
- **Item roulette** can still show a sliver while sliding.
- **Not tested:** Retro Rewind, online play, the diorama camera for more than a minute, USB
  wheels, multiplayer, passthrough (there is nothing to pass through to; the Frame shows SteamVR's
  void around the menus), hand tracking.
- **Not for Quest.** The kit objects are compiled for Android arm64 and would run on a Quest, but
  the interpolation change is exactly what upstream turned off there because it misbehaved on
  Quest, and the controller layout only activates when SteamVR binds the Frame actions. Use
  upstream's APK on a Quest.
- **Updates.** This is built on upstream v1.3.1. A newer upstream APK has a different kit, and
  `build.sh` refuses to make an APK when the objects it compiled do not export the same symbols as
  the ones in the APK; the patches will need porting when upstream moves.

## Controls

The binding is installed per app by `install.sh`; the game picks the layout up by itself.

| Steam Frame | In the game |
| --- | --- |
| Left stick | Steer, navigate menus |
| Right trigger, or A | Accelerate / confirm |
| Left trigger, or B | Brake, reverse / cancel |
| X, or left bumper | Use or hold item |
| Y | Look back |
| Grips, or right bumper | Hop and drift |
| D-pad | D-pad (up is a trick) |
| Right stick directions | Tricks and wheelies |
| Right stick click | Cycle the three cameras |
| Menu (right) | Start / pause |
| View (left), tap | Back |
| View (left), hold half a second | VR settings panel (press again to close) |

The left View tap is sent on release so that a hold never also goes back. Steam Input and SteamVR's
binding UI are the right place to change any of this: the file is a normal SteamVR binding and shows
up under Manage Controller Bindings for the app.

## Setting it up on a Steam Frame

You need: a Steam Frame; **Lepton** installed from Steam (Library > Tools); your own clean PAL
`RMCP01` Mario Kart Wii disc image (I used an ISO; upstream's launcher also accepts RVZ, WBFS and
the others it lists); about 5 GB free; and a terminal on the Frame: Konsole in desktop mode, or SSH
from another machine with developer mode on. Nothing needs root and nothing is installed with pacman.

1. Get the repository and the APK:

   ```sh
   git clone -b steam-frame https://github.com/simplysanatori/Wiicompiled_VR-PLUS-steam-frame.git
   cd Wiicompiled_VR-PLUS-steam-frame
   ```

   Download `WiiCompiled-VR-SteamFrame-*.apk` from the
   [Releases](https://github.com/simplysanatori/Wiicompiled_VR-PLUS-steam-frame/releases) page, or build
   it yourself (next section). The APK contains no game data, exactly like upstream's.

2. Install it, with Steam running:

   ```sh
   steam-frame/install.sh ~/Downloads/WiiCompiled-VR-SteamFrame-*.apk
   ```

   This copies the APK and the launch wrapper to `~/Games/WiiCompiledVR-SteamFrame/`, adds a
   **Mario Kart Wii VR** entry to the Steam library set to the Lepton compatibility tool (through
   Steam's own client API, so no editing of `shortcuts.vdf` behind Steam's back), and writes the
   controller binding and the 90 Hz settings into SteamVR for that entry. It prints the entry's
   appid; you need it for the next step. Running it again later updates the same entry.

3. Put the disc image in `~/Documents/` on the Frame (Lepton shows that folder to the app as
   `/sdcard/Documents`).

4. Put the headset on and launch **Mario Kart Wii VR** from the library. The first launch takes a
   minute while Lepton installs the APK. You will see SteamVR's empty space and nothing else: the
   app's launcher is running but cannot be seen. Leave it running.

5. From the terminal, start the disc import, then the build. `<appid>` is the number from step 2
   and `MarioKart.iso` is your file's name:

   ```sh
   LEPTON=~/.local/share/Steam/steamapps/common/Lepton/lepton
   $LEPTON exec steamlaunch-<appid> am start-foreground-service -n org.wiicompiled.quest/.launcher.GameSetupService \
       -d file:///sdcard/Documents/MarioKart.iso --es task ExtractDisc
   ```

   This checks the disc (it must be the clean PAL game; anything else is rejected) and extracts
   it into the app's data folder, a few minutes. Then:

   ```sh
   $LEPTON exec steamlaunch-<appid> am start-foreground-service -n org.wiicompiled.quest/.launcher.GameSetupService \
       --es task BuildGame
   ```

   The headset now translates and compiles the game against the Frame kit. It took about a
   quarter of an hour on my Frame (upstream quotes half an hour on a Quest 3). Progress goes to
   `~/.local/share/Steam/steamapps/compatdata/<appid>/external/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/Logs/build_*.log`,
   and the extraction and build both log through `adb`:

   ```sh
   /usr/lib/android-sdk/platform-tools/adb connect localhost:5555
   /usr/lib/android-sdk/platform-tools/adb -s localhost:5555 logcat | grep -iE 'GameSetup|DiscExtract|GameBuild|WiiCompiled'
   ```

6. When the build log ends with the game installed, quit the app (Steam button > exit) and launch
   it again. From now on every launch goes straight into the game. The first time, the game asks
   you to pick a camera with the pointer; that choice and everything in the VR settings panel are
   saved to `Config.toml`.

If you already had upstream's APK set up on the Frame under the same library entry name, step 2
repoints that entry. On the next launch Lepton reinstalls the app because the APK changed; the
extracted disc files and the previously built game both survive, but that game was built against
upstream's kit and the app will not run it (the launcher calls it stale, and the wrapper sees the
fingerprint mismatch and leaves the launcher up). Run the `BuildGame` command from step 5; the
extraction is not needed again.

## Building the APK yourself

```sh
steam-frame/build.sh                  # downloads upstream's v1.3.1 APK and checks its SHA-256
steam-frame/build.sh path/to/WiiCompiled-VR-Quest3-v1.3.1.apk
```

On the Frame this takes about 20 seconds once the toolchain is there, and produces
`steam-frame/release/WiiCompiled-VR-SteamFrame-<git tag>.apk`. It needs `curl`, `tar`, `python3`
and `openssl`, all on SteamOS already, and no root. What it does:

1. Fetches Android NDK r29 into `~/.local/opt/android-build/` (Google ships no arm64 Linux NDK, so
   on the Frame it uses [HomuHomu833's r29 build](https://github.com/HomuHomu833/android-ndk-custom);
   on x86_64 it would use Google's r29, untried). It has to be r29, the NDK the kit was built with:
   the first release APK was compiled with r30, whose libc++ headers emit a helper
   (`std::__hash_memory`) that r29's library lacks, and the headset's link failed on it. The build
   now also refuses any object that imports a C++ symbol the kit's original does not.
2. Compiles the two runtime unity groups that contain the patched files and Aurora's `common.cpp`
   with the same flags upstream's Android build uses. The third-party headers those three units need
   are vendored in [`steam-frame/build-deps/`](steam-frame/build-deps/) (see its `NOTICE.md`),
   because upstream fetches its dependencies with CMake on Windows and this build runs no CMake.
3. Extracts the matching objects from the upstream APK's kit and checks that each new object exports
   exactly the same symbols and imports no C++ symbol the original did not. If not, it stops: the
   sources and the APK are out of step, or the toolchain is wrong.
4. `make-apk.py` rewrites the APK: the four kit files (`007_unity_runtime_4`, `001_unity_runtime_6`,
   `020_libaurora_gx.a` with its `common.cpp.o` swapped, and `include/vr/openxr_input.h`), the new
   kit fingerprint in `kit.json`, the patched `classes.dex`, zipalign-equivalent alignment, and an
   APK Signature Scheme v2 signature made with `openssl`. No Java, apksigner or zipalign. The signing
   key is generated in `steam-frame/signing/` on first use; keep it, an APK signed with another key
   will not install over the previous one. `make-apk.py verify file.apk` checks any APK's signature.

The game itself (`libmain.so`) is never built here. It needs upstream's translator and your disc,
and the headset does that part. For what it is worth, during development the AI also linked
`libmain.so` on the Frame directly against the patched objects, using a translation produced by
upstream's .NET translator on the Frame; that is how the changes were iterated on in minutes. Those
tools and that translation output are not in this repository (the translator is upstream's, the
translation is Nintendo's code), so the on-device build is the path for everyone else.

## Where things are, for digging

- Steam library entry: a non-Steam shortcut whose executable is the APK and whose compatibility
  tool is `lepton`; `lepton-show-flatscreen` next to the APK would show the 2D window if SteamVR
  ever picked it up. Launch options run `~/Games/WiiCompiledVR-SteamFrame/wiicompiled-launch.sh`.
- The app's data: `~/.local/share/Steam/steamapps/compatdata/<appid>/`. `external/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/`
  has `Config.toml`, `DATA` (the extracted disc), saves and `Logs/` (one folder per run with the
  game's `console.log`, plus the build logs). `internal/org.wiicompiled.quest/files/game/base/` has
  the built `libmain.so` and its `game.json`.
- Lepton's log for the launch: `~/.local/share/Steam/logs/lepton-steamlaunch-<appid>.log`; the
  wrapper's: `/tmp/wiicompiled-launch.log`.
- SteamVR's per-app settings are the `steam.app.<appid>` section of
  `~/.config/openvr/config/steamvr.vrsettings`; `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-int steam.app.<appid>.preferredRefreshRate 90`
  is how the installer writes them while SteamVR runs.
- `[diagnostics] openxr_logging = true` in `Config.toml` makes the game log frame timing to its
  `console.log` (late frames, stalls); `setprop debug.wiicompiled.fpslog 1` over adb before the game
  starts logs the frame rate. Both were how the menu and resolution problems above were found.
- Lepton's Android reaches the host's `~/Documents`, `~/Downloads` and `~/Videos` as
  `/sdcard/Documents`, `/sdcard/Download` and `/sdcard/Movies`. `lepton exec steamlaunch-<appid> <command>`
  runs a command inside the running container; `adb connect localhost:5555` reaches it too.

## How this was made

Over about a week in October 2026, in a series of Claude Code sessions on the Frame. The rough order:

- The first attempt, a few days earlier, got upstream's v1.3 Quest APK installed through Lepton,
  found the clipboard crash in a logcat dump and patched it, drove the invisible launcher over adb
  to import the disc, and got the game running. It was, in my words at the time, "a flickering
  mess with shaky head tracking, vomit inducing". The AI tried a long list of SteamVR settings
  (refresh rates, motion smoothing on and off, resolution overrides); one of those left the image
  warped in a corner, and the actual cause of a lot of it turned out to be that the in-game panel
  had quietly written foveation, adaptive resolution and passthrough into `Config.toml`.
- I then asked whether the PC version through Proton would be better. The AI cross-compiled
  upstream's Windows build on the Frame and ran it under Proton for ARM64 in VR: it worked and
  flickered because x86 emulation could not keep up (27 ms frames). It then wrote a native Linux
  ARM64 OpenXR port, which also worked and which I did not want either. Both were deleted at my
  request once the Android build was good; this fork is the Android path only.
- The real fix was reading upstream's runtime and finding that frame interpolation was compiled
  out on Android. The AI rebuilt the two runtime objects with that enabled, found that SteamVR under
  Lepton has no clock conversion extension and wrote the fallback, re-signed the APK, and the result
  was "very good". Then the controls, in code rather than only in the binding file because the
  game reads X and Y as the left hand's buttons. Then the menus, after the frame-timing diagnostics
  showed exactly which draw was slow. Then the item box.
- The last change before publishing was a mistake: raising the resolution for sharper menus,
  which I reported as a performance regression the next day and which was rolled back. The
  numbers in the "Resolution" note above are from that log.
- This README was written by the AI from the session logs and its memory notes, at my request and
  in my voice, and I read it before publishing. If something in it is wrong, it is because I
  missed it; open an issue with the Lepton log and the game's `console.log` attached.

I will look at issues and pull requests, but I am one person with one headset and no ability to
fix emulation-level problems; the useful contribution here, as the Baiji README puts it better
than I can, is **testing**. If you make a change, say what you tested it on and how.

## Licence and credits

GPL-3.0, as upstream (`LICENSE`). The fork's own additions are under the same licence. Third-party
headers vendored for the build are listed with their licences in
[`steam-frame/build-deps/NOTICE.md`](steam-frame/build-deps/NOTICE.md).

The port is [heurazy's Wiicompiled VR PLUS](https://github.com/heurazy/Wiicompiled_VR-PLUS), itself
built on [iChris4's Wiicompiled VR](https://github.com/iChris4/Wiicompiled_VR) and the WiiCompiled
static recompilation project, on [Aurora](https://github.com/encounter/aurora), Dawn, SDL3 and
OpenXR; the credits in [README.upstream.md](README.upstream.md) are theirs and they did the hard
part. Lepton is Valve's. Mario Kart Wii is Nintendo's; nothing of theirs is in this repository or
in the APK, and you need your own disc.
