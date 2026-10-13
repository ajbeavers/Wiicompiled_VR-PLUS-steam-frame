#!/usr/bin/env bash
# Fail if a directory or tarball holds anything derived from the game disc.
#
#   boundary-check.sh <dir|.tar|.tar.gz|.tgz|.tar.xz> [allowlist-file]
#
# The allowlist holds one shell glob per line (relative path as listed in the tree/tarball, without
# any leading "./" or top-level folder; * also matches "/") for files over 1 MB that are allowed
# (the kit's libraries). frame/kit-allowlist.txt is the kit's. Default: that file when it exists.
set -euo pipefail

[[ $# -ge 1 ]] || { echo "usage: boundary-check.sh <dir|tarball> [allowlist]" >&2; exit 2; }
input=$1 allow=${2:-$(dirname "${BASH_SOURCE[0]}")/kit-allowlist.txt}
[[ -f $allow ]] || allow=/dev/null
work=
if [[ -d $input ]]; then
    root=$input
else
    work=$(mktemp -d "${TMPDIR:-$HOME/.cache}/kf-boundary.XXXXXX")
    trap 'rm -rf "$work"' EXIT
    tar -xf "$input" -C "$work"
    root=$work
fi

# A path (or the path without its top-level folder, for tarballs) matching a glob of the allowlist.
allowed() {
    local rel=$1 pat
    while IFS= read -r pat; do
        [[ -z $pat || $pat == \#* ]] && continue
        # shellcheck disable=SC2053
        [[ $rel == $pat || ${rel#*/} == $pat ]] && return 0
    done < "$allow"
    return 1
}

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
    if (( size > 1048576 )) && ! allowed "$rel"; then
        flag "over 1 MB and not allowlisted" "$rel"
    fi
done < <(find "$root" \( -type f -o -type l \) -print0)

if (( bad )); then echo "boundary-check: FAILED" >&2; exit 1; fi
echo "boundary-check: ok ($input)"
