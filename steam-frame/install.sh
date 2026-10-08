#!/bin/bash
# Installs the Steam Frame build of WiiCompiled VR: copies the APK and launch wrapper to
# ~/Games/WiiCompiledVR-SteamFrame, adds (or updates) a "Mario Kart Wii VR" entry in the Steam
# library set to the Lepton compatibility tool, and gives SteamVR the Frame controller binding
# plus the per-app settings that keep the game at 90 Hz. Steam must be running.
#
#   steam-frame/install.sh [path/to/WiiCompiled-VR-SteamFrame-*.apk]
#
# Without an argument the newest APK in steam-frame/release/ is used.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
APK=${1:-$(ls -t "$HERE"/release/WiiCompiled-VR-SteamFrame*.apk 2>/dev/null | head -1)}
[ -f "$APK" ] || { echo "usage: $0 path/to/WiiCompiled-VR-SteamFrame-*.apk (download one from the Releases page, or run steam-frame/build.sh)"; exit 1; }
DEST=$HOME/Games/WiiCompiledVR-SteamFrame
NAME=${WCF_SHORTCUT_NAME:-Mario Kart Wii VR}
LEPTON=$HOME/.local/share/Steam/steamapps/common/Lepton
VRCMD=/opt/steamvr/bin/linuxarm64/vrcmd
VRSETTINGS=$HOME/.config/openvr/config/steamvr.vrsettings
[ -x "$LEPTON/lepton" ] || echo "warning: Lepton is not installed (Steam > Library > Tools > Lepton); the game cannot launch without it"
curl -s --max-time 3 127.0.0.1:8080/json >/dev/null || { echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)."; exit 1; }

mkdir -p "$DEST/app" "$DEST/tools"
cp "$APK" "$DEST/app/WiiCompiled-VR-SteamFrame.apk"
touch "$DEST/app/lepton-show-flatscreen"   # lets Lepton show the 2D launcher window, should SteamVR ever pick it up
cp "$HERE/wiicompiled-launch.sh" "$DEST/wiicompiled-launch.sh"; chmod +x "$DEST/wiicompiled-launch.sh"
cp "$HERE/Config.frame.toml" "$DEST/Config.frame.toml"
cp "$HERE/steam-cef.py" "$DEST/tools/steam-cef.py"; chmod +x "$DEST/tools/steam-cef.py"
CEF="python3 $DEST/tools/steam-cef.py"

# The Steam shortcut, through Steam's own client API (no editing of shortcuts.vdf behind its back).
EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2>/dev/null || echo 0)
if [ "$EXISTING" != "0" ] && [ -n "$EXISTING" ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' shortcut (appid $APPID): pointing it at the new APK"
    $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$DEST/app/WiiCompiled-VR-SteamFrame.apk\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$DEST/app\"'); return 1; })()" >/dev/null
else
    APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/app/WiiCompiled-VR-SteamFrame.apk', '$DEST/app', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
    echo "added the '$NAME' shortcut (appid $APPID)"
fi
$CEF "(() => { SteamClient.Apps.SetShortcutLaunchOptions($APPID, '$DEST/wiicompiled-launch.sh %command%'); SteamClient.Apps.SpecifyCompatTool($APPID, 'lepton'); return 1; })()" >/dev/null

# SteamVR: the controller binding for this app, and the frame pacing that makes 90 Hz hold.
sed "s/@APPKEY@/steam.app.$APPID/" "$HERE/bindings_frame_controller.json" > "$DEST/bindings_frame_controller.json"
# SteamVR names the Frame controller's binding keys after its input profile revision; reuse whatever
# revision this SteamVR already wrote for another app, else the one on my Frame.
SUFFIX=$(python3 - "$VRSETTINGS" <<'EOF'
import json, re, sys
try:
    text = open(sys.argv[1]).read()
except OSError:
    text = ''
m = re.search(r'"frame_controller_(\d+)_CurrentURL_openxr"', text)
print(m.group(1) if m else '250820')
EOF
)
set_vr() {  # key type value: through vrcmd while SteamVR runs, else straight into its settings file
    if pgrep -x vrserver >/dev/null 2>&1 && [ -x "$VRCMD" ] && "$VRCMD" "--set-settings-$2" "$1" "$3" >/dev/null 2>&1; then
        return
    fi
    python3 - "$VRSETTINGS" "$1" "$2" "$3" <<'EOF'
import json, os, sys
path, key, kind, value = sys.argv[1:]
section, name = key.rsplit('.', 1)
try:
    settings = json.load(open(path))
except (OSError, ValueError):
    settings = {}
settings.setdefault(section, {})[name] = int(value) if kind == 'int' else value
os.makedirs(os.path.dirname(path), exist_ok=True)
json.dump(settings, open(path, 'w'), indent=3)
EOF
}
set_vr "steam.app.$APPID.preferredRefreshRate" int 90
set_vr "steam.app.$APPID.framesToThrottle" int 0
set_vr "steam.app.$APPID.additionalFramesToPredict" int 0
set_vr "steam.app.$APPID.frame_controller_${SUFFIX}_CurrentURL_openxr" string "file://$DEST/bindings_frame_controller.json"

cat <<MSG

Installed. The Steam library entry "$NAME" (appid $APPID) runs $DEST/app/WiiCompiled-VR-SteamFrame.apk through Lepton,
with the Frame controller binding and 90 Hz settings in SteamVR.

Next: launch it once from the library with the headset on (Lepton installs the APK, which takes a minute), then
import your Mario Kart Wii disc and let the headset build the game; the README's "Setting it up" section has the
two commands. Logs: ~/.local/share/Steam/logs/lepton-steamlaunch-$APPID.log and /tmp/wiicompiled-launch.log
MSG
