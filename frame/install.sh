#!/bin/bash
# Installs KartFrame on the Steam Frame: the program into ~/Games/KartFrame/app, the launch wrapper
# and tools beside it, and a non-Steam shortcut in Steam's library (through Steam's own client API,
# so Steam must be running; no editing of shortcuts.vdf behind its back).
#
#   frame/install.sh [options] [kartframe-*-linux-arm64.tar.gz | BUILD_DIR]
#   frame/install.sh --dev [BUILD_DIR]     a separate "KartFrame (dev)" shortcut that runs the build
#                                          tree's binary in place (default <repo>/build-linux)
#
#   --name NAME     shortcut name (default "KartFrame", with --dev "KartFrame (dev)")
#   --art DIR       optional library art: p.png (portrait), wide.png, hero.png, logo.png, icon.png
#   --no-models     do not convert the Frame controller models
#   -h, --help
#
# Without a source: the newest release tarball in ./release, else <repo>/build-linux.
# A BUILD_DIR is a CMake build directory: kartframe plus the resources next to it.
#
# Environment:
#   KARTFRAME_APP=DIR        where the program goes (default ~/Games/KartFrame/app; dev: app-dev)
#   KARTFRAME_HOME=DIR       data folder (default ~/.local/share/KartFrame); only created here
#   KARTFRAME_SKIP_STEAM=1   no Steam shortcut at all (Steam need not be running)
#   KARTFRAME_STEAM_DIR=DIR  edit the shortcuts.vdf files of this (copied) Steam folder offline
#                            instead of the live one: DIR/userdata/*/config/shortcuts.vdf, grid art
#                            beside it. Used for dry runs; nothing of the real Steam is touched.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
DEV=0; NAME=; ART=; MODELS=1; SRC=
while [ $# -gt 0 ]; do
    case $1 in
        --dev) DEV=1 ;;
        --name) NAME=$2; shift ;;
        --art) ART=$2; shift ;;
        --no-models) MODELS=0 ;;
        -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) SRC=$1 ;;
    esac
    shift
done
if [ "$DEV" = 1 ]; then
    NAME=${NAME:-KartFrame (dev)}
    APP=${KARTFRAME_APP:-$HOME/Games/KartFrame/app-dev}
    BUILD=$(cd "${SRC:-$REPO/build-linux}" 2> /dev/null && pwd) || { echo "no build tree at ${SRC:-$REPO/build-linux}" >&2; exit 1; }
    [ -x "$BUILD/kartframe" ] || { echo "no kartframe in $BUILD (build with kfbuild.sh first)" >&2; exit 1; }
    EXE=$BUILD/kartframe
else
    NAME=${NAME:-KartFrame}
    APP=${KARTFRAME_APP:-$HOME/Games/KartFrame/app}
    [ -n "$SRC" ] || SRC=$(ls "$REPO"/release/kartframe-*-linux-arm64.tar.gz 2> /dev/null | sort -V | tail -1)
    [ -n "$SRC" ] || { [ -x "$REPO/build-linux/kartframe" ] && SRC=$REPO/build-linux; }
    [ -n "$SRC" ] && [ -e "$SRC" ] || { echo "usage: $0 [--dev] kartframe-*-linux-arm64.tar.gz | BUILD_DIR" >&2; exit 1; }
    EXE=$APP/kartframe
fi
case $NAME in *\'* | *\\*) echo "the shortcut name cannot hold a quote or backslash" >&2; exit 2 ;; esac
KFHOME=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
STEAM=${KARTFRAME_STEAM_DIR:-$HOME/.local/share/Steam}

if [ -z "$KARTFRAME_SKIP_STEAM" ] && [ -z "$KARTFRAME_STEAM_DIR" ]; then
    curl -s --max-time 3 127.0.0.1:8080/json > /dev/null || {
        echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)." >&2
        exit 1
    }
fi

# The program. TODO(lane B / lane F): the release tarball's layout and the kit files are not final.
# The wrapper looks for the kit fingerprint in kit/kit.json or kit.fingerprint and for the built
# game's record in kartframe.fingerprint, all in the app folder; a tarball is unpacked as it comes.
mkdir -p "$APP/tools" "$KFHOME"
if [ "$DEV" = 1 ]; then
    echo "dev install: the shortcut runs $EXE in place"
elif [ -d "$SRC" ]; then
    echo "installing the build in $SRC"
    [ -x "$SRC/kartframe" ] || { echo "no kartframe in $SRC" >&2; exit 1; }
    cp "$SRC/kartframe" "$APP/kartframe.new" && mv "$APP/kartframe.new" "$APP/kartframe"   # the game may be running
    for r in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap resources; do
        [ -e "$SRC/$r" ] && { rm -rf "${APP:?}/$r"; cp -r "$SRC/$r" "$APP/$r"; }
    done
    for r in kartframe.fingerprint kit.fingerprint kit; do
        [ -e "$SRC/$r" ] && { rm -rf "${APP:?}/$r"; cp -r "$SRC/$r" "$APP/$r"; }
    done
else
    echo "installing $SRC"
    TOPS=$(tar -tzf "$SRC" | sed 's|^\./||; s|/.*||' | sort -u | wc -l)
    STRIP=0; [ "$TOPS" = 1 ] && [ -z "$(tar -tzf "$SRC" | sed 's|^\./||' | grep -m1 '^kartframe$')" ] && STRIP=1
    tar -xzf "$SRC" -C "$APP" --strip-components=$STRIP
    [ -x "$APP/kartframe" ] || { echo "the tarball has no kartframe binary" >&2; exit 1; }
fi
cp "$HERE/kartframe.sh" "$APP/kartframe.sh"
cp "$HERE/Config.frame.toml" "$APP/Config.frame.toml"
for t in vrtiming.py session-report.py; do cp "$HERE/tools/$t" "$APP/tools/$t"; done
cp "$HERE/steam-cef.py" "$APP/tools/steam-cef.py"
chmod +x "$APP/kartframe.sh" "$APP/tools/steam-cef.py"
for f in collect-logs.sh uninstall.sh; do cp "$HERE/$f" "$APP/tools/$f" 2> /dev/null && chmod +x "$APP/tools/$f"; done

# The Frame's own controller models, converted from this Frame's SteamVR (lane C's script, if present).
if [ "$MODELS" = 1 ] && [ -f "$HERE/frame-controller-models.py" ]; then
    python3 "$HERE/frame-controller-models.py" "$APP/frame_controller_models" ||
        echo "warning: could not convert SteamVR's Frame controller models"
fi

APPID=none
LAUNCH="\"$APP/kartframe.sh\" %command%"
if [ -n "$KARTFRAME_SKIP_STEAM" ]; then
    echo "KARTFRAME_SKIP_STEAM set: no Steam shortcut"
elif [ -n "$KARTFRAME_STEAM_DIR" ]; then
    # Offline: every userdata/*/config/shortcuts.vdf of the (copied) Steam folder.
    for CFG in "$STEAM"/userdata/*/config; do
        [ -d "$CFG" ] || continue
        APPID=$(python3 "$HERE/steam-cef.py" --vdf "$CFG/shortcuts.vdf" --name "$NAME" --exe "\"$EXE\"" \
            --startdir "\"$(dirname "$EXE")\"" --opts "$LAUNCH")
        echo "shortcut '$NAME' written to $CFG/shortcuts.vdf (appid $APPID)"
        GRID=$CFG/grid
    done
    [ "$APPID" != none ] || echo "warning: no userdata/*/config in $STEAM; no shortcut written"
else
    CEF="python3 $HERE/steam-cef.py"
    EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
    if [ "$EXISTING" != "0" ] && [ -n "$EXISTING" ]; then
        APPID=$EXISTING
        echo "Steam already has a '$NAME' shortcut (appid $APPID): updating it"
        $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$EXE\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$(dirname "$EXE")\"'); return 1; })()" > /dev/null
    else
        APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$EXE', '$(dirname "$EXE")', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
        echo "added the '$NAME' shortcut (appid $APPID)"
    fi
    $CEF "(() => { SteamClient.Apps.SetShortcutLaunchOptions($APPID, '$LAUNCH'); return 1; })()" > /dev/null
    GRID=$(ls -d "$STEAM"/userdata/*/config 2> /dev/null | head -1)/grid
fi

# Optional library art; Steam finds it by the shortcut's appid.
if [ -n "$ART" ] && [ "$APPID" != none ] && [ -n "$GRID" ]; then
    mkdir -p "$GRID"
    for kind in p:p wide: hero:_hero logo:_logo icon:_icon; do
        f=$ART/${kind%%:*}.png
        [ -f "$f" ] && cp "$f" "$GRID/$APPID${kind#*:}.png" && echo "art: $GRID/$APPID${kind#*:}.png"
    done
fi

cat << MSG

Installed in $APP.
$([ "$APPID" != none ] && echo "Steam library entry \"$NAME\" (appid $APPID) runs $EXE through $APP/kartframe.sh.")
Game data: frame/setup.sh <your Mario Kart Wii disc image>  (once)
Then launch it from the library with SteamVR running.
Logs: $KFHOME/sessions/latest/ (launch.log, game.log, vrtiming.log, report.txt)
MSG
