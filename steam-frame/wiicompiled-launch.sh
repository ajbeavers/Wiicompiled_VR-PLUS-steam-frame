#!/bin/bash
# Steam launch wrapper for the WiiCompiled VR shortcut (Lepton compatibility tool).
# Launch options: /home/<you>/Games/WiiCompiledVR-SteamFrame/wiicompiled-launch.sh %command%
#
# Lepton boots Android and opens the app's 2D launcher, which never shows up in the headset on
# the Frame. Once a game has been built this starts the game activity itself, so a launch goes
# straight into the race menus. Before that (a fresh install, or after an APK update cleared the
# app's data) it seeds Config.toml with the Frame settings and leaves the launcher running for the
# disc import and the build, which are driven over adb; see the README.
ADB=/usr/lib/android-sdk/platform-tools/adb
PKG=org.wiicompiled.quest
HERE=$(dirname "$(readlink -f "$0")")
LOG=/tmp/wiicompiled-launch.log
SAVED_LD_PRELOAD=$LD_PRELOAD
unset LD_PRELOAD   # Steam's overlay preload only spams errors for adb

GAME=
if [ -n "$STEAM_COMPAT_DATA_PATH" ]; then
    # Lepton mounts $STEAM_COMPAT_DATA_PATH/external as the container's shared storage and
    # internal/<package> as the app's private data.
    CFG=$STEAM_COMPAT_DATA_PATH/external/Android/data/$PKG/files/WiiCompiledOpenXRVR
    if [ ! -f "$CFG/Config.toml" ] && [ -f "$HERE/Config.frame.toml" ]; then
        mkdir -p "$CFG"
        cp "$HERE/Config.frame.toml" "$CFG/Config.toml" && chmod 664 "$CFG/Config.toml"
        echo "$(date +%T) seeded $CFG/Config.toml with the Frame settings" >> "$LOG"
    fi
    GAME=$STEAM_COMPAT_DATA_PATH/internal/$PKG/files/game/base/libmain.so
fi

(
    echo "$(date +%T) waiting for the $PKG launcher"
    for _ in $(seq 1 480); do
        sleep 0.5
        timeout 5 "$ADB" connect localhost:5555 >/dev/null 2>&1
        if timeout 5 "$ADB" -s localhost:5555 shell dumpsys window 2>/dev/null \
            | grep -q "mCurrentFocus=.*$PKG/.*LauncherActivity"; then
            if [ -n "$GAME" ] && [ ! -f "$GAME" ]; then
                echo "$(date +%T) no game built yet ($GAME missing): leaving the launcher up"
                exit 0
            fi
            if [ -n "$WCF_STAY_IN_LAUNCHER" ]; then
                echo "$(date +%T) WCF_STAY_IN_LAUNCHER set: leaving the launcher up"
                exit 0
            fi
            sleep 0.5
            timeout 10 "$ADB" -s localhost:5555 shell am start -n "$PKG/.QuestActivity"
            echo "$(date +%T) game activity started"
            exit 0
        fi
    done
    echo "$(date +%T) the launcher never came up"
) >"$LOG" 2>&1 &

[ -n "$SAVED_LD_PRELOAD" ] && export LD_PRELOAD=$SAVED_LD_PRELOAD
exec "$@"
