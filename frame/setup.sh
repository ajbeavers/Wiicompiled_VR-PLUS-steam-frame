#!/usr/bin/env bash
# KartFrame first-run data setup. Everything here runs on the user's own Frame from the user's own
# disc; nothing derived from the disc is ever stored in the repository or a release.
#
#   setup.sh extract   <iso> <dest>        disc -> <dest>/{DATA,main.dol,StaticR.rel,.complete}
#   setup.sh translate <assets> <outdir>   translator pipeline -> <outdir>/{generated,build/base}
#
# <assets> is the <dest> of extract (it holds main.dol, StaticR.rel and DATA/).
# Environment: KARTFRAME_CACHE (default ~/.cache/kartframe), KARTFRAME_TRANSLATOR (path to the
# published Translator.Cli binary; default <cache>/translator/Translator.Cli),
# KARTFRAME_THREADS (translator threads, default nproc).
set -euo pipefail

NODTOOL_VERSION=v2.0.0-alpha.10   # the version upstream's NodToolProvider.cs pins
NODTOOL_SHA256=b0d94617ed2393334845669cc1f74f1e4fce0d6acb24e8be28c479689a1b907b
NODTOOL_URL=https://github.com/encounter/nod/releases/download/$NODTOOL_VERSION/nodtool-linux-aarch64

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
recomp=$repo/projects/mkwii/recomp.yml
cache=${KARTFRAME_CACHE:-$HOME/.cache/kartframe}

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
    local t=${KARTFRAME_TRANSLATOR:-$cache/translator/Translator.Cli}
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
    ln -s "$repo/projects" "$out/projects"
    ln -s "$assets" "$out/Assets"
    ln -s "$repo/runtime" "$out/runtime"
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

case "${1:-}" in
    extract)   shift; cmd_extract "$@" ;;
    translate) shift; cmd_translate "$@" ;;
    *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
