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

READY=   # set once a game built against this APK's kit is installed
REASON="no STEAM_COMPAT_DATA_PATH"
if [ -n "$STEAM_COMPAT_DATA_PATH" ]; then
    # Lepton mounts $STEAM_COMPAT_DATA_PATH/external as the container's shared storage and
    # internal/<package> as the app's private data. The latter survives an APK update, so a game
    # built against another kit can be sitting there: the app calls it stale and will not run it.
    CFG=$STEAM_COMPAT_DATA_PATH/external/Android/data/$PKG/files/WiiCompiledOpenXRVR
    if [ ! -f "$CFG/Config.toml" ] && [ -f "$HERE/Config.frame.toml" ]; then
        mkdir -p "$CFG"
        cp "$HERE/Config.frame.toml" "$CFG/Config.toml" && chmod 664 "$CFG/Config.toml"
        echo "$(date +%T) seeded $CFG/Config.toml with the Frame settings" >> "$LOG"
    fi
    # Steam Frame controller models (made by install.sh from SteamVR's own) in place of the Touch Plus
    # ones. The app unpacks its resources again after an APK update, so after an update the Frame
    # models show from the second launch on.
    RES=$STEAM_COMPAT_DATA_PATH/internal/$PKG/files/runtime_resources/quest_touch_plus
    if [ -d "$RES" ] && [ -f "$HERE/frame_controller_models/right.wccontroller" ]; then
        for f in left.wccontroller left.rgba right.wccontroller right.rgba; do
            cmp -s "$HERE/frame_controller_models/$f" "$RES/$f" || cp "$HERE/frame_controller_models/$f" "$RES/$f"
        done
    fi
    MANIFEST=$STEAM_COMPAT_DATA_PATH/internal/$PKG/files/game/base/game.json
    APK=$(ls "$HERE"/app/*.apk 2>/dev/null | head -1)
    if [ ! -f "$MANIFEST" ]; then
        REASON="no game built yet"
    elif [ -z "$APK" ]; then
        REASON="no APK next to this script to compare the kit with"
    else
        KIT=$(unzip -p "$APK" assets/game_kit/kit.json 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["fingerprint"])' 2>/dev/null)
        BUILT=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("kitFingerprint", ""))' "$MANIFEST" 2>/dev/null)
        if [ -n "$KIT" ] && [ "$KIT" = "$BUILT" ]; then
            READY=1
        else
            REASON="the built game is for another kit (${BUILT:0:12} vs this APK's ${KIT:0:12}); it needs building again"
        fi
    fi
fi

(
    echo "$(date +%T) waiting for the $PKG launcher"
    for _ in $(seq 1 480); do
        sleep 0.5
        timeout 5 "$ADB" connect localhost:5555 >/dev/null 2>&1
        if timeout 5 "$ADB" -s localhost:5555 shell dumpsys window 2>/dev/null \
            | grep -q "mCurrentFocus=.*$PKG/.*LauncherActivity"; then
            if [ -z "$READY" ]; then
                echo "$(date +%T) leaving the launcher up: $REASON"
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
