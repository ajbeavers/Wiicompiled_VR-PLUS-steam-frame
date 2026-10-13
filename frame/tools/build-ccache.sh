#!/bin/bash
# Build ccache from the release source tarball into ~/.local/bin/ccache (no package exists
# for the Frame and the upstream binaries are x86_64 only). Usage: frame/tools/build-ccache.sh [version]
# Then configure the shared cache once with frame/tools/build-ccache.sh --config (also done after install).
set -e
VER=${1:-4.14.1}
[ "$VER" = "--config" ] && VER=""
PREFIX=${PREFIX:-$HOME/.local}
CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache-kf}

configure() {
  mkdir -p "$CCACHE_DIR"
  # base_dir makes paths under the KartFrame tree relative, so the src checkout and every
  # worktree under wt/ hit the same entries. Fixed paths in the build dir name are avoided by
  # keeping the build dir name identical (build-linux) in every worktree.
  cat > "$CCACHE_DIR/ccache.conf" <<CONF
max_size = 20G
base_dir = $HOME/Games/KartFrame
compression = true
compression_level = 3
# Debug info (RelWithDebInfo) embeds the cwd; hash_dir=false lets worktrees share entries.
hash_dir = false
sloppiness = time_macros,include_file_mtime,include_file_ctime,pch_defines
CONF
  echo "ccache config written to $CCACHE_DIR/ccache.conf"
}

if [ -n "$VER" ]; then
  if command -v ccache >/dev/null && [ "$(ccache --version | head -1 | awk '{print $3}')" = "$VER" ]; then
    echo "ccache $VER already installed"
  else
    W=$(mktemp -d "${TMPDIR:-$HOME/.cache}"/kf-ccache-build.XXXXXX)
    trap 'rm -rf "$W"' EXIT
    curl -fsSL -o "$W/src.tar.gz" "https://github.com/ccache/ccache/releases/download/v$VER/ccache-$VER.tar.gz"
    mkdir "$W/src" && tar -xzf "$W/src.tar.gz" -C "$W/src" --strip-components=1
    cmake -S "$W/src" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DENABLE_TESTING=OFF -DREDIS_STORAGE_BACKEND=OFF \
      -DENABLE_DOCUMENTATION=OFF
    nice ninja -j4 -C "$W/build" install
  fi
fi
configure
"$PREFIX/bin/ccache" --version | head -1
