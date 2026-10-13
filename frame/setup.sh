#!/usr/bin/env bash
# KartFrame first-run setup. Everything here runs on the user's own Frame from the user's own disc;
# nothing derived from the disc is ever stored in the repository, the kit or a release.
#
#   setup.sh [--force] <iso>               everything below, in order; idempotent and resumable
#   setup.sh extract   <iso> <dest>        disc -> <dest>/{DATA,main.dol,StaticR.rel,.complete}
#   setup.sh translate <assets> <outdir>   translator pipeline -> <outdir>/{generated,build/base}
#   setup.sh build                         compile the translation against the kit, link kartframe, install
#   setup.sh status                        what is done
#
# Steps of `setup.sh <iso>` (about 20 minutes unattended, mostly the compile):
#   1. extract    nodtool verifies RMCP01 and the main.dol / StaticR.rel hashes; -> $KARTFRAME_HOME/game
#   2. translate  the translator (recompiler) -> $KARTFRAME_HOME/build/{generated,build}
#   3. build      the translated shards are compiled with the kit's flags and the bundled toolchain,
#                 linked against the kit -> $KARTFRAME_HOME/bin/kartframe (+ resources, kartframe.fingerprint)
#   4. seed       portable.txt and UserData/Config.toml (only when missing)
#
# Environment:
#   KARTFRAME_HOME        data folder, everything user-specific lives here (default ~/.local/share/KartFrame);
#                         portable.txt there keeps the game's own data out of ~/.local/share/WiiCompiled
#   KARTFRAME_KIT         the game kit (default <this dir>/kit)
#   KARTFRAME_TOOLCHAIN   portable toolchain root with bin/clang (default <this dir>/toolchain, else the
#                         cache, else toolchain.sh makes it)
#   KARTFRAME_TRANSLATOR  published Translator.Cli (default <this dir>/translator/Translator.Cli, else cache)
#   KARTFRAME_CACHE       downloads (default ~/.cache/kartframe)
#   KARTFRAME_JOBS        parallel shard compiles (default 3, fewer when RAM is short)
#   KARTFRAME_THREADS     translator threads (default nproc)
set -euo pipefail

NODTOOL_VERSION=v2.0.0-alpha.10   # the version upstream's NodToolProvider.cs pins
NODTOOL_SHA256=b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b
NODTOOL_URL=https://github.com/encounter/nod/releases/download/$NODTOOL_VERSION/nodtool-linux-aarch64

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
cache=${KARTFRAME_CACHE:-$HOME/.cache/kartframe}
home=${KARTFRAME_HOME:-$HOME/.local/share/KartFrame}

# The kit carries the project files and runtime sources the translator reads (workspace/); in a
# source tree they are the repo's own.
kit_dir() {
    local k=${KARTFRAME_KIT:-$here/kit}
    [[ -f $k/kit.json ]] && { (cd "$k" && pwd); return; }
    return 1
}
workspace_src() {
    local k
    if k=$(kit_dir) && [[ -d $k/workspace/projects ]]; then echo "$k/workspace"; else echo "$repo"; fi
}
recomp=$(workspace_src)/projects/mkwii/recomp.yml

fail() { echo "setup.sh: error: $*" >&2; exit 1; }
step() { printf '[setup] %s\n' "$*"; }
sha256_of() { sha256sum "$1" | awk '{print $1}'; }
now() { date +%s.%N; }
elapsed() { awk -v a="$1" -v b="$(now)" 'BEGIN{printf "%.1fs", b-a}'; }

# sha256 pin for inputs.<dol|rel> in recomp.yml
pin() {
    awk -v key="$1:" '
        $1 == key { found = 1; next }
        found && $1 == "sha256:" { print $2; exit }' "$recomp"
}

ensure_nodtool() {
    local dir=$cache/nodtool/$NODTOOL_VERSION bin
    bin=$dir/nodtool
    if [[ -x $bin && $(sha256_of "$bin") == "$NODTOOL_SHA256" ]]; then echo "$bin"; return; fi
    mkdir -p "$dir"
    step "fetching nodtool $NODTOOL_VERSION" >&2
    curl -fsSL -o "$bin.tmp" "$NODTOOL_URL"
    [[ $(sha256_of "$bin.tmp") == "$NODTOOL_SHA256" ]] || { rm -f "$bin.tmp"; fail "nodtool sha256 mismatch"; }
    chmod +x "$bin.tmp"; mv -f "$bin.tmp" "$bin"
    echo "$bin"
}

cmd_extract() {
    [[ $# -eq 2 ]] || fail "usage: setup.sh extract <iso> <dest>"
    local iso=$1 dest=$2 nod t0 id staging dol_pin rel_pin
    [[ -r $iso ]] || fail "cannot read $iso"
    dol_pin=$(pin dol); rel_pin=$(pin rel)
    [[ -n $dol_pin && -n $rel_pin ]] || fail "could not read the sha256 pins from $recomp"
    nod=$(ensure_nodtool)

    # Header first: a wrong disc fails in a second, before anything is written.
    id=$("$nod" info "$iso" 2>/dev/null | sed -n 's/^Game ID: \(\S*\).*/\1/p' | head -n1 || true)
    [[ $id == RMCP01 ]] || fail "this disc is '${id:-unreadable}', not RMCP01 (Mario Kart Wii, PAL)"

    mkdir -p "$dest"
    dest=$(cd "$dest" && pwd)
    staging=$dest/DATA.extracting
    rm -rf "$staging" "$dest/.complete"
    t0=$(now)
    step "extracting the data partition (takes a minute or two)"
    "$nod" extract -q "$iso" "$staging"

    local dol=$staging/sys/main.dol rel=$staging/files/rel/StaticR.rel
    [[ -f $dol && -f $rel ]] || { rm -rf "$staging"; fail "nodtool did not produce sys/main.dol and files/rel/StaticR.rel"; }
    [[ $(sha256_of "$dol") == "$dol_pin" ]] || { rm -rf "$staging"; fail "main.dol sha256 differs from the pin; wrong disc revision"; }
    [[ $(sha256_of "$rel") == "$rel_pin" ]] || { rm -rf "$staging"; fail "StaticR.rel sha256 differs from the pin; wrong disc revision"; }

    # Same layout as upstream's Linux setup (DiscTool.cs): DATA/ is kept, dvd_root points at it, and
    # the two translator inputs are copied next to it.
    rm -rf "$dest/DATA"
    mv "$staging" "$dest/DATA"
    cp -f "$dest/DATA/sys/main.dol" "$dest/main.dol"
    cp -f "$dest/DATA/files/rel/StaticR.rel" "$dest/StaticR.rel"
    cat > "$dest/.complete" <<EOT
game_id=RMCP01
nodtool=$NODTOOL_VERSION
main.dol.sha256=$dol_pin
StaticR.rel.sha256=$rel_pin
layout:
  main.dol, StaticR.rel   translator inputs (copies of DATA/sys/main.dol, DATA/files/rel/StaticR.rel)
  DATA/                   nodtool data partition: sys/, files/ (the runtime's [paths] dvd_root)
EOT
    step "extract done in $(elapsed "$t0"): $dest"
}

translator_bin() {
    local t=${KARTFRAME_TRANSLATOR:-}
    if [[ -z $t ]]; then
        for t in "$here/translator/Translator.Cli" "$cache/translator/Translator.Cli"; do [[ -x $t ]] && break; done
    fi
    [[ -x $t ]] || fail "translator not found at $t (run frame/publish-translator.sh, or set KARTFRAME_TRANSLATOR)"
    echo "$t"
}

# The translator resolves recomp.yml's workspace-relative paths (workspace_root ../.., Assets/*,
# projects/mkwii/MAP.txt, runtime/src), so <outdir> is laid out like upstream's workspace root:
# projects/, Assets/ and runtime/ are links, generated/ and build/base/ are the outputs.
cmd_translate() {
    [[ $# -eq 2 ]] || fail "usage: setup.sh translate <assets> <outdir>"
    local assets out tr threads t0 t1 entry
    assets=$(cd "$1" && pwd); mkdir -p "$2"; out=$(cd "$2" && pwd)
    [[ -f $assets/main.dol && -f $assets/StaticR.rel ]] || fail "$assets lacks main.dol / StaticR.rel (run extract first)"
    tr=$(translator_bin)
    threads=${KARTFRAME_THREADS:-$(nproc)}
    rm -rf "$out/generated" "$out/build"
    rm -f "$out/projects" "$out/Assets" "$out/runtime"
    local ws; ws=$(workspace_src)
    ln -s "$ws/projects" "$out/projects"
    ln -s "$assets" "$out/Assets"
    ln -s "$ws/runtime" "$out/runtime"
    local gen=$out/generated functions=$out/generated/functions meta=$out/generated/base_translation_output.json
    local basedir=$out/build/base project=$out/projects/mkwii/recomp.yml
    mkdir -p "$gen" "$basedir"
    entry=$(awk '/^translation:/{t=1} t && /^[[:space:]]*-[[:space:]]*0[xX][0-9a-fA-F]+[[:space:]]*$/{gsub(/[[:space:]-]/,""); print; exit}' "$recomp")
    [[ -n $entry ]] || fail "no translation entry point in $recomp"

    # The sequence of Launcher/local-build.sh, base product only, with the Linux assembly flavour.
    t0=$(now); t1=$t0
    step "translate-recursive $entry"
    "$tr" translate-recursive "$entry" --project "$project" --outdir "$functions" \
        --output-metadata "$meta" --production-source-bundle "$gen/base_translation_sources.bin" \
        --no-function-files --prune-stale --threads "$threads"
    step "translate-recursive: $(elapsed "$t1")"; t1=$(now)
    "$tr" emit-base-manifest --project "$project" --out "$basedir" \
        --functions-dir "$functions" --translation-output-metadata "$meta" --region P
    step "emit-base-manifest: $(elapsed "$t1")"; t1=$(now)
    "$tr" generate-data-init --project "$project" --target-os linux
    step "generate-data-init: $(elapsed "$t1")"; t1=$(now)
    "$tr" emit-build-shards --project "$project" --base-metadata "$meta" \
        --base-functions-dir "$functions" --native-source-dir "$out/runtime/src" --out "$gen/build_shards"
    step "emit-build-shards: $(elapsed "$t1")"
    step "translate done in $(elapsed "$t0"): $out"
}

# ---- the kit --------------------------------------------------------------------------------------

# The fingerprint of a kit: sha256 over (sha256 of kit.json without its "fingerprint" line, sha256 of
# the kit.sha256 file manifest). frame/export-kit.py computes the same value; verify_kit also
# checks every file against the manifest.
kit_fingerprint_of() {
    local k=$1 body man
    body=$(grep -v '^  "fingerprint"' "$k/kit.json" | sha256sum | awk '{print $1}')
    man=$(sha256_of "$k/kit.sha256")
    printf '%s\n%s\n' "$body" "$man" | sha256sum | awk '{print $1}'
}
verify_kit() {
    local k=$1 want got
    want=$(sed -n 's/^  "fingerprint": "\([0-9a-f]*\)".*/\1/p' "$k/kit.json")
    [[ -n $want ]] || fail "$k/kit.json has no fingerprint"
    got=$(kit_fingerprint_of "$k")
    [[ $got == "$want" ]] || fail "the kit's kit.json or kit.sha256 was changed (fingerprint $got, expected $want)"
    # symlinks are listed as "<hash>  <path> -> <target>" and are not checked by sha256sum
    (cd "$k" && grep -v ' -> ' kit.sha256 | sha256sum -c --quiet) || fail "the kit has damaged or missing files; reinstall it"
    echo "$want"
}

toolchain_root() {
    local c
    for c in "${KARTFRAME_TOOLCHAIN:-}" "$here/toolchain" "$here/toolchain/toolchain-aarch64" \
             "$cache/toolchain/toolchain-aarch64"; do
        [[ -n $c && -x $c/bin/clang++ && -x $c/bin/ld.lld && -x $c/bin/ninja && -x $c/bin/cmake ]] && { (cd "$c" && pwd); return; }
    done
    [[ -x $here/toolchain.sh ]] || fail "no toolchain (set KARTFRAME_TOOLCHAIN, or run toolchain.sh)"
    step "making the toolchain (toolchain.sh; downloads about 1.8 GB)" >&2
    "$here/toolchain.sh"
}

# Parallel compiles: each translated shard needs about 2.5 GB at peak, so RAM caps the default 3.
default_jobs() {
    local avail_kb n
    avail_kb=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    n=$(( avail_kb / 2621440 ))
    (( n > 3 )) && n=3
    (( n < 1 )) && n=1
    echo "$n"
}

cmd_build() {
    [[ $# -eq 0 ]] || fail "usage: setup.sh build"
    local kit tc fp jobs out bld t0 t1 tr
    kit=$(kit_dir) || fail "no kit at ${KARTFRAME_KIT:-$here/kit} (set KARTFRAME_KIT)"
    out=$home/build
    [[ -f $out/generated/build_shards/shards.cmake ]] || fail "no translation in $out (run setup.sh translate first)"
    tc=$(toolchain_root)
    t0=$(now)
    step "checking the kit"
    fp=$(verify_kit "$kit")
    jobs=${KARTFRAME_JOBS:-$(default_jobs)}
    bld=$out/kit-build
    step "build: kit ${fp:0:12}, toolchain $("$tc/bin/clang++" --version | sed -n 's/.*version \(\S*\).*/\1/p'), $jobs parallel compiles"
    mkdir -p "$bld"
    "$tc/bin/cmake" -DKIT="$kit" -DWORKSPACE="$out" -DBUILD="$bld" -DTOOLCHAIN="$tc" -DJOBS="$jobs" \
        -P "$kit/kitbuild.cmake" > "$bld/kitbuild.log" 2>&1 || { cat "$bld/kitbuild.log" >&2; fail "kitbuild.cmake failed"; }
    t1=$(now)
    nice -n 5 "$tc/bin/ninja" -C "$bld" -j"$jobs" 2>&1 | tee "$bld/ninja.log" | awk '
        /^\[[0-9]+\/[0-9]+\]/ { n++; if (n % 10 == 0 || /LINK|PCH/) print "[setup] " $1 " " $2 " " $3; fflush() ; next }
        { print }' || fail "compile or link failed; the log is $bld/ninja.log (rerun setup.sh to resume)"
    [[ -x $bld/kartframe ]] || fail "no kartframe was produced; see $bld/ninja.log"
    step "compile and link: $(elapsed "$t1")"

    # Install: the binary, what the game wants beside it, and the record of which kit it was built for.
    mkdir -p "$home/bin"
    cp -f "$bld/kartframe" "$home/bin/kartframe.new" && mv -f "$home/bin/kartframe.new" "$home/bin/kartframe"
    local r
    for r in "$kit"/resources/*; do
        rm -rf "${home:?}/bin/$(basename "$r")"; cp -a "$r" "$home/bin/"
    done
    printf '%s\n' "$fp" > "$home/bin/kartframe.fingerprint"
    tr=$(translator_bin)
    {
        echo "kit_fingerprint=$fp"
        echo "translator_sha256=$(sha256_of "$tr")"
        echo "toolchain=$("$tc/bin/clang++" --version | head -n1)"
        echo "game_id=$(sed -n 's/^game_id=//p' "$home/game/.complete" 2>/dev/null)"
        echo "jobs=$jobs"
        echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "binary_sha256=$(sha256_of "$home/bin/kartframe")"
    } > "$home/bin/kartframe.build.txt"
    step "build done in $(elapsed "$t0"): $home/bin/kartframe"
}

# portable.txt keeps the game's own data (Config.toml, NAND, Cache, Logs) under $home/UserData; the
# Config.toml seed is written only when there is none.
cmd_seed() {
    mkdir -p "$home/UserData" "$home/bin"
    : > "$home/portable.txt"
    [[ -f $home/UserData/Config.toml ]] && return 0
    local seed
    for seed in "$here/Config.frame.toml" "$here/../frame/Config.frame.toml"; do [[ -f $seed ]] && break; done
    if [[ -f $seed ]]; then
        sed "s|@DVD_ROOT@|$home/game/DATA|" "$seed" > "$home/UserData/Config.toml"
    else
        printf '[paths]\ndvd_root = "%s"\n' "$home/game/DATA" > "$home/UserData/Config.toml"
    fi
    step "seeded $home/UserData/Config.toml"
}

cmd_status() {
    local k
    echo "KARTFRAME_HOME  $home"
    [[ -f $home/game/.complete ]] && echo "extract         done ($(sed -n 's/^game_id=//p' "$home/game/.complete"))" || echo "extract         not done"
    [[ -f $home/build/translate.done ]] && echo "translate       done" || echo "translate       not done"
    [[ -x $home/bin/kartframe ]] && echo "build           done (kit $(cut -c1-12 "$home/bin/kartframe.fingerprint" 2>/dev/null))" || echo "build           not done"
    if k=$(kit_dir); then echo "kit             $k ($(sed -n 's/^  "fingerprint": "\(.\{12\}\).*/\1/p' "$k/kit.json"))"; else echo "kit             none"; fi
}

# ---- everything -------------------------------------------------------------------------------------

cmd_all() {
    local force=0
    [[ ${1:-} == --force ]] && { force=1; shift; }
    [[ $# -eq 1 ]] || fail "usage: setup.sh [--force] <iso>"
    local iso=$1 t0 kit fp tr key
    t0=$(now)
    kit=$(kit_dir) || fail "no kit at ${KARTFRAME_KIT:-$here/kit} (set KARTFRAME_KIT)"
    mkdir -p "$home"
    [[ -w $home ]] || fail "cannot write to $home"
    step "KARTFRAME_HOME=$home"
    fp=$(verify_kit "$kit")

    if [[ $force == 0 && -f $home/game/.complete ]]; then
        step "extract: already done ($home/game); use --force to redo"
    else
        cmd_extract "$iso" "$home/game"
    fi

    tr=$(translator_bin)
    key=$(printf '%s\n%s\n%s\n' "$(sha256_of "$tr")" "$(sha256_of "$home/game/.complete")" "$fp" | sha256sum | awk '{print $1}')
    if [[ $force == 0 && -f $home/build/translate.done && $(cat "$home/build/translate.done") == "$key" ]]; then
        step "translate: already done for this kit and translator"
    else
        rm -f "$home/build/translate.done"
        cmd_translate "$home/game" "$home/build"
        printf '%s\n' "$key" > "$home/build/translate.done"
    fi

    if [[ $force == 0 && -x $home/bin/kartframe && $(cat "$home/bin/kartframe.fingerprint" 2>/dev/null) == "$fp" \
          && $(sed -n 's/^binary_sha256=//p' "$home/bin/kartframe.build.txt" 2>/dev/null) == "$(sha256_of "$home/bin/kartframe")" \
          && $home/bin/kartframe -nt $home/build/translate.done ]]; then
        step "build: already done for this kit"
    else
        cmd_build
    fi
    cmd_seed
    step "setup finished in $(elapsed "$t0"). Launch the game from Steam (or $home/bin/kartframe through the wrapper)."
}

case "${1:-}" in
    extract)   shift; cmd_extract "$@" ;;
    translate) shift; cmd_translate "$@" ;;
    build)     shift; cmd_build "$@" ;;
    status)    cmd_status ;;
    seed)      cmd_seed ;;
    ""|-h|--help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    *)         cmd_all "$@" ;;
esac
