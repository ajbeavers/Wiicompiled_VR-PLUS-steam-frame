#!/bin/bash
# KartFrame launch wrapper, run by the Steam shortcut ("<this script> %command%"). There is no screen
# for messages in VR: everything goes to the session log, and a refusal is a non-zero exit.
#
# Every launch: re-applies SteamVR's per-app settings (it drops them now and then), starts the
# compositor timing logger, runs the game, then writes a session folder
# $KARTFRAME_HOME/sessions/<date-time>/ with launch.log, stdout.log, game.log, vrtiming.log, report.txt.
# $KARTFRAME_HOME/sessions/latest points at the newest one.
#
# Environment (all optional; session.env below is the way to set them from the Steam library):
#   KARTFRAME_HOME          data folder, default ~/.local/share/KartFrame (game/, build/, sessions/)
#   KARTFRAME_REFRESH_HZ    refresh rate asked of SteamVR, 90 or 120 (default 120)
#   KARTFRAME_FDM_LAYER=0   sets DISABLE_VULKAN_FDM_INJECTION_LAYER=1 (A/B: Valve's FDM layer off)
#   KARTFRAME_SYSMEM=0      leaves Turnip in tiled mode; default 1 sets TU_DEBUG=sysmem (measured: the
#                           race holds 120 Hz at 1728 only with it)
#   KARTFRAME_PACING        passed through to the game (auto | inline | pipelined)
#   KARTFRAME_VRTIMING=0    no compositor timing log
#   KARTFRAME_SESSION_ENV_ONCE=1   (inside session.env) delete session.env after this launch
#   KARTFRAME_KEEP_SESSIONS number of session folders kept (default 30)
# Test hooks: KARTFRAME_VRCMD, KARTFRAME_STEAMVR_DIR, KARTFRAME_VRSETTINGS, KARTFRAME_VRTIMING_PY,
# KARTFRAME_REPORT_PY, KARTFRAME_EXE.
HERE=$(dirname "$(readlink -f "$0")")
KFHOME=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}
STAMP=$(date +%Y%m%d-%H%M%S)
SESSION=$KFHOME/sessions/$STAMP
mkdir -p "$SESSION" || exit 1
ln -sfn "$STAMP" "$KFHOME/sessions/latest"
exec > "$SESSION/launch.log" 2>&1
log() { echo "$(date +%T) $*"; }
log "KartFrame wrapper, session $STAMP, steam appid '${SteamAppId:-unset}'"

# One launch's test environment: VAR=value lines (KARTFRAME_*, DISABLE_VULKAN_FDM_INJECTION_LAYER, ...).
if [ -f "$KFHOME/session.env" ]; then
    log "session.env:"; cat "$KFHOME/session.env"; cp "$KFHOME/session.env" "$SESSION/session.env"
    set -a; . "$KFHOME/session.env"; set +a
    [ "$KARTFRAME_SESSION_ENV_ONCE" = 1 ] && { rm -f "$KFHOME/session.env"; log "session.env deleted (KARTFRAME_SESSION_ENV_ONCE)"; }
fi
[ "$KARTFRAME_FDM_LAYER" = 0 ] && { export DISABLE_VULKAN_FDM_INJECTION_LAYER=1; log "A/B: Valve FDM layer disabled"; }
[ "${KARTFRAME_SYSMEM:-1}" = 1 ] && { export TU_DEBUG=sysmem; log "TU_DEBUG=sysmem (KARTFRAME_SYSMEM=0 turns it off)"; }
[ -n "$KARTFRAME_PACING" ] && { export KARTFRAME_PACING; log "KARTFRAME_PACING=$KARTFRAME_PACING"; }

# The game: Steam's %command% (the binary the shortcut names), else the installed one.
EXE=${KARTFRAME_EXE:-${1:-$KFHOME/bin/kartframe}}
[ $# -gt 0 ] && shift
[ -x "$EXE" ] || { log "REFUSING TO START: no game binary at $EXE (run frame/setup.sh)"; exit 1; }

# Kit check: the installed binary must have been built against the kit that is installed now.
# Installed kit: <app>/kit/kit.json "fingerprint"; the built game's record: $KFHOME/bin/kartframe.fingerprint.
# kit_fingerprint FILE prints the "fingerprint" of a kit.json, or the first line of a plain file.
kit_fingerprint() {
    python3 -I -c '
import json, sys
t = open(sys.argv[1]).read()
try:
    print(json.loads(t)["fingerprint"])
except Exception:
    print(t.split("\n")[0].strip())' "$1" 2> /dev/null
}
kit_check() {  # prints the reason and returns 1 when the binary and the kit disagree
    local kit= built=
    [ -f "$HERE/kit/kit.json" ] && kit=$HERE/kit/kit.json
    [ -f "$KFHOME/bin/kartframe.fingerprint" ] && built=$KFHOME/bin/kartframe.fingerprint
    [ -n "$kit" ] || { log "kit check: no kit fingerprint installed (development build), skipped"; return 0; }
    [ -n "$built" ] || { echo "no record of the kit this binary was built against; run frame/setup.sh again"; return 1; }
    local k b
    k=$(kit_fingerprint "$kit"); b=$(kit_fingerprint "$built")
    [ -n "$k" ] && [ "$k" = "$b" ] && { log "kit check: ok (${k:0:12})"; return 0; }
    echo "the game was built for another kit (${b:0:12} vs installed ${k:0:12}); run frame/setup.sh again"; return 1
}
if ! why=$(kit_check); then log "REFUSING TO START: $why"; exit 78; fi
[ -n "$why" ] && echo "$why"

# Portable mode: portable.txt in KFHOME (a parent of bin/) makes the game's root $KFHOME/UserData.
STATE=$KFHOME/UserData
mkdir -p "$STATE"; [ -f "$KFHOME/portable.txt" ] || : > "$KFHOME/portable.txt"
if [ ! -f "$STATE/Config.toml" ] && [ -f "$HERE/Config.frame.toml" ]; then
    sed "s|@DVD_ROOT@|$KFHOME/game/DATA|" "$HERE/Config.frame.toml" > "$STATE/Config.toml" &&
        log "seeded $STATE/Config.toml"
fi

# SteamVR's per-app settings, every launch. SteamAppId is set by Steam for a library launch.
HZ=${KARTFRAME_REFRESH_HZ:-120}   # TODO(lane A): follow Config's refresh choice if one is added
case "$HZ" in 90 | 120) ;; *) log "KARTFRAME_REFRESH_HZ=$HZ is not 90 or 120, using 120"; HZ=120 ;; esac
VRCMD=${KARTFRAME_VRCMD:-$(command -v vrcmd || echo /opt/steamvr/bin/linuxarm64/vrcmd)}
VRSETTINGS=${KARTFRAME_VRSETTINGS:-$HOME/.config/openvr/config/steamvr.vrsettings}
STEAMVR_DIR=${KARTFRAME_STEAMVR_DIR:-/opt/steamvr/bin/linuxarm64}
SETTINGS="preferredRefreshRate=$HZ framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=1728"
if [ -z "$SteamAppId" ]; then
    log "no SteamAppId: SteamVR per-app settings NOT applied (not a Steam library launch)"
elif [ ! -x "$VRCMD" ]; then
    log "no vrcmd at $VRCMD: SteamVR per-app settings NOT applied"
else
    for kv in $SETTINGS; do
        "$VRCMD" --set-settings-int "steam.app.$SteamAppId.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
            log "WARNING: vrcmd could not set ${kv%=*} (is SteamVR running?)"
    done
    sleep 0.3   # SteamVR writes its settings file shortly after a change
    # Read back from SteamVR's settings file and log every difference.
    python3 -I - "$VRSETTINGS" "steam.app.$SteamAppId" $SETTINGS << 'PY' || log "settings read-back failed"
import json, sys, time
path, section, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
try:
    got = json.load(open(path)).get(section, {})
except (OSError, ValueError) as e:
    print(f"{time.strftime('%T')} read-back: cannot read {path} ({e.__class__.__name__}); settings unverified")
    sys.exit(0)
bad = 0
for p in pairs:
    k, v = p.split("=")
    if str(got.get(k)) != v:
        bad += 1
        print(f"{time.strftime('%T')} MISMATCH {section}.{k}: wanted {v}, SteamVR has {got.get(k)}")
print(f"{time.strftime('%T')} read-back: {len(pairs) - bad} of {len(pairs)} per-app settings as asked")
PY
fi

# Compositor timing for the whole session; it cannot stop the game from starting.
VRT_PID=
VRT=${KARTFRAME_VRTIMING_PY:-$HERE/tools/vrtiming.py}
if [ "${KARTFRAME_VRTIMING:-1}" != 0 ] && [ -f "$VRT" ] && [ -f "$STEAMVR_DIR/libopenvr_api.so" ]; then
    env -u LD_LIBRARY_PATH python3 -I "$VRT" 86400 "$SESSION/vrtiming.log" > /dev/null 2>&1 &
    VRT_PID=$!
else
    log "no vrtiming.log (disabled, or vrtiming.py / libopenvr_api.so missing)"
fi

# Not exec: the wrapper stays to stop the logger and write the report. A signal for the wrapper
# (Steam stopping the app) goes on to the game.
log "starting $EXE"
touch "$SESSION/.start"
"$EXE" "$@" > "$SESSION/stdout.log" 2>&1 &
GAME_PID=$!
trap 'kill -TERM "$GAME_PID" 2> /dev/null' TERM INT HUP
rc=0
while kill -0 "$GAME_PID" 2> /dev/null; do
    wait "$GAME_PID"; rc=$?
done
trap - TERM INT HUP
log "game exited with status $rc"
[ -n "$VRT_PID" ] && { kill "$VRT_PID" 2> /dev/null; wait "$VRT_PID" 2> /dev/null; }

# The game's own log: the newest console.log of this session, else what it printed.
GL=$(find "$STATE/Logs" -name console.log -newer "$SESSION/.start" 2> /dev/null | xargs -r ls -t 2> /dev/null | head -1)
if [ -n "$GL" ]; then cp "$GL" "$SESSION/game.log"; else log "no console.log from this run; the report reads stdout.log"; cp "$SESSION/stdout.log" "$SESSION/game.log"; fi
rm -f "$SESSION/.start"
REPORT=${KARTFRAME_REPORT_PY:-$HERE/tools/session-report.py}
if [ -f "$REPORT" ]; then
    set -- "$SESSION/game.log"; [ -s "$SESSION/vrtiming.log" ] && set -- "$@" "$SESSION/vrtiming.log"
    python3 -I "$REPORT" "$@" > "$SESSION/report.txt" 2>&1 || log "session-report exited non-zero"
else
    log "no session-report.py at $REPORT"
fi
# Keep the newest sessions only.
KEEP=${KARTFRAME_KEEP_SESSIONS:-30}
ls -1d "$KFHOME"/sessions/2* 2> /dev/null | head -n -"$KEEP" | xargs -r rm -rf
exit $rc
