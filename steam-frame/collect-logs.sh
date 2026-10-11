#!/bin/bash
# Collects the logs needed for a bug report into one archive, with personal data masked.
#
#   steam-frame/collect-logs.sh [appid]
#
# Without an appid it uses the newest Lepton prefix that has the WiiCompiled app in it.
# Collected: Lepton's launch log, the launch wrapper's log, Config.toml, the newest run's
# console.log, the newest build log, and a summary (SteamOS, APK and kit versions).
# Masked: IP and MAC addresses, UUIDs, hostnames, Wi-Fi network names, serial numbers,
# Steam IDs, email addresses, tokens, certificate and key blocks, and user names in /home paths.
# The masking is a best effort. Read the files before you post them.
set -euo pipefail
PKG=org.wiicompiled.quest
STEAM=$HOME/.local/share/Steam
COMPAT=$STEAM/steamapps/compatdata

if [ -n "${1:-}" ]; then
    APPID=$1
else
    APPID=$(ls -td "$COMPAT"/*/external/Android/data/$PKG 2>/dev/null | head -1 | sed "s|^$COMPAT/||; s|/.*||")
fi
[ -n "$APPID" ] && [ -d "$COMPAT/$APPID" ] || { echo "no WiiCompiled prefix found; pass the appid that install.sh printed"; exit 1; }
DATA=$COMPAT/$APPID/external/Android/data/$PKG/files/WiiCompiledOpenXRVR

STAMP=$(date +%Y%m%d-%H%M%S)
WORK=$(mktemp -d)
RAW=$WORK/raw; OUT=$WORK/wiicompiled-frame-logs-$STAMP
mkdir -p "$RAW" "$OUT"
grab() { [ -f "$1" ] && cp "$1" "$RAW/$2" || true; }

grab "$STEAM/logs/lepton-steamlaunch-$APPID.log" lepton.log
grab "$STEAM/logs/lepton-steamlaunch-$APPID.prev.log" lepton.prev.log
grab /tmp/wiicompiled-launch.log wiicompiled-launch.log
grab "$DATA/Config.toml" Config.toml
RUN=$(ls -td "$DATA"/Logs/*/ 2>/dev/null | head -1 || true)
[ -n "$RUN" ] && grab "$RUN/console.log" console.log
grab "$(ls -t "$DATA"/Logs/build_*.log 2>/dev/null | head -1 || true)" build.log
grab "$DATA/VR-diagnostics.txt" VR-diagnostics.txt

{
    echo "collected: $(date -u +%Y-%m-%dT%H:%MZ)"
    grep -E '^(VERSION_ID|BUILD_ID)=' /etc/os-release 2>/dev/null | sed 's/^/steamos /' || true
    APK=$(ls "$HOME"/Games/WiiCompiledVR-SteamFrame/app/*.apk 2>/dev/null | head -1 || true)
    if [ -n "$APK" ]; then
        echo "apk sha256: $(sha256sum "$APK" | cut -d' ' -f1)"
        echo "apk kit: $(unzip -p "$APK" assets/game_kit/kit.json 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["fingerprint"])' 2>/dev/null || echo unknown)"
    fi
    GAME=$COMPAT/$APPID/internal/$PKG/files/game/base/game.json
    [ -f "$GAME" ] && echo "built game kit: $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("kitFingerprint", ""))' "$GAME" 2>/dev/null)"
    [ -f "$RAW/lepton.log" ] && grep -m1 -o 'Lepton v[^ ]* ([^)]*)' "$RAW/lepton.log" || true
    [ -f "$RAW/console.log" ] && grep -m1 -c 'Steam Frame button layout active' "$RAW/console.log" | sed 's/^/frame layout active: /' || true
} > "$RAW/summary.txt"

python3 -I - "$RAW" "$OUT" "$(hostname 2>/dev/null || true)" <<'EOF'
import os, re, sys
src, dst, host = sys.argv[1], sys.argv[2], sys.argv[3]
I = re.I
DEFAULT_HOSTS = {'steamdeck', 'steamos', 'steamframe', 'frame', 'localhost'}
rules = [
    (re.compile(r'-----BEGIN [A-Z ]+-----.*?-----END [A-Z ]+-----', re.S), '<pem removed>'),
    (re.compile(r'\b(?:(?:ghp|gho|ghu|ghs|github_pat|xox[bp])_|sk-)[A-Za-z0-9_-]{16,}'), '<token>'),
    (re.compile(r'[\w.+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.(?!(?:service|socket|mount|slice|target|scope|so|rc|xml|bin|conf|json|log)\b)[A-Za-z]{2,}\b'), '<email>'),
    (re.compile(r'\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b', I), '<uuid>'),
    (re.compile(r'\b[0-9a-f]{2}([:-])[0-9a-f]{2}(\1[0-9a-f]{2}){4}\b', I), '<mac>'),
    (re.compile(r'\b(?:[0-9a-f]{1,4}:){4,7}[0-9a-f]{1,4}\b|\b(?:[0-9a-f]{1,4}:){1,6}:(?:[0-9a-f]{1,4}:){0,5}[0-9a-f]{1,4}\b', I), '<ipv6>'),
    (re.compile(r'\b(?!0\.0\.0\.0\b)(?!127\.)(?:\d{1,3}\.){3}\d{1,3}\b'), '<ip>'),
    (re.compile(r'\b(\w*(?:_sn|serial_?no|serial_?number|android_?id|device_?id|imei)\b["\']?\s*[:=]\s*)\S+', I), r'\1<serial>'),
    (re.compile(r'\b(b?ssid\b["\']?\s*[:=]?\s*)("[^"]*"|\'[^\']*\'|\S+)', I), r'\1<ssid>'),
    (re.compile(r'(Hostname set to\s*)<*[^\s<>]+>*', I), r'\1<host>'),
    (re.compile(r'\b7656119\d{10}\b'), '<steamid>'),
    (re.compile(r'(userdata/)\d+'), r'\1<id>'),
    (re.compile(r'/home/[^/\s]+'), '/home/<user>'),
]
for name in sorted(os.listdir(src)):
    text = open(os.path.join(src, name), errors='replace').read()
    if len(host) >= 3 and host.lower() not in DEFAULT_HOSTS:
        text = re.sub(r'\b' + re.escape(host) + r'\b', '<host>', text)
    for pattern, repl in rules:
        text = pattern.sub(repl, text)
    open(os.path.join(dst, name), 'w').write(text)
EOF

DEST=${WCF_LOG_DIR:-$HOME}/wiicompiled-frame-logs-$STAMP.tar.gz
tar -czf "$DEST" -C "$WORK" "$(basename "$OUT")"
rm -rf "$WORK"
echo "wrote $DEST"
echo "Open it and read the files before you attach it anywhere. The masking can miss things."
