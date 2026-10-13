#!/usr/bin/env bash
# Portable build toolchain for the first-run build: pruned LLVM 22 (clang, lld, llvm-ar), cmake and
# ninja for aarch64, made by upstream's Launcher/prepare-portable-tools.sh. The SteamOS image has
# no compiler, and the ones the maintainer installed by hand are not something users have.
#
#   toolchain.sh [destdir]    prints the toolchain root (<root>/bin/clang ...)
#
# Everything is cached under $KARTFRAME_CACHE/toolchain (default ~/.cache/kartframe/toolchain),
# including upstream's downloads (all sha256-pinned by that script). With destdir, the finished
# toolchain is also hard-linked (or copied) to destdir/toolchain-aarch64.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
cache=${KARTFRAME_CACHE:-$HOME/.cache/kartframe}/toolchain
root=$cache/toolchain-aarch64

if [[ ! -x $root/bin/clang ]]; then
    # Upstream's script keeps its downloads and extraction scratch next to itself
    # (Launcher/artifacts), so run a copy from the cache and keep the repo tree clean.
    mkdir -p "$cache/launcher"
    install -m 0755 "$repo/Launcher/prepare-portable-tools.sh" "$cache/launcher/"
    "$cache/launcher/prepare-portable-tools.sh" --arch aarch64 --destination "$cache" >&2
    rm -rf "$cache/launcher/artifacts/.extract-"*
fi

if [[ $# -ge 1 && -n $1 ]]; then
    mkdir -p "$1"
    dest=$(cd "$1" && pwd)/toolchain-aarch64
    if [[ ! -x $dest/bin/clang ]]; then
        rm -rf "$dest"
        cp -al "$root" "$dest" 2>/dev/null || cp -a "$root" "$dest"
    fi
    root=$dest
fi
echo "$root"
