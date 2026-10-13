#!/bin/bash
# Golden pictures: a fixed set of headless captures of the game, taken frame-locked so that
# the same build gives bit-identical PNGs run after run, to compare builds and renderer backends.
#
#   tools/golden.sh capture <build-dir> <out-dir> [--verify] [--only boot,boot,...] [--xr-only]
#   tools/golden.sh compare <dirA> <dirB> [--tolerant] [--all]
#   tools/golden.sh list
#
# capture   <build-dir> holds libgame.so (e.g. build-linux).  Boots the game headless once per
#           scene group (about 40 s each, 9 groups), plays the title and file select with the
#           scripted controller, jumps into a galaxy (PETARI_STAGE) and saves the flat picture
#           (<name>.png) at fixed game frames; the shots marked x also save the headset
#           presentation of that frame (<name>_xr.png, both eyes with the HUD as UI layers, and
#           <name>_xr_mv.png, the motion vectors).  --verify then captures a second time and
#           demands bit-identical files (a few minutes more).  MANIFEST.txt in <out-dir> records
#           the commit, the backend and a fingerprint of the saves and shader cache.
# compare   Bit-exact by default (a refactor must not change a pixel).  --tolerant is the rule
#           for another backend (Vulkan against GLES): mean absolute difference under 1/255 and
#           under 0.5% of the pixels more than 8/255 off (tools/imgdiff.py).  Exit status 0 when
#           every picture passes.  The title and file select shots are skipped (see UNSTABLE) unless --all.
#
# Why it is deterministic: tools/golden_runner.c, not petari_headless, drives the game.  It
# feeds the scripted controller by the game's own retrace count (60 retraces a second, as the
# game sees them), and at a shot pauses the retraces, lets the frame in flight finish, renders
# the latest frame and resumes.  Wall-clock timing (petari_headless's snapshots) lands a frame
# or two apart from run to run and animated pixels then differ.
#
# Backend: PETARI_GFX=gles|vk is passed through to the game unchanged (and written to the
# manifest); until the Vulkan backend lands it is ignored.  The runner calls the same entry
# points as petari_headless (port_headless_render, port_headless_xrsim), so a backend that keeps
# those works with no change here.  Other environment for the game goes in GOLDEN_ENV, e.g.
# GOLDEN_ENV="PETARI_NOCOPY=1 FOO=bar".
#
# Inputs and where they come from (nothing in ~/.local/share/GalaxyFrame is written):
#   GOLDEN_HOME   scratch folder, default ~/.cache/gq-golden-home (game files linked, saves, shader
#                 cache and VR settings copied per run)
#   GOLDEN_GAME   the converted game files, default ~/.local/share/GalaxyFrame/game
#   GOLDEN_NAND   the saves, default ~/.local/share/GalaxyFrame/nand (the file select shows them)
#   GOLDEN_SHADERS  shader cache to start from, default ~/.local/share/GalaxyFrame/shaders.bin
#
# Captures take the machine-wide lock $GQROOT/.build.lock (the one gqbuild.sh uses; GQROOT defaults to ~/Games/GalaxyQuest, the development folder), so
# they never overlap a build or another capture; GOLDEN_NO_LOCK=1 when the caller already holds it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
GQROOT=${GQROOT:-$HOME/Games/GalaxyQuest}
DATA=${GALAXYFRAME_HOME:-${GALAXYQUEST_HOME:-$HOME/.local/share/GalaxyFrame}}  # GALAXYQUEST_HOME: the old name
GOLDEN_HOME=${GOLDEN_HOME:-$HOME/.cache/gq-golden-home}
GOLDEN_GAME=${GOLDEN_GAME:-$DATA/game}
GOLDEN_NAND=${GOLDEN_NAND:-$DATA/nand}
GOLDEN_SHADERS=${GOLDEN_SHADERS:-$DATA/shaders.bin}

# Scene groups: name|galaxy|extra controller script|shots (name:retrace[:x], 60 retraces a second).
# The controller always plays the title (about 28 s) and file select (about 42 s) and then presses
# A every 3 s to pass the galaxy's introduction.
SCENES=(
  "egg|EggStarGalaxy|71.6-71.8:PLUS|title:1500,fileselect:2100,wipe:2400,egg_intro:3300,egg_play:3900:x,egg_pause:4400"
  "beach|HeavenlyBeachGalaxy||beach_intro:3300:x,beach_play:3900,beach_move:4500"
  "ocean|OceanRingGalaxy||ocean_intro:3300,ocean_play:3900,ocean_move:4500"
  "honey|HoneyBeeKingdomGalaxy||honey_intro:3300,honey_play:3900,honey_move:4500"
  "phantom|PhantomGalaxy||phantom_intro:3300,phantom_play:3900:x,phantom_move:4500"
  "hell|HellProminenceGalaxy||hell_intro:3300,hell_play:3900,hell_move:4500"
  "battle|BattleShipGalaxy||battle_intro:3300,battle_play:3900,battle_move:4500"
  "sand|SandClockGalaxy||sand_intro:3300,sand_play:3900,sand_move:4500"
  "surf|SurfingLv1Galaxy||surf_intro:3300,surf_play:3900:x,surf_move:4500"
)
# Shots whose picture depends on wall-clock time inside the game (the title and file select
# backgrounds move with it; the Honeyhive bee shots differ in Mario's pose now and then): captured, but skipped by `compare` unless --all.
UNSTABLE=title,fileselect,honey_intro,honey_move
# Mario walks (stick) between the play and move shots.
MOVE="72-78:SX=0.7|SY=0.7"

usage() { sed -n '2,/^set -e/p' "$0" | sed 's/^# \{0,1\}//; /^set -e/d' >&2; exit 2; }

fingerprint() { # a stable hash of a folder tree or file
  if [ -d "$1" ]; then (cd "$1" && find . -type f | sort | xargs cat | sha256sum | cut -c1-16)
  elif [ -f "$1" ]; then sha256sum "$1" | cut -c1-16
  else echo missing; fi
}

capture_once() { # build-dir out-dir only xronly
  local BUILD=$1 OUT=$2 ONLY=$3 XRONLY=$4
  rm -rf "$OUT"; mkdir -p "$OUT" "$GOLDEN_HOME"
  [ -f "$BUILD/libgame.so" ] || { echo "no libgame.so in $BUILD" >&2; return 1; }
  [ -d "$GOLDEN_GAME/files" ] || { echo "no game files at $GOLDEN_GAME" >&2; return 1; }
  pgrep -x "galaxyframe|galaxyquest" > /dev/null && { echo "the game is running; quit it first" >&2; return 1; }
  local RUNNER=$GOLDEN_HOME/golden_runner
  if [ ! -x "$RUNNER" ] || [ "$HERE/golden_runner.c" -nt "$RUNNER" ]; then
    clang -O1 -Wall -I"$REPO/platform/include" "$HERE/golden_runner.c" -o "$RUNNER" -ldl -lm
  fi
  # A pristine copy of everything the game may write, per capture.
  rm -rf "$GOLDEN_HOME/nand.pristine" "$GOLDEN_HOME/shaders.bin" "$GOLDEN_HOME/petari_vr.ini"
  cp -r "$GOLDEN_NAND" "$GOLDEN_HOME/nand.pristine"
  [ -f "$GOLDEN_SHADERS" ] && cp "$GOLDEN_SHADERS" "$GOLDEN_HOME/shaders.bin"
  # Fixed VR settings, not the owner's: the headset presentation depends on them.
  cat > "$GOLDEN_HOME/petari_vr.ini" <<'INI'
passthrough = 0
stereo_screen = 0
diorama_distance = 1.5
diorama_height = 0.95
diorama_scale = 1
turn_smoothing = 0.35
sharpening = 1
resolution = 0.8
min_resolution = 0.8
giant_screen = 0
screen_distance = 4.5
follow_smoothing = 0.2
turn_with_camera = 0
cutaway = 1
stereo_resolution = 1
vignette = 0.8
INI
  local PRESSES MENU
  PRESSES=$(for t in $(seq 44 3 65); do printf ",%s-%s.2:A" $t $t; done)
  MENU="28-28.4:A|B,30-36:PX=-0.25|PY=0.27,33-33.2:A,36-42:PX=0.52|PY=0.78,38-38.2:A"
  {
    echo "commit: $(git -C "$BUILD/.." rev-parse --short HEAD 2> /dev/null || echo unknown)"
    echo "build: $BUILD"
    echo "libgame.so: $(fingerprint "$BUILD/libgame.so")"
    echo "PETARI_GFX: ${PETARI_GFX:-default}"
    echo "GOLDEN_ENV: ${GOLDEN_ENV:-}"
    echo "saves: $(fingerprint "$GOLDEN_NAND")"
    echo "shader cache (start): $(fingerprint "$GOLDEN_SHADERS")"
    echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
  } > "$OUT/MANIFEST.txt"
  local entry n=0
  for entry in "${SCENES[@]}"; do
    IFS='|' read -r boot galaxy extra shots <<< "$entry"
    [ -n "$ONLY" ] && [[ ",$ONLY," != *",$boot,"* ]] && continue
    local script="$MENU$PRESSES"
    [ -n "$extra" ] && script="$script,$extra"
    # The galaxy groups walk between their play and move shots.
    [[ "$shots" == *_move:* ]] && script="$script,$MOVE"
    if [ -n "$XRONLY" ]; then
      shots=$(echo "$shots" | tr ',' '\n' | grep ':x$' | paste -sd, || true)
      [ -z "$shots" ] && continue
    fi
    local last
    last=$(echo "$shots" | tr ',' '\n' | sed 's/^[^:]*://; s/:.*//' | sort -n | tail -1)
    rm -rf "$GOLDEN_HOME/run"; mkdir -p "$GOLDEN_HOME/run"
    cp -r "$GOLDEN_HOME/nand.pristine" "$GOLDEN_HOME/run/nand"
    ln -sfn "$GOLDEN_GAME" "$GOLDEN_HOME/game"
    printf '%-8s %-24s ' "$boot" "$galaxy"
    local envs=(EGL_PLATFORM=surfaceless SDL_AUDIODRIVER=dummy GALAXYFRAME_LIB="$BUILD"
                PETARI_SHADER_CACHE="$GOLDEN_HOME/shaders.bin" PETARI_VRINI="$GOLDEN_HOME/petari_vr.ini"
                PETARI_STAGE="$galaxy" PETARI_INPUT="$script" PETARI_LANGUAGE=english
                PETARI_XRSIM_LAYERS=1 PETARI_XRSIM_MV=1 PETARI_XRSIM_STEPS=40)
    [ -n "$PETARI_GFX" ] && envs+=(PETARI_GFX="$PETARI_GFX")
    # shellcheck disable=SC2206
    [ -n "$GOLDEN_ENV" ] && envs+=($GOLDEN_ENV)
    local rc=0
    env "${envs[@]}" timeout $((last / 30 + 240)) "$RUNNER" "$GOLDEN_GAME" "$GOLDEN_HOME/run/nand" "$OUT" $((last + 60)) "$shots" \
      > "$OUT/$boot.log" 2>&1 || rc=$?
    local got
    got=$(grep -c 'golden: shot .* is game frame' "$OUT/$boot.log" || true)
    echo "$got of $(echo "$shots" | tr ',' '\n' | wc -l) shots (exit $rc)"
    # The game frame each shot rendered, to tell a shifted capture from a changed picture.
    grep -a 'golden: shot .* is game frame' "$OUT/$boot.log" | sed 's/^golden: shot //; s/ is game frame / /' >> "$OUT/frames.txt" || true
    n=$((n + got))
    [ "$rc" != 0 ] && echo "  failed: see $OUT/$boot.log" >&2
  done
  echo "shots: $n" >> "$OUT/MANIFEST.txt"
  echo "$n shots in $OUT"
}

cmd=${1:-}
case "$cmd" in
  capture)
    [ $# -ge 3 ] || usage
    if [ -z "$GOLDEN_NO_LOCK" ]; then
      exec 9> "$GQROOT/.build.lock"
      echo "golden: waiting for the build lock"; flock 9
    fi
    BUILD=$(cd "$2" && pwd); OUT=$3; shift 3
    VERIFY=; ONLY=; XRONLY=
    while [ $# -gt 0 ]; do
      case "$1" in
        --verify) VERIFY=1 ;;
        --only) ONLY=$2; shift ;;
        --xr-only) XRONLY=1 ;;
        *) usage ;;
      esac
      shift
    done
    mkdir -p "$(dirname "$OUT")"; OUT=$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")
    capture_once "$BUILD" "$OUT" "$ONLY" "$XRONLY"
    if [ -n "$VERIFY" ]; then
      echo "verifying: capturing again"
      capture_once "$BUILD" "$OUT.verify" "$ONLY" "$XRONLY"
      if python3 -I "$HERE/imgdiff.py" "$OUT" "$OUT.verify" --ignore "$UNSTABLE" | tail -3; then
        echo "deterministic: both captures are bit-identical"; rm -rf "$OUT.verify"
      else
        echo "NOT deterministic: see $OUT.verify" >&2; exit 1
      fi
    fi
    ;;
  compare)
    [ $# -ge 3 ] || usage
    A=$2; B=$3; shift 3
    MODE=; IGN="--ignore $UNSTABLE"
    for a in "$@"; do
      [ "$a" = "--tolerant" ] && MODE=--tolerant
      [ "$a" = "--all" ] && IGN=
    done
    for f in commit PETARI_GFX saves; do
      va=$(grep "^$f:" "$A/MANIFEST.txt" 2> /dev/null || true); vb=$(grep "^$f:" "$B/MANIFEST.txt" 2> /dev/null || true)
      [ "$va" != "$vb" ] && echo "note: $f differs: A '${va#*: }' B '${vb#*: }'"
    done
    [ "$(grep '^saves:' "$A/MANIFEST.txt" 2> /dev/null)" != "$(grep '^saves:' "$B/MANIFEST.txt" 2> /dev/null)" ] && echo "warning: the saves differ, so the file select and some scenes will"
    python3 -I "$HERE/imgdiff.py" "$A" "$B" $MODE $IGN
    ;;
  list)
    for entry in "${SCENES[@]}"; do IFS='|' read -r boot galaxy extra shots <<< "$entry"; echo "$boot ($galaxy): $shots"; done
    ;;
  *) usage ;;
esac
