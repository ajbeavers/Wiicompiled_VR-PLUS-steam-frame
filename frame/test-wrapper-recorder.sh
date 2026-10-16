#!/bin/bash
# Test for the panel recorder handling in kartframe.sh (KARTFRAME_RECORD): the game dies early, the
# game ends normally, the wrapper is killed. Touches nothing real: a scratch KARTFRAME_HOME, HOME and
# cache, a fake game, a fake framecorder (and framecorder-grab) first on PATH. No Steam, no SteamVR.
#   frame/test-wrapper-recorder.sh [SCRATCH_DIR]      (default ~/.cache/kf-test-wrapper-recorder, emptied first)
HERE=$(cd "$(dirname "$0")" && pwd)
T=${1:-$HOME/.cache/kf-test-wrapper-recorder}
rm -rf "$T"; mkdir -p "$T/bin" "$T/home" "$T/cache"
fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
check() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }

# A game that lives N seconds (exec, so a TERM ends it).
printf '#!/bin/bash\necho $$ > "$KARTFRAME_HOME/game.pid"\nexec sleep "${FAKE_GAME_SECONDS:-1}"\n' > "$T/game"
# The recorder's helper: runs until killed.
printf '#!/bin/bash\nwhile :; do sleep 1; done\n' > "$T/bin/framecorder-grab"
# The recorder: grows panels.mkv.part, takes SIGINT to finish the file (python, as a signal handler
# must be installed explicitly: a background job of the wrapper starts with SIGINT ignored).
cat > "$T/bin/framecorder" << 'PY'
#!/usr/bin/python3 -I
import os, signal, subprocess, sys, time
out = sys.argv[-1]
grab = subprocess.Popen([os.path.join(os.path.dirname(os.path.realpath(__file__)), "framecorder-grab")])
stop = []
signal.signal(signal.SIGINT, lambda *a: stop.append(1))
f = open(out + ".part", "wb")
print("fake framecorder started", flush=True)
while not stop:
    f.write(b"x" * 4096); f.flush(); time.sleep(0.1)
print("fake framecorder got SIGINT, finishing", flush=True)
time.sleep(1)
f.close(); os.rename(out + ".part", out)
grab.terminate(); grab.wait()
print("fake framecorder finished the file", flush=True)
PY
chmod +x "$T/game" "$T/bin/framecorder" "$T/bin/framecorder-grab"

survivors() { pgrep -f "$T/bin/framecorder" | tr '\n' ' '; }
# run NAME GAME_SECONDS: starts the wrapper in the background; WRAP_PID, KFH (its KARTFRAME_HOME)
run() {
    KFH=$T/$1; mkdir -p "$KFH"
    ( cd "$T" && exec env -u SteamAppId HOME="$T/home" XDG_CACHE_HOME="$T/cache" PATH="$T/bin:$PATH" \
        KARTFRAME_HOME="$KFH" KARTFRAME_EXE="$T/game" FAKE_GAME_SECONDS="$2" KARTFRAME_VRTIMING=0 \
        KARTFRAME_RECORD=raw bash "$HERE/kartframe.sh" ) > "$T/$1.out" 2>&1 &
    WRAP_PID=$!
}
waitfor() {  # waitfor PID SECONDS: true when PID is gone in time
    local n; for ((n = 0; n < $2 * 10; n++)); do kill -0 "$1" 2> /dev/null || return 0; sleep 0.1; done; return 1
}
LOG() { echo "$KFH/sessions/latest/launch.log"; }
show() { echo "     --- launch.log (recorder lines)"; grep -i 'record\|panels\|game exited' "$KFH/sessions/latest/launch.log" | sed 's/^/     /'; }

echo "== 1: game exits after 1 s, before the recorder's 8 s =="
run early 1
check "wrapper finished" waitfor "$WRAP_PID" 30
sleep 9   # past the point where the old wrapper's recorder started anyway
check "no recorder process survives (survivors: $(survivors))" test -z "$(survivors)"
check "log says it was never started" grep -q 'recorder was never started' "$(LOG)"
check "no panels.mkv.part" test ! -e "$KFH/sessions/latest/panels.mkv.part"
show

echo "== 2: game runs 12 s, recorder runs, game exits normally =="
run normal 12
check "wrapper finished" waitfor "$WRAP_PID" 60
check "no recorder process survives (survivors: $(survivors))" test -z "$(survivors)"
check "log says recorder stopped" grep -q 'recorder stopped' "$(LOG)"
check "recorder got SIGINT and finished the file" grep -q 'finished the file' "$KFH/sessions/latest/framecorder.log"
check "panels.mkv exists, no .part left" test -s "$KFH/sessions/latest/panels.mkv" -a ! -e "$KFH/sessions/latest/panels.mkv.part"
show

echo "== 3: wrapper killed with TERM while the game runs (recorder up) =="
run killed 60
sleep 10
check "recorder is running before the kill" test -n "$(survivors)"
kill -TERM "$WRAP_PID"
check "wrapper finished" waitfor "$WRAP_PID" 30
check "no recorder process survives (survivors: $(survivors))" test -z "$(survivors)"
check "game is gone" test ! -d "/proc/$(cat "$KFH/game.pid")"
check "log says recorder stopped" grep -q 'recorder stopped' "$(LOG)"
show

echo "== 4: wrapper killed with TERM before the recorder started =="
run killedearly 60
sleep 3
kill -TERM "$WRAP_PID"
check "wrapper finished" waitfor "$WRAP_PID" 30
sleep 7
check "no recorder process survives (survivors: $(survivors))" test -z "$(survivors)"
check "log says it was never started" grep -q 'recorder was never started' "$(LOG)"
show

echo
[ "$fails" = 0 ] && echo "all passed" || echo "$fails FAILED"
exit $((fails > 0))
