#!/usr/bin/env bash
# Fail if a directory or tarball holds anything derived from the game disc.
#
#   boundary-check.sh <dir|.tar|.tar.gz|.tgz|.tar.xz> [allowlist-file]
#
# The allowlist holds one path per line (relative, as listed in the tree/tarball, without any
# leading "./" or top-level folder) for files over 1 MB that are allowed (the kit's libraries).
set -euo pipefail

[[ $# -ge 1 ]] || { echo "usage: boundary-check.sh <dir|tarball> [allowlist]" >&2; exit 2; }
input=$1 allow=${2:-/dev/null}
work=
if [[ -d $input ]]; then
    root=$input
else
    work=$(mktemp -d "${TMPDIR:-$HOME/.cache}/kf-boundary.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    tar -xf "$input" -C "$work"
    root=$work
fi

bad=0
flag() { echo "boundary-check: $1: $2" >&2; bad=1; }

while IFS= read -r -d '' f; do
    rel=${f#"$root"/}
    base=${rel##*/}; lower=${base,,}
    case $lower in
        *.dol|*.rel|*.szs|*.brres|*.brstm|*.thp|*.arc|*.u8|*.iso|*.wbfs|*.rvz|*.ciso|*.wia|*.bin.lz|rksys.dat|game.json|*game*.json)
            flag "disc-derived file type" "$rel" ;;
        data_sections_init*|base_translation_*|mkwii_base_manifest*|*_translation_sources.bin|translation-provenance*)
            flag "translator output" "$rel" ;;
    esac
    # Anything under generated/ or functions/, or a translated shard.
    if [[ /$rel == */generated/* || /$rel == */functions/* || /$rel == */build_shards/* ]]; then
        flag "translator output directory" "$rel"
    fi
    size=$(stat -c %s "$f")
    if (( size > 1048576 )) && ! grep -qxF -- "${rel#*/}" "$allow" && ! grep -qxF -- "$rel" "$allow"; then
        flag "over 1 MB and not allowlisted" "$rel"
    fi
done < <(find "$root" \( -type f -o -type l \) -print0)

if (( bad )); then echo "boundary-check: FAILED" >&2; exit 1; fi
echo "boundary-check: ok ($input)"
